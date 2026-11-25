/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Default keyboard layout (GOW3_KEY_MAP changes the buttons, see apply_button_map):
 *   WASD left stick, mouse or arrow keys right stick (see mouse_stick), Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad, LCtrl gravity slide (L2+R2),
 *   IJKL d-pad (I up, K down, J left, L right). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#ifdef __SWITCH__
#include "gr_sdl_horizon.h" /* SDL's gamepad API on the console's hid service */
#else
#include <SDL3/SDL.h>
#endif
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with GOW3_PAD_MAP (gamepad) and GOW3_KEY_MAP (keyboard and mouse), e.g.
 * GOW3_KEY_MAP="cross=Q,square=E|Mouse Left" or GOW3_PAD_MAP="cross=b,circle=a": comma-separated
 * <ps4 button>=<names>, '|' between several sources (SDL key names, "Mouse Left", "Mouse Right",
 * "Mouse Middle", "Mouse X1", "Mouse X2"; SDL gamepad button names). A mapped PS4 button loses
 * its default sources; an empty name leaves it without any. Sticks, triggers and the overlay
 * shortcuts are not remapped. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    /* Gravity slide: the game wants L2 and R2 held together; one key or button presses both. */
    {"slide",BTN_L2|BTN_R2},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
/* Mouse buttons in the key map: MOUSE_SOURCE + index in mouse_names (past every scancode). */
#define MOUSE_SOURCE 0x10000
static const struct { const char *name; uint32_t mask; } mouse_names[]={
    {"Mouse Left",SDL_BUTTON_LMASK}, {"Mouse Right",SDL_BUTTON_RMASK}, {"Mouse Middle",SDL_BUTTON_MMASK},
    {"Mouse X1",SDL_BUTTON_X1MASK}, {"Mouse X2",SDL_BUTTON_X2MASK},
};
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static ButtonMap key_map={{
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_LSHIFT,BTN_CIRCLE}, {SDL_SCANCODE_E,BTN_SQUARE},
    {SDL_SCANCODE_Q,BTN_TRIANGLE}, {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
    {SDL_SCANCODE_R,BTN_L2}, {SDL_SCANCODE_F,BTN_R2}, {SDL_SCANCODE_Z,BTN_L3}, {SDL_SCANCODE_C,BTN_R3},
    {SDL_SCANCODE_RETURN,BTN_OPTIONS},
    {SDL_SCANCODE_I,BTN_UP}, {SDL_SCANCODE_K,BTN_DOWN}, {SDL_SCANCODE_J,BTN_LEFT}, {SDL_SCANCODE_L,BTN_RIGHT},
    /* Mouse look: kick, gravity shift, stasis field. */
    {MOUSE_SOURCE+0,BTN_SQUARE}, {MOUSE_SOURCE+1,BTN_R1}, {MOUSE_SOURCE+2,BTN_CIRCLE},
    {SDL_SCANCODE_LCTRL,BTN_L2|BTN_R2}, /* gravity slide */
},19};

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

/* Applies a GOW3_PAD_MAP/GOW3_KEY_MAP value; returns how many entries were used. Bad entries are
 * logged and skipped, so their PS4 button keeps its default. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        /* Its sources, '|' between them; all must be known, or the entry is skipped. */
        int sources[4], count=0;
        bool bad=!ps;
        char names[256];
        snprintf(names,sizeof(names),"%s",name);
        for (char *part=names, *bar; part && !bad; part=bar) {
            bar=strchr(part,'|');
            if (bar) *bar++=0;
            const char *one=trim(part);
            if (!*one) continue;
            int source=-1;
            if (keyboard) {
                for (size_t m=0;m<sizeof(mouse_names)/sizeof(*mouse_names);++m)
                    if (!SDL_strcasecmp(one,mouse_names[m].name)) source=MOUSE_SOURCE+(int)m;
                if (source<0) source=(int)SDL_GetScancodeFromName(one);
                if (source==(int)SDL_SCANCODE_UNKNOWN) source=-1;
            } else {
                source=(int)SDL_GetGamepadButtonFromString(one);
                if (source==(int)SDL_GAMEPAD_BUTTON_INVALID) source=-1;
            }
            if (source<0 || count==4) bad=true;
            else sources[count++]=source;
        }
        if (bad) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        size_t kept=0;
        for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
        map->count=kept;
        for (int i=0;i<count;++i)
            if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){sources[i],ps};
        ++used;
    }
    return used;
}

