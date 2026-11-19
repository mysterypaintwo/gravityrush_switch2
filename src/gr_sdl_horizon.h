/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The part of SDL3's gamepad API that runtime_pad.c uses, on the Switch 2's hid service (there is
 * no SDL there). runtime_pad.c includes it instead of <SDL3/SDL.h> on __SWITCH__, so the pad code
 * (button maps, the motion filter, touch, scripted input) is the same as on the PC.
 *
 * One gamepad: the console in handheld mode, or controller 1 (Pro Controller, a Joy-Con pair).
 * Buttons by position, as the prompts show them (gpu/shim/gow3_button_glyphs.cpp): B is SOUTH
 * (the game's ✕), A is EAST (○), Y is WEST (□), X is NORTH (△); - is BACK (the touch pad
 * click), + is START (OPTIONS). ZL/ZR are digital: full trigger while held.
 *
 * Motion: SDL reports a DualSense in rad/s and m/s^2 with x right, y up, z towards the player.
 * hid's six-axis state is in revolutions/s and g with x right, y away from the player, z up,
 * and reads gravity as -z when the controller lies flat; the conversion below gives the SDL
 * frame, so the runtime's DualSense path (gr_motion_filter.h) handles Switch controllers too.
 * The sensor: the console in handheld mode, the Pro Controller, the right Joy-Con of a pair.
 *
 * The handheld touch screen is the gamepad's touch pad (1280x720 -> 0..1). No keyboard or mouse:
 * their functions report nothing, so those code paths stay idle. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

typedef uint32_t SDL_JoystickID;
typedef uint32_t SDL_MouseButtonFlags;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Gamepad {
    PadState pad;
    HidSixAxisSensorHandle handheld, fullkey, joydual[2];
    int ready;
    uint64_t buttons, style;
    HidAnalogStickState sticks[2];
} SDL_Gamepad;

