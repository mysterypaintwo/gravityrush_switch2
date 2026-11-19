// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 (Switch 2): the part of SDL3 the GPU library's shim uses (gow3gpu.cpp, gow3_overlay.cpp),
// without SDL. There is one display (the console's: the default NWindow, shim/horizon/
// window_horizon.cpp), and no SDL events: the settings menu's keyboard, mouse and gamepad
// handlers compile but never run. Pads go through the runtime (src/gr_sdl_horizon.h).
#pragma once
#include <cstdint>
#include <ctime>

struct SDL_Window;
using SDL_WindowID = uint32_t;
using SDL_DisplayID = uint32_t;
using SDL_Keycode = uint32_t;
using SDL_Keymod = uint16_t;

struct SDL_DisplayMode {
    SDL_DisplayID displayID;
    int format, w, h;
    float pixel_density, refresh_rate;
};

// Key codes (SDL3's values): only compared against events that never arrive.
enum : SDL_Keycode {
    SDLK_RETURN = 0x0d, SDLK_ESCAPE = 0x1b, SDLK_BACKSPACE = 0x08, SDLK_TAB = 0x09, SDLK_SPACE = 0x20,
    SDLK_E = 0x65, SDLK_Q = 0x71, SDLK_S = 0x73, SDLK_W = 0x77, SDLK_DELETE = 0x7f,
    SDLK_F11 = 0x40000044, SDLK_F12 = 0x40000045, SDLK_INSERT = 0x40000049, SDLK_HOME = 0x4000004a,
    SDLK_PAGEUP = 0x4000004b, SDLK_END = 0x4000004d, SDLK_PAGEDOWN = 0x4000004e, SDLK_RIGHT = 0x4000004f,
    SDLK_LEFT = 0x40000050, SDLK_DOWN = 0x40000051, SDLK_UP = 0x40000052, SDLK_KP_ENTER = 0x40000058,
    SDLK_LCTRL = 0x400000e0, SDLK_LSHIFT = 0x400000e1, SDLK_LALT = 0x400000e2, SDLK_RCTRL = 0x400000e4,
    SDLK_RSHIFT = 0x400000e5, SDLK_RALT = 0x400000e6,
};
enum : SDL_Keymod { SDL_KMOD_SHIFT = 0x0003, SDL_KMOD_CTRL = 0x00c0, SDL_KMOD_ALT = 0x0300 };
enum { SDL_BUTTON_LEFT = 1, SDL_BUTTON_MIDDLE = 2, SDL_BUTTON_RIGHT = 3 };
enum SDL_GamepadButton {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};
enum SDL_GamepadAxis {
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};
enum SDL_EventType : uint32_t {
    SDL_EVENT_KEY_DOWN = 0x300, SDL_EVENT_KEY_UP, SDL_EVENT_TEXT_INPUT = 0x303,
    SDL_EVENT_MOUSE_MOTION = 0x400, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP, SDL_EVENT_MOUSE_WHEEL,
    SDL_EVENT_GAMEPAD_AXIS_MOTION = 0x650, SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_EVENT_GAMEPAD_BUTTON_UP,
};

union SDL_Event {
    uint32_t type;
    struct { uint32_t type; SDL_Keycode key; SDL_Keymod mod; bool down, repeat; } key;
    struct { uint32_t type; const char* text; } text;
    struct { uint32_t type; SDL_WindowID windowID; float x, y; } motion;
    struct { uint32_t type; uint8_t button; } button;
    struct { uint32_t type; float x, y; } wheel;
    struct { uint32_t type; uint8_t button; } gbutton;
    struct { uint32_t type; uint8_t axis; int16_t value; } gaxis;
};

inline void SDL_Delay(uint32_t ms) {
    const timespec t{time_t(ms / 1000), long(ms % 1000) * 1000000L};
    nanosleep(&t, nullptr); // a quit point at exit (switch2/host/gr_suspend_horizon.c)
}
inline SDL_DisplayID SDL_GetPrimaryDisplay() { return 1; }
// The console's output runs at 60 Hz, docked or handheld.
inline const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID) {
    static const SDL_DisplayMode mode{1, 0, 1280, 720, 1.0f, 60.0f};
    return &mode;
}
inline SDL_Window* SDL_GetWindowFromID(SDL_WindowID) { return nullptr; }
inline float SDL_GetWindowPixelDensity(SDL_Window*) { return 1.0f; }
inline bool SDL_TextInputActive(SDL_Window*) { return false; }
inline bool SDL_StartTextInput(SDL_Window*) { return false; }
inline bool SDL_StopTextInput(SDL_Window*) { return true; }