static void load_button_maps(void) {
    static bool loaded;
    if (loaded) return;
    loaded=true;
    const int pad=apply_button_map(&pad_map,getenv("GOW3_PAD_MAP"),false);
    const int keys=apply_button_map(&key_map,getenv("GOW3_KEY_MAP"),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard buttons remapped\n",pad,keys);
}

/* Mouse look (GOW3_MOUSE_LOOK, default on): while the game window has the focus and no menu of
 * the port is open, the window thread captures the cursor and passes its motion here; the right
 * stick follows the mouse's speed (GOW3_MOUSE_SENS, GOW3_MOUSE_INVERT_Y=1 inverts it). The mouse
 * buttons are sources of the key map (GOW3_KEY_MAP). Without mouse look the left button is a
 * finger on the touchpad, as before, and the other buttons work over the window. */
static long long mouse_dx, mouse_dy; /* motion in 1/256 pixel, window thread -> pad sampling */
static int mouse_captured;
void runtime_pad_mouse_motion(float dx, float dy) {
    __atomic_fetch_add(&mouse_dx,(long long)lrintf(dx*256.0f),__ATOMIC_RELAXED);
    __atomic_fetch_add(&mouse_dy,(long long)lrintf(dy*256.0f),__ATOMIC_RELAXED);
}
void runtime_pad_mouse_capture(int on) { __atomic_store_n(&mouse_captured,on,__ATOMIC_RELAXED); }
/* GOW3_INPUT: 'pad' plays with the controller only, 'keyboard' with the keyboard and mouse only
 * (a connected controller is ignored); unset, all of them work together. */
static int input_mode(void) {
    static int mode=-1;
    if (mode<0) {
        const char *e=getenv("GOW3_INPUT");
        mode=e && !SDL_strcasecmp(e,"pad") ? 1 : e && !SDL_strcasecmp(e,"keyboard") ? 2 : 0;
    }
    return mode;
}
int runtime_pad_mouse_look(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("GOW3_MOUSE_LOOK"); on=!(e && e[0]=='0') && input_mode()!=1; }
    return on;
}
/* Degrees the camera turns per mouse count (the mouse's DPI set the speed; GOW3_MOUSE_DEG). */
#define MOUSE_DEG_PER_COUNT 0.05f
#include "gr_mouse_camera.inc"

/* Mouse speed -> right stick (where the camera cave cannot be installed). The stick is read every frame and can be read more than once a
 * frame, so the motion is measured over at least 8 ms and the last deflection is kept between. */
static void mouse_stick(PadData *d) {
    static uint64_t last;
    static long long acc_x, acc_y;
    static float out_x, out_y;
    static float sens=-1.0f;
    static int invert;
    if (sens<0) {
        const char *e=getenv("GOW3_MOUSE_SENS");
        sens=e && atof(e)>0 ? (float)atof(e) : 1.0f;
        const char *i=getenv("GOW3_MOUSE_INVERT_Y");
        invert=i && i[0]=='1';
    }
    const long long dx=__atomic_exchange_n(&mouse_dx,0,__ATOMIC_RELAXED);
    const long long dy=__atomic_exchange_n(&mouse_dy,0,__ATOMIC_RELAXED);
    if (mouse_camera_drive(dx,dy,invert)) return;
    acc_x+=dx;
    acc_y+=dy;
    const uint64_t now=now_us();
    if (!last) last=now;
    if (now-last>=8000) {
        const float seconds=(float)(now-last)*1e-6f;
        const float vx=(float)acc_x/256.0f/seconds, vy=(float)acc_y/256.0f/seconds; /* pixels per second */
        /* Full deflection at 1500 px/s (sensitivity 1); a small step past the game's dead zone
         * makes slow movements turn the camera too. */
        const float speed=1500.0f/sens;
        out_x=fabsf(vx)<20.0f ? 0.0f : copysignf(fminf(1.0f,0.12f+fabsf(vx)/speed),vx);
        out_y=fabsf(vy)<20.0f ? 0.0f : copysignf(fminf(1.0f,0.12f+fabsf(vy)/speed),vy)*(invert ? -1.0f : 1.0f);
        acc_x=acc_y=0;
        last=now;
    }
    if (out_x!=0.0f) d->right_x=(uint8_t)lrintf(127.5f+out_x*127.5f);
    if (out_y!=0.0f) d->right_y=(uint8_t)lrintf(127.5f+out_y*127.5f);
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}
/* Motion sensors (DualSense / DualShock 4 through SDL, as shadPS4 does): angular velocity in
 * rad/s on the PS4's axes, acceleration in g (SDL's m/s^2 x 0.098), and an orientation
 * quaternion (scePadResetOrientation brings it back to identity). The orientation comes from
 * gr_motion_filter.h: the gyro bias is learned while the controller lies still, and with the
 * game's tilt correction (Gravity Rush asks for it) the accelerometer keeps pitch and roll from
 * drifting; before, the raw integration made the view turn by itself (the apple scene).
 * Sensors are switched on again whenever the gamepad changes (hot-plugging). A gamepad without
 * a gyro uses the PC's own sensors when it has them (handhelds: gr_host_motion.h). */