#define SDL_INIT_AUDIO 0x10u
#define SDL_INIT_VIDEO 0x20u
#define SDL_INIT_GAMEPAD 0x2000u
typedef enum {
    SDL_GAMEPAD_BUTTON_INVALID = -1,
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    SDL_GAMEPAD_BUTTON_TOUCHPAD, SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;
typedef enum {
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
} SDL_GamepadAxis;
typedef enum { SDL_SENSOR_ACCEL = 1, SDL_SENSOR_GYRO = 2 } SDL_SensorType;
/* Keyboard scancodes the key map names (no keyboard on the console: never pressed). */
enum {
    SDL_SCANCODE_UNKNOWN = 0, SDL_SCANCODE_A = 4, SDL_SCANCODE_C = 6, SDL_SCANCODE_D = 7,
    SDL_SCANCODE_E = 8, SDL_SCANCODE_F = 9, SDL_SCANCODE_I = 12, SDL_SCANCODE_J = 13,
    SDL_SCANCODE_K = 14, SDL_SCANCODE_L = 15, SDL_SCANCODE_Q = 20, SDL_SCANCODE_R = 21,
    SDL_SCANCODE_S = 22, SDL_SCANCODE_W = 26, SDL_SCANCODE_Z = 29, SDL_SCANCODE_1 = 30,
    SDL_SCANCODE_3 = 32, SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_BACKSPACE = 42, SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44, SDL_SCANCODE_F9 = 66, SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82, SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT = 225,
};
#define SDL_BUTTON_LMASK 1u
#define SDL_BUTTON_MMASK 2u
#define SDL_BUTTON_RMASK 4u
#define SDL_BUTTON_X1MASK 8u
#define SDL_BUTTON_X2MASK 16u

#define SDL_strcasecmp strcasecmp
#define SDL_free free

static SDL_Gamepad sdl_horizon_pad;

static inline bool SDL_InitSubSystem(uint32_t flags) { (void)flags; return true; }
static inline uint32_t SDL_WasInit(uint32_t flags) { return flags & (SDL_INIT_GAMEPAD | SDL_INIT_AUDIO); }

static void sdl_horizon_init(SDL_Gamepad* g)
{
    if (g->ready) return;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g->pad);
    hidInitializeTouchScreen();
    hidGetSixAxisSensorHandles(&g->handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&g->fullkey, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(g->joydual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    hidStartSixAxisSensor(g->handheld);
    hidStartSixAxisSensor(g->fullkey);
    hidStartSixAxisSensor(g->joydual[0]);
    hidStartSixAxisSensor(g->joydual[1]);
    g->ready = 1;
}

/* Reads the controller once per pad sample (runtime_pad.c asks whether it is connected first). */
static inline bool SDL_GamepadConnected(SDL_Gamepad* g)
{
    sdl_horizon_init(g);
    padUpdate(&g->pad);
    g->buttons = padGetButtons(&g->pad);
    g->style = padGetStyleSet(&g->pad);
    g->sticks[0] = padGetStickPos(&g->pad, 0);
    g->sticks[1] = padGetStickPos(&g->pad, 1);
    return padIsConnected(&g->pad);
}

static inline SDL_JoystickID* SDL_GetGamepads(int* count)
{
    *count = SDL_GamepadConnected(&sdl_horizon_pad) ? 1 : 0;
    SDL_JoystickID* ids = malloc(sizeof *ids);
    if (ids) ids[0] = 1;
    return ids;
}
static inline SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id) { (void)id; return &sdl_horizon_pad; }
static inline void SDL_CloseGamepad(SDL_Gamepad* g) { (void)g; }
static inline const char* SDL_GetGamepadName(SDL_Gamepad* g)
{
    if (!g) return "none";
    if (g->style & HidNpadStyleTag_NpadHandheld) return "Switch 2 (handheld)";
    if (g->style & HidNpadStyleTag_NpadFullKey) return "Pro Controller";
    if (g->style & HidNpadStyleTag_NpadJoyDual) return "Joy-Con pair";
    return "Switch controller";
}

static inline bool SDL_GetGamepadButton(SDL_Gamepad* g, SDL_GamepadButton b)
{
    static const uint64_t hid[SDL_GAMEPAD_BUTTON_COUNT] = {
        [SDL_GAMEPAD_BUTTON_SOUTH] = HidNpadButton_B, [SDL_GAMEPAD_BUTTON_EAST] = HidNpadButton_A,
        [SDL_GAMEPAD_BUTTON_WEST] = HidNpadButton_Y, [SDL_GAMEPAD_BUTTON_NORTH] = HidNpadButton_X,
        [SDL_GAMEPAD_BUTTON_BACK] = HidNpadButton_Minus, [SDL_GAMEPAD_BUTTON_START] = HidNpadButton_Plus,
        [SDL_GAMEPAD_BUTTON_LEFT_STICK] = HidNpadButton_StickL, [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = HidNpadButton_StickR,
        [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = HidNpadButton_L, [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = HidNpadButton_R,
        [SDL_GAMEPAD_BUTTON_DPAD_UP] = HidNpadButton_Up, [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = HidNpadButton_Down,
        [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = HidNpadButton_Left, [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = HidNpadButton_Right,
    };
    if (!g || b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT) return false;
    /* options.ini swap_ab (GOW3_BUTTON_LABELS=swap_ab): A is the PS4's cross, B its circle. */
    static int swap_ab = -1;
    if (swap_ab < 0) { const char* e = getenv("GOW3_BUTTON_LABELS"); swap_ab = e && !strcmp(e, "swap_ab"); }
    uint64_t want = hid[b];
    if (swap_ab && b == SDL_GAMEPAD_BUTTON_SOUTH) want = HidNpadButton_A;
    else if (swap_ab && b == SDL_GAMEPAD_BUTTON_EAST) want = HidNpadButton_B;
    return want && (g->buttons & want);
}

static inline int16_t SDL_GetGamepadAxis(SDL_Gamepad* g, SDL_GamepadAxis a)
{
    if (!g) return 0;
    const int32_t y_inverted[2] = {-(int32_t)g->sticks[0].y, -(int32_t)g->sticks[1].y}; /* SDL: up is negative */
    int32_t v = 0;
    switch (a) {
    case SDL_GAMEPAD_AXIS_LEFTX: v = g->sticks[0].x; break;
    case SDL_GAMEPAD_AXIS_LEFTY: v = y_inverted[0]; break;
    case SDL_GAMEPAD_AXIS_RIGHTX: v = g->sticks[1].x; break;
    case SDL_GAMEPAD_AXIS_RIGHTY: v = y_inverted[1]; break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: v = (g->buttons & HidNpadButton_ZL) ? 32767 : 0; break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: v = (g->buttons & HidNpadButton_ZR) ? 32767 : 0; break;
    }
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* --- motion */
static inline bool SDL_GamepadHasSensor(SDL_Gamepad* g, SDL_SensorType type) { (void)type; return g != NULL; }
static inline bool SDL_SetGamepadSensorEnabled(SDL_Gamepad* g, SDL_SensorType type, bool on) { (void)type; (void)on; return g != NULL; }
static inline bool SDL_GetGamepadSensorData(SDL_Gamepad* g, SDL_SensorType type, float* data, int n)
{
    if (!g || n < 3) return false;
    HidSixAxisSensorHandle h;
    if (g->style & HidNpadStyleTag_NpadHandheld) h = g->handheld;
    else if (g->style & HidNpadStyleTag_NpadFullKey) h = g->fullkey;
    else if (g->style & HidNpadStyleTag_NpadJoyDual) h = g->joydual[1];
    else return false;
    HidSixAxisSensorState s;
    memset(&s, 0, sizeof s);
    if (hidGetSixAxisSensorStates(h, &s, 1) != 1 || !s.sampling_number) return false;
    if (type == SDL_SENSOR_GYRO) {
        const float k = 6.28318531f; /* revolutions/s -> rad/s */
        data[0] = s.angular_velocity.x * k;
        data[1] = s.angular_velocity.z * k;
        data[2] = -s.angular_velocity.y * k;
    } else {
        const float g0 = 9.80665f; /* g -> m/s^2 */
        data[0] = -s.acceleration.x * g0;
        data[1] = -s.acceleration.z * g0;
        data[2] = s.acceleration.y * g0;
    }
    return true;
}

/* --- touch: the handheld screen */
static inline int SDL_GetNumGamepadTouchpads(SDL_Gamepad* g)
{
    return g && (g->style & HidNpadStyleTag_NpadHandheld) ? 1 : 0;
}
static HidTouchScreenState sdl_horizon_touch;
static inline int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad* g, int touchpad)
{
    (void)g; (void)touchpad;
    memset(&sdl_horizon_touch, 0, sizeof sdl_horizon_touch);
    if (!hidGetTouchScreenStates(&sdl_horizon_touch, 1)) return 0;
    return sdl_horizon_touch.count < 2 ? (int)sdl_horizon_touch.count : 2;
}
static inline bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad* g, int touchpad, int finger, bool* down,
                                                float* x, float* y, float* pressure)
{
    (void)g; (void)touchpad;
    if (finger < 0 || finger >= (int)sdl_horizon_touch.count) { if (down) *down = false; return false; }
    const HidTouchState* t = &sdl_horizon_touch.touches[finger];
    if (down) *down = true;
    if (x) *x = (float)t->x / 1280.0f;
    if (y) *y = (float)t->y / 720.0f;
    if (pressure) *pressure = 1.0f;
    return true;
}

static inline bool SDL_RumbleGamepad(SDL_Gamepad* g, uint16_t low, uint16_t high, uint32_t ms)
{
    (void)g; (void)low; (void)high; (void)ms; /* vibration: not yet on the console */
    return true;
}

/* --- names (button maps) */
static inline SDL_GamepadButton SDL_GetGamepadButtonFromString(const char* name)
{
    static const char* const names[SDL_GAMEPAD_BUTTON_COUNT] = {
        "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick",
        "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1",
        "paddle1", "paddle2", "paddle3", "paddle4", "touchpad"};
    /* Nintendo's names for the face buttons too (a, b, x, y as printed on the controller). */
    if (!strcasecmp(name, "a")) return SDL_GAMEPAD_BUTTON_EAST;
    if (!strcasecmp(name, "b")) return SDL_GAMEPAD_BUTTON_SOUTH;
    if (!strcasecmp(name, "x")) return SDL_GAMEPAD_BUTTON_NORTH;
    if (!strcasecmp(name, "y")) return SDL_GAMEPAD_BUTTON_WEST;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
        if (!strcasecmp(name, names[i])) return (SDL_GamepadButton)i;
    return SDL_GAMEPAD_BUTTON_INVALID;
}
static inline int SDL_GetScancodeFromName(const char* name) { (void)name; return SDL_SCANCODE_UNKNOWN; }

/* --- no keyboard, mouse or window */
static inline const bool* SDL_GetKeyboardState(int* count) { if (count) *count = 0; return NULL; }
static inline SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) { if (x) *x = 0; if (y) *y = 0; return 0; }
static inline SDL_Window* SDL_GetMouseFocus(void) { return NULL; }
static inline bool SDL_GetWindowSize(SDL_Window* w, int* width, int* height) { (void)w; if (width) *width = 0; if (height) *height = 0; return false; }