#include "gr_motion_filter.h"
#include "gr_host_motion.h"
static struct { SDL_Gamepad *pad; int gyro, accel, tilt, host; MotionFilter filter; uint64_t last_us; } motion;
static void motion_reset(void) { motion_filter_reset(&motion.filter); motion.last_us=0; }
/* The orientation the game sees: the filtered one with its rotation angle scaled by the gyro
 * sensitivity (scaling the integration instead would fight the tilt correction). */
static void scaled_orientation(const float q[4], float sens, float out[4]) {
    float w=q[3] < -1.0f ? -1.0f : q[3] > 1.0f ? 1.0f : q[3];
    float s=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]);
    if (s<1e-6f || sens==1.0f) { memcpy(out,q,4*sizeof(float)); return; }
    float half=atan2f(s,w)*sens, k=sinf(half)/s;
    out[0]=q[0]*k; out[1]=q[1]*k; out[2]=q[2]*k; out[3]=cosf(half);
}
static void sample_motion(SDL_Gamepad *g, PadData *d) {
    /* GOW3_GYRO=0 (options.ini gyro=off): the controller reads as lying still. */
    static int gyro_on=-1;
    if (gyro_on<0) { const char *e=getenv("GOW3_GYRO"); gyro_on=!(e && !strcmp(e,"0")); if (!gyro_on) puts("Runtime: motion sensors off (gyro=off)"); }
    if (!gyro_on) return;
    if (g!=motion.pad) {
        motion.pad=g;
        motion.gyro=SDL_GamepadHasSensor(g,SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_GYRO,true);
        motion.accel=SDL_GamepadHasSensor(g,SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_ACCEL,true);
        motion_filter_init(&motion.filter);
        motion.last_us=0;
        printf("Runtime: gamepad motion sensors: gyro %s, accelerometer %s\n",motion.gyro ? "on" : "none",motion.accel ? "on" : "none");
        float probe[3];
        motion.host=!motion.gyro && host_motion_read(probe,NULL);
        if (motion.host) printf("Runtime: system motion sensors instead: %s\n",host_motion_describe());
    }
    float v[3], a[3];
    const int host=motion.host ? host_motion_read(v,a) : 0;
    const int have_accel=host ? host==3 : motion.accel && SDL_GetGamepadSensorData(g,SDL_SENSOR_ACCEL,a,3);
    if (have_accel)
        for (int i=0;i<3;++i) { a[i]*=0.098f; d->acceleration[i]=a[i]; }
    /* GOW3_GYRO_SENS: gyro sensitivity (default 1.4: the player found 1.0 a little slow). */
    static float sens=-1.0f;
    if (sens<0.0f) { const char *e=getenv("GOW3_GYRO_SENS"); sens=e && atof(e)>0.0 ? (float)atof(e) : 1.4f; }
    if (host || (motion.gyro && SDL_GetGamepadSensorData(g,SDL_SENSOR_GYRO,v,3))) {
        uint64_t now=d->timestamp;
        float dt=motion.last_us && now>motion.last_us ? (float)(now-motion.last_us)/1e6f : 0.0f;
        motion.last_us=now;
        float w[3];
        motion_filter_update(&motion.filter,v,have_accel ? a : NULL,motion.tilt,dt,w);
        for (int i=0;i<3;++i) d->angular_velocity[i]=w[i]*sens;
        /* GOW3_MOTION_LOG=1: the filtered orientation and the learned bias every 2 s. */
        static int motion_log=-1;
        static uint64_t next_log;
        if (motion_log<0) motion_log=getenv("GOW3_MOTION_LOG")!=NULL;
        if (motion_log && now>=next_log) {
            const float *q=motion.filter.q, *bias=motion.filter.bias;
            printf("Motion: sticks L %d,%d R %d,%d buttons 0x%x | q %.4f %.4f %.4f %.4f (%.1f deg) bias %.4f %.4f %.4f tilt %d still %.1f s"
                   " gyro %.4f %.4f %.4f accel %.3f %.3f %.3f dt %.4f\n",
                   d->left_x,d->left_y,d->right_x,d->right_y,d->buttons,
                   q[0],q[1],q[2],q[3],2.0f*acosf(fminf(1.0f,fabsf(q[3])))*57.2958f,
                   bias[0],bias[1],bias[2],motion.tilt,motion.filter.still_s,
                   v[0],v[1],v[2],have_accel ? a[0] : 0.0f,have_accel ? a[1] : 0.0f,have_accel ? a[2] : 0.0f,dt);
            next_log=now+500000;
        }
    }
    scaled_orientation(motion.filter.q,sens,d->orientation);
}
static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    mouse_camera_test();
    SDL_Gamepad *g=input_mode()==2 ? NULL : current_gamepad();
    if (gow3gpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    load_button_maps();
    if (g) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        sample_motion(g,d);
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    }
    /* The keyboard works with or without a gamepad: its buttons add to the pad's, and a stick
     * follows the keys only while one of them is held. */
    if (!k || input_mode()==1) return;
    /* The mouse buttons held: all while the cursor is captured (mouse look); over the window
     * without mouse look, all but the left one (a finger on the touchpad there). */
    const bool captured=runtime_pad_mouse_look() && __atomic_load_n(&mouse_captured,__ATOMIC_RELAXED);
    SDL_MouseButtonFlags mouse_held=0;
    if (captured) mouse_held=SDL_GetMouseState(NULL,NULL);
    else if (!runtime_pad_mouse_look() && SDL_GetMouseFocus()) mouse_held=SDL_GetMouseState(NULL,NULL) & ~SDL_BUTTON_LMASK;
    for (size_t i=0;i<key_map.count;++i) {
        const int source=key_map.entries[i].source;
        const bool held=source>=MOUSE_SOURCE ? (mouse_held & mouse_names[source-MOUSE_SOURCE].mask)!=0 : k[source];
        if (held) d->buttons|=key_map.entries[i].ps;
    }
    if (k[SDL_SCANCODE_TAB]) touch_click(d,0);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0); /* a remapped touchpad key */
    if (k[SDL_SCANCODE_BACKSPACE]) touch_click(d,1);
    if (captured) {
        mouse_stick(d);
    } else {
        mouse_camera_release();
    }
    if (!captured && !runtime_pad_mouse_look()) {
        /* Left mouse button held over the window is a finger at the pointer, so dragging is a swipe. */
        SDL_Window *mw=SDL_GetMouseFocus();
        float mx=0, my=0;
        int ww=0, wh=0;
        if (mw && (SDL_GetMouseState(&mx,&my) & SDL_BUTTON_LMASK) && SDL_GetWindowSize(mw,&ww,&wh) && ww>0 && wh>0) {
            d->buttons|=BTN_TOUCHPAD;
            d->touch_count=1;
            d->touches[0]=(PadTouch){.x=touch_axis(mx/ww,1919),.y=touch_axis(my/wh,942),.id=0};
        }
    }
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0); /* a mouse button */
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,k[SDL_SCANCODE_A],k[SDL_SCANCODE_D]);
    key_axis(&d->left_y,k[SDL_SCANCODE_W],k[SDL_SCANCODE_S]);
    key_axis(&d->right_x,k[SDL_SCANCODE_LEFT],k[SDL_SCANCODE_RIGHT]);
    key_axis(&d->right_y,k[SDL_SCANCODE_UP],k[SDL_SCANCODE_DOWN]);
}

/* GOW3_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a GOW3_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("GOW3_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* GOW3_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* GOW3_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; GOW3_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in GOW3_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("GOW3_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("GOW3_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        /* GOW3_PAD_REPLAY_SKIP_MS: start that far into the recording (a run that reaches the same
         * point of the game another way, e.g. by the autotest's apple taps). */
        const char *skip=getenv("GOW3_PAD_REPLAY_SKIP_MS");
        replay_start=now_us()-(skip ? (uint64_t)strtoull(skip,NULL,10)*1000 : 0);
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
/* Buttons of the gamepad and keyboard as mapped, even while the overlay holds the game neutral. */
static uint32_t host_buttons(void) {
    uint32_t buttons=0;
    load_button_maps();
    SDL_Gamepad *g=current_gamepad();
    if (g) for (size_t i=0;i<pad_map.count;++i)
        if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) buttons|=pad_map.entries[i].ps;
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (k) for (size_t i=0;i<key_map.count;++i) if (k[key_map.entries[i].source]) buttons|=key_map.entries[i].ps;
    return buttons;
}
/* Buttons for the port's save list (sceSaveDataDialog), polled by the overlay every frame while
 * it is open: the game stops reading the pad during the dialog. Gamepad and keyboard as mapped,
 * scripted input, and the left stick as up/down. */
static uint32_t list_buttons(void) {
    pthread_mutex_lock(&lock);
    read_inject();
    uint32_t buttons=host_buttons()|injected.buttons;
    SDL_Gamepad *g=current_gamepad();
    if (g) {
        const int y=SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY);
        if (y<-16000) buttons|=BTN_UP;
        else if (y>16000) buttons|=BTN_DOWN;
    }
    if (injected.stick[1]>=0 && injected.stick[1]<64) buttons|=BTN_UP;
    else if (injected.stick[1]>192) buttons|=BTN_DOWN;
    pthread_mutex_unlock(&lock);
    return buttons;
}
/* Like the real pad (and shadPS4): every new finger gets a fresh id 1..127, kept while it stays
 * down, and touch_held_time counts microseconds since the first finger went down. */
static void assign_touch_ids(PadData *d) {
    static uint8_t slot_id[2], next_id=1;
    static uint64_t down_since;
    if (d->buttons==0 && hold_after_capture) d->touch_count=0;
    for (int i=0;i<2;++i) {
        if (i>=d->touch_count) { slot_id[i]=0; continue; }
        if (!slot_id[i]) { slot_id[i]=next_id; next_id=next_id==127 ? 1 : next_id+1; }
        d->touches[i].id=slot_id[i];
    }
    if (!d->touch_count) down_since=0;
    else if (!down_since) down_since=d->timestamp;
    d->touch_held_time=down_since ? (uint32_t)(d->timestamp-down_since) : 0;
}
/* GOW3_MOTION_FAKE=qx,qy,qz,qw[,ax,ay,az]: a fixed orientation (and acceleration in g), and
 * GOW3_MOTION_FAKE_GYRO=wx,wy,wz a fixed angular velocity (rad/s), integrated into the orientation
 * when no fixed one is given, from GOW3_MOTION_FAKE_AFTER seconds on, with or without a gamepad
 * (diagnostics: what the game does with the motion sensors). */
static void fake_motion(PadData *d) {
    static int parsed, count, gyro_count;
    static float v[7], w[3];
    static uint64_t from;
    if (!parsed) {
        parsed=1;
        const char *e=getenv("GOW3_MOTION_FAKE"), *g=getenv("GOW3_MOTION_FAKE_GYRO");
        const char *after=getenv("GOW3_MOTION_FAKE_AFTER");
        if (e) count=sscanf(e,"%f,%f,%f,%f,%f,%f,%f",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6]);
        if (g) gyro_count=sscanf(g,"%f,%f,%f",&w[0],&w[1],&w[2]);
        from=now_us()+(uint64_t)((after ? atof(after) : 0.0)*1e6);
    }
    if ((count<4 && gyro_count<3) || now_us()<from) return;
    static int logged;
    if (!logged++) printf("Runtime: GOW3_MOTION_FAKE from now on\n");
    if (count>=4) memcpy(d->orientation,v,4*sizeof(float));
    if (count>=7) memcpy(d->acceleration,v+4,3*sizeof(float));
    if (gyro_count>=3) memcpy(d->angular_velocity,w,3*sizeof(float));
    if (gyro_count>=3 && count<4) {
        static float q[4]={0,0,0,1};
        static uint64_t last;
        const float dt=last && d->timestamp>last ? (float)(d->timestamp-last)/1e6f : 0.0f;
        last=d->timestamp;
        if (dt>0.0f && dt<1.0f) {
            const float dq[4]={q[3]*w[0]+q[1]*w[2]-q[2]*w[1], q[3]*w[1]+q[2]*w[0]-q[0]*w[2],
                               q[3]*w[2]+q[0]*w[1]-q[1]*w[0], -q[0]*w[0]-q[1]*w[1]-q[2]*w[2]};
            for (int i=0;i<4;++i) q[i]+=0.5f*dq[i]*dt;
            const float n=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
            if (n>0.0f) for (int i=0;i<4;++i) q[i]/=n;
        }
        memcpy(d->orientation,q,sizeof(q));
    }
}

/* Movement diagnostics: the ground camera (PlayerCamera, eboot 0x98440, the update that calls
 * 0x986b0 and 0x98b90) is entered with the camera in rdi. Its first bytes jump to a cave in the
 * free tail of the code segment that keeps that pointer and counts the runs in the free tail of the
 * data segment, then goes on. The camera's position is at +0x10, the point it looks at (by the
 * player) at +0x170, the turning speeds pitch +0x220 and yaw +0x224 (gr_mouse_camera.inc). The
 * game's input state (eboot 0x912d0 returns it) holds the values the cameras read at +0x3c0..
 * Twice a second while a stick is off centre, one "Move:" line: the sticks as sent, the motion
 * data, the game's input values and the camera. GOW3_MOVE_LOG=0 turns it off. */
#define PROBE_ENTRY 0x98440u
#define PROBE_RESUME 0x98446u
#define PROBE_CAVE 0x15e7900u  /* the code segment ends at 0x15e785c, its page at 0x15e8000 */
#define PROBE_SLOT 0x23b7700u  /* the data segment ends at 0x23b7668, its page at 0x23b8000 */
#define PROBE_INPUT 0x1ed53b0u
#define GRAVITY_ENTRY 0x926b0u
#define GRAVITY_CAVE (PROBE_CAVE+0x40)
static unsigned char *probe_image;
void runtime_camera_probe_install(unsigned char *image, size_t size) {
    static const unsigned char entry[6]={0x55,0x48,0x89,0xe5,0x41,0x57}; /* push rbp; mov rbp,rsp; push r15 */
    const char *e=getenv("GOW3_MOVE_LOG");
    if (e && !strcmp(e,"0")) return;
    if (size<PROBE_SLOT+16 || memcmp(image+PROBE_ENTRY,entry,6)) { puts("Move: camera probe not installed (another build of the game)"); return; }
    for (unsigned i=0;i<0x60;++i) if (image[PROBE_CAVE+i] || (i<16 && image[PROBE_SLOT+i])) { puts("Move: camera probe not installed (no free space)"); return; }
    unsigned char *c=image+PROBE_CAVE;
    int32_t rel;
    c[0]=0x48; c[1]=0x89; c[2]=0x3d; rel=(int32_t)(PROBE_SLOT-(PROBE_CAVE+7)); memcpy(c+3,&rel,4);      /* mov [slot], rdi */
    c[7]=0xf0; c[8]=0xff; c[9]=0x05; rel=(int32_t)(PROBE_SLOT+8-(PROBE_CAVE+14)); memcpy(c+10,&rel,4);  /* lock inc dword [slot+8] */
    memcpy(c+14,entry,6);
    c[20]=0xe9; rel=(int32_t)(PROBE_RESUME-(PROBE_CAVE+25)); memcpy(c+21,&rel,4);                       /* jmp back */
    unsigned char *at=image+PROBE_ENTRY;
    at[0]=0xe9; rel=(int32_t)(PROBE_CAVE-(PROBE_ENTRY+5)); memcpy(at+1,&rel,4); at[5]=0x90;
    /* The gravity-shift camera (0x926b0, while Kat floats): its runs only (slot+12), so
     * runtime_pad_gameplay() knows a gameplay camera is in use in the air too. */
    static const unsigned char gravity_entry[6]={0x55,0x48,0x89,0xe5,0x41,0x56}; /* push rbp; mov rbp,rsp; push r14 */
    if (!memcmp(image+GRAVITY_ENTRY,gravity_entry,6)) {
        unsigned char *g=image+GRAVITY_CAVE;
        g[0]=0xf0; g[1]=0xff; g[2]=0x05; rel=(int32_t)(PROBE_SLOT+12-(GRAVITY_CAVE+7)); memcpy(g+3,&rel,4); /* lock inc dword [slot+12] */
        memcpy(g+7,gravity_entry,6);
        g[13]=0xe9; rel=(int32_t)(GRAVITY_ENTRY+6-(GRAVITY_CAVE+18)); memcpy(g+14,&rel,4);
        unsigned char *ga=image+GRAVITY_ENTRY;
        ga[0]=0xe9; rel=(int32_t)(GRAVITY_CAVE-(GRAVITY_ENTRY+5)); memcpy(ga+1,&rel,4); ga[5]=0x90;
    }
    probe_image=image;
    puts("Move: camera probe installed");
}
/* A gameplay camera (on the ground or floating) ran in the last 300 ms: menus, pauses and
 * cut-scenes have neither. Called at each pad sample. */
static int gameplay_now;
static void update_gameplay(void) {
    static uint32_t last_ground, last_gravity;
    static uint64_t last_seen;
    if (!probe_image) { gameplay_now=1; return; }
    uint32_t ground, gravity;
    memcpy(&ground,probe_image+PROBE_SLOT+8,4);
    memcpy(&gravity,probe_image+PROBE_SLOT+12,4);
    const uint64_t now=now_us();
    if (ground!=last_ground || gravity!=last_gravity) last_seen=now;
    last_ground=ground; last_gravity=gravity;
    gameplay_now=last_seen && now-last_seen<300000;
}
int runtime_pad_gameplay(void) { return gameplay_now; }
/* GOW3_MENU_SWAP=1 (options.ini swap_ab=1): outside gameplay the Switch's A sends the PS4's cross
 * (the European release's confirm) and B its circle, so menus confirm with A; in gameplay every
 * button stays by position (B is cross: jump). The prompts follow (gow3_button_glyphs.cpp). */
int runtime_pad_menu_swapped(void) {
    static int on=-1;
    if (on<0) { const char *e=getenv("GOW3_MENU_SWAP"); on=e && e[0]=='1'; }
    return on && !gameplay_now;
}
#if defined(__APPLE__)
/* GOW3_FIND_KAT=1 (macOS host, diagnostics): at the first line with the camera in use, finds the
 * guest memory holding the camera's look-at point (Kat's position) and from then on appends
 * 0x800 bytes around each match to out/katdump.bin at every line, to find the fields of her
 * movement. Record: f64 seconds, u32 match, u32 0, u64 address, then the bytes. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#define KAT_SPAN 0x800u
static uintptr_t kat_hits[16];
static int kat_count=-1;
static FILE *kat_file;
static void kat_find(const float *t) {
    kat_count=0;
    mach_vm_address_t a=0x7000000000ull;
    while (a<0x8400000000ull && kat_count<16) {
        mach_vm_size_t size; vm_region_basic_info_data_64_t info; mach_msg_type_number_t n=VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
        if (mach_vm_region(mach_task_self(),&a,&size,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&n,&obj)!=KERN_SUCCESS) break;
        if ((info.protection&3)==3 && a+size<=0x8400000000ull)
            for (const float *f=(const float *)(uintptr_t)a, *e=(const float *)(uintptr_t)(a+size)-3; f<e && kat_count<16; f++)
                if (fabsf(f[0]-t[0])<0.02f && fabsf(f[2]-t[2])<0.02f && fabsf(f[1]-t[1])<3.0f && (uintptr_t)f>=0x7000000000ull+KAT_SPAN/2) {
                    kat_hits[kat_count++]=(uintptr_t)f;
                    printf("Kat: match %d at 0x%llx: %.3f %.3f %.3f\n",kat_count-1,(unsigned long long)(uintptr_t)f,f[0],f[1],f[2]);
                }
        a+=size;
    }
    kat_file=fopen("out/katdump.bin","wb");
    printf("Kat: %d matches\n",kat_count);
}
static void kat_dump(void) {
    if (!kat_file) return;
    const double s=(double)now_us()/1e6;
    for (int i=0;i<kat_count;i++) {
        const uint32_t hdr[2]={(uint32_t)i,0}; const uint64_t at=kat_hits[i]-KAT_SPAN/2;
        fwrite(&s,8,1,kat_file); fwrite(hdr,4,2,kat_file); fwrite(&at,8,1,kat_file);
        fwrite((const void *)(uintptr_t)at,1,KAT_SPAN,kat_file);
    }
    fflush(kat_file);
}
#endif
static void move_log(const PadData *d) {
    static uint64_t next;
    static uint32_t last_runs;
    static int fast=-1;
    if (fast<0) { const char *e=getenv("GOW3_MOVE_LOG"); fast=e && !strcmp(e,"fast"); }
    if (!probe_image) return;
    const uint64_t now=now_us();
    /* Each change of the buttons sent (fast log): whether jump (cross) reaches the game. */
    static uint32_t last_buttons;
    if (fast && d->buttons!=last_buttons) {
        printf("Move: buttons 0x%x%s%s%s%s%s\n",d->buttons,d->buttons & BTN_CROSS ? " cross" : "",d->buttons & BTN_CIRCLE ? " circle" : "",
               d->buttons & BTN_SQUARE ? " square" : "",d->buttons & BTN_TRIANGLE ? " triangle" : "",gameplay_now ? "" : " (not gameplay)");
        last_buttons=d->buttons;
    }
    if (now<next) return;
    if (abs(d->left_x-128)<24 && abs(d->left_y-128)<24 && abs(d->right_x-128)<24 && abs(d->right_y-128)<24 && !(fast && d->buttons)) return;
    next=now+(fast ? 100000 : 500000);
#ifdef __SWITCH__
    /* The console's own stick readings, before any scaling (src/gr_sdl_horizon.h). */
    if (fast) printf("Move: raw L %d,%d R %d,%d style 0x%llx%s\n",(int)sdl_horizon_pad.sticks[0].x,(int)sdl_horizon_pad.sticks[0].y,
                     (int)sdl_horizon_pad.sticks[1].x,(int)sdl_horizon_pad.sticks[1].y,(unsigned long long)sdl_horizon_pad.style,
                     gameplay_now ? "" : " (not gameplay)");
#endif
    const float *in=(const float *)(probe_image+PROBE_INPUT+0x3b0);
    char camera[220]="camera: not in use";
    uint32_t runs; memcpy(&runs,probe_image+PROBE_SLOT+8,4);
    if (runs!=last_runs) {
        /* Ran since the last line, so the object is alive. */
        uintptr_t cam; memcpy(&cam,probe_image+PROBE_SLOT,8);
        const float *p=(const float *)(cam+0x10), *t=(const float *)(cam+0x170), *turn=(const float *)(cam+0x220);
        const float heading=atan2f(t[0]-p[0],t[2]-p[2])*57.2958f;
        snprintf(camera,sizeof camera,"camera %.2f %.2f %.2f look-at %.2f %.2f %.2f heading %.1f deg turn pitch %.3f yaw %.3f (%u runs)",
                 p[0],p[1],p[2],t[0],t[1],t[2],heading,turn[0],turn[1],runs-last_runs);
#if defined(__APPLE__)
        static int find=-1;
        if (find<0) { const char *e=getenv("GOW3_FIND_KAT"); find=e && e[0]=='1'; }
        if (find && kat_count<0 && (abs(d->left_x-128)>=24 || abs(d->left_y-128)>=24)) kat_find(t);
        if (find) kat_dump();
#endif
    }
    last_runs=runs;
    printf("Move: sticks L %d,%d R %d,%d | q %.3f %.3f %.3f %.3f w %.2f %.2f %.2f | game input %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f | %s\n",
           d->left_x,d->left_y,d->right_x,d->right_y,d->orientation[0],d->orientation[1],d->orientation[2],d->orientation[3],
           d->angular_velocity[0],d->angular_velocity[1],d->angular_velocity[2],
           in[0],in[1],in[2],in[3],in[4],in[5],in[6],in[7],camera);
}
/* The movement tests (GOW3_MOVE_LOG=fast): one line on screen (gpu/shim/gow3_overlay.cpp) with
 * the left stick as the console reads it and as the game gets it, and the buttons sent, so a
 * push can be checked against the thumb while playing. */
static PadData shown_sample;
static int shown_valid;
static const char *stick_way(double x, double y) {
    static const char *const names[8]={"right","up-right","up","up-left","left","down-left","down","down-right"};
    if (sqrt(x*x+y*y)<0.25) return "centre";
    const double deg=atan2(y,x)*57.29578;
    return names[((int)floor((deg+360.0+22.5)/45.0))%8];
}
int runtime_pad_stick_text(char *out, size_t size) {
    static int on=-1;
    if (on<0) { const char *e=getenv("GOW3_MOVE_LOG"); on=e && !strcmp(e,"fast"); }
    if (!on || !shown_valid) return 0;
    PadData d;
    pthread_mutex_lock(&lock); d=shown_sample; pthread_mutex_unlock(&lock);
    char raw[96]="";
#ifdef __SWITCH__
    const HidAnalogStickState r=sdl_horizon_pad.sticks[0];
    snprintf(raw,sizeof raw,"Stick read: %s (%d, %d)\n",stick_way(r.x/32767.0,r.y/32767.0),(int)r.x,(int)r.y);
#endif
    snprintf(out,size,"%sGame gets: %s (%d, %d)%s\nButtons: %s%s%s%s%s",raw,
             stick_way((d.left_x-128)/127.0,(128-d.left_y)/127.0),d.left_x,d.left_y,gameplay_now ? "" : " (not gameplay)",
             d.buttons & BTN_CROSS ? "cross " : "",d.buttons & BTN_CIRCLE ? "circle " : "",
             d.buttons & BTN_SQUARE ? "square " : "",d.buttons & BTN_TRIANGLE ? "triangle " : "",
             d.buttons & (BTN_CROSS|BTN_CIRCLE|BTN_SQUARE|BTN_TRIANGLE) ? "" : "none");
    return 1;
}
static void sample(PadData *d) {
    sample_host(d);
    fake_motion(d);
    if (gow3gpu_overlay_captures_input()) { hold_after_capture=1; return; }
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
    assign_touch_ids(d);
    update_gameplay();
    if (runtime_pad_menu_swapped()) {
        const uint32_t cross=d->buttons & BTN_CROSS, circle=d->buttons & BTN_CIRCLE;
        d->buttons=(d->buttons & ~(uint32_t)(BTN_CROSS|BTN_CIRCLE)) | (cross ? BTN_CIRCLE : 0) | (circle ? BTN_CROSS : 0);
    }
    move_log(d);
    shown_sample=*d; shown_valid=1;
}

static ABI int32_t pad_init(void) {
    pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock);
    gow3gpu_choice_set_input(list_buttons);
    return 0;
}
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }
/* The game's motion sensor settings: the tilt correction is applied (gr_motion_filter.h). */
static ABI int32_t pad_deadband_state(int32_t handle, uint8_t flag) {
    static int logged;
    if (!logged++) printf("Runtime: scePadSetAngularVelocityDeadbandState(%d)\n",flag);
    return pad_ok_handle(handle);
}
static ABI int32_t pad_tilt_correction_state(int32_t handle, uint8_t flag) {
    static int logged;
    if (!logged++) printf("Runtime: scePadSetTiltCorrectionState(%d)\n",flag);
    int32_t r=pad_ok_handle(handle);
    if (!r) { pthread_mutex_lock(&lock); motion.tilt=flag!=0; pthread_mutex_unlock(&lock); }
    return r;
}
static ABI int32_t pad_reset_orientation(int32_t handle) {
    static int logged;
    if (logged++<5) printf("Runtime: scePadResetOrientation\n");
    int32_t r=pad_ok_handle(handle);
    if (!r) { pthread_mutex_lock(&lock); motion_reset(); pthread_mutex_unlock(&lock); }
    return r;
}

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
/* scePadGetHandle: the handle scePadOpen returned for this user's standard port. */
static ABI int32_t pad_get_handle(int32_t user, int32_t type, int32_t index) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1 || (type!=0 && type!=2) || index) return ERR_INVALID_ARG;
    return opened ? PAD_HANDLE : (int32_t)0x80920008; /* SCE_PAD_ERROR_NO_HANDLE */
}
static const RuntimeExport exports[]={
    {"scePadGetHandle",pad_get_handle},
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_reset_orientation},
    {"scePadSetAngularVelocityDeadbandState",pad_deadband_state}, {"scePadSetTiltCorrectionState",pad_tilt_correction_state},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
