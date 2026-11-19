// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 (Switch 2): the game's window is the console's default NWindow, which Vulkan presents to
// through VK_NN_vi_surface (renderer_vulkan/vk_platform.cpp). It replaces shim/window.cpp
// (SDL): there are no window events, the HOME menu and sleep are handled by the host
// (switch2/host/gr_suspend_horizon.c), and pads are read by the runtime.
//
// The size is 1920x1080 handheld and docked (the handheld screen's resolution; a 4K TV gets a
// 1080p image the console scales), so the swapchain never changes size when the console is
// docked or undocked.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "horizon/gow3_switch.h"
#include <SDL3/SDL.h>
#include "common/logging/log.h"
#include "gow3_overlay.h"
#include "sdl_window.h"
#include "gow3_bringup.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    (void)title;
    NWindow* nw = nwindowGetDefault();
    nwindowSetModeDimensions(nw, 1920, 1080, 1920, 1080);
    u32 w = 1920, h = 1080;
    nwindowGetDimensions(nw, &w, &h);
    width = s32(w);
    height = s32(h);
    window_info.type = WindowSystemType::Vi;
    window_info.render_surface = nw;
    std::printf("Window: the console's display, %ux%u\n", w, h);
}

WindowSDL::~WindowSDL() = default;

// The game's text dialog (sceImeDialog): no keyboard yet; the dialog confirms the text it was
// given.
void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 1;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

// The settings menu's input (Gow3Overlay::HandleEvent takes SDL events, which the console has
// none of): the pad's buttons and triggers as gamepad events (R3+L2 opens the menu, as on the
// PC), and the touch screen as the mouse. Buttons by position, as the game's prompts show them:
// the bottom face button (B) is the PS4's cross. Read on the window thread, every ~2 ms.
namespace {
struct ButtonMap {
    u64 nx;
    u8 sdl;
};
constexpr ButtonMap kButtons[] = {
    {HidNpadButton_B, SDL_GAMEPAD_BUTTON_SOUTH},         {HidNpadButton_A, SDL_GAMEPAD_BUTTON_EAST},
    {HidNpadButton_Y, SDL_GAMEPAD_BUTTON_WEST},          {HidNpadButton_X, SDL_GAMEPAD_BUTTON_NORTH},
    {HidNpadButton_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {HidNpadButton_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {HidNpadButton_StickL, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {HidNpadButton_StickR, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {HidNpadButton_Minus, SDL_GAMEPAD_BUTTON_BACK},      {HidNpadButton_Plus, SDL_GAMEPAD_BUTTON_START},
    {HidNpadButton_Up, SDL_GAMEPAD_BUTTON_DPAD_UP},      {HidNpadButton_Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {HidNpadButton_Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT},  {HidNpadButton_Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

void Send(SDL_Event& event) {
    Gow3Overlay::HandleEvent(event);
}

void PollMenuInput() {
    static bool ready = false;
    static PadState pad;
    static u64 held = 0;
    static bool zl = false, zr = false, touching = false;
    static float range_x = 1280.0f, range_y = 720.0f; // the touch screen's units (see below)
    if (!ready) {
        padInitializeDefault(&pad); // the runtime configured the pads (src/gr_sdl_horizon.h)
        hidInitializeTouchScreen();
        ready = true;
    }
    padUpdate(&pad);
    const u64 now = padGetButtons(&pad);
    // options.ini swap_ab: A is the cross (the menu's confirm), as in the game.
    static const bool swap_ab = [] {
        const char* env = std::getenv("GOW3_BUTTON_LABELS");
        return env && !std::strcmp(env, "swap_ab");
    }();
    for (const auto& entry : kButtons) {
        ButtonMap b = entry;
        if (swap_ab && b.nx == HidNpadButton_A) b.nx = HidNpadButton_B;
        else if (swap_ab && b.nx == HidNpadButton_B) b.nx = HidNpadButton_A;
        const bool down = (now & b.nx) != 0;
        if (down == ((held & b.nx) != 0)) {
            continue;
        }
        SDL_Event event{};
        event.gbutton.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        event.gbutton.button = b.sdl;
        Send(event);
    }
    const auto trigger = [](bool& state, bool down, u8 axis) {
        if (down == state) {
            return;
        }
        state = down;
        SDL_Event event{};
        event.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        event.gaxis.axis = axis;
        event.gaxis.value = down ? 32767 : 0;
        Send(event);
    };
    trigger(zl, (now & HidNpadButton_ZL) != 0, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    trigger(zr, (now & HidNpadButton_ZR) != 0, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    held = now;

    // Touch: one finger is the mouse (moves, then the left button follows it).
    HidTouchScreenState touch{};
    const bool down = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;
    if (down) {
        // libnx's touch units are the first Switch's 1280x720; a point beyond them means this
        // console reports its 1920x1080 screen instead.
        if ((touch.touches[0].x > 1280 || touch.touches[0].y > 720) && range_x < 1920.0f) {
            range_x = 1920.0f;
            range_y = 1080.0f;
            std::printf("Overlay: touch screen reports 1920x1080 units\n");
        }
        SDL_Event motion{};
        motion.motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.x = float(touch.touches[0].x) * 1920.0f / range_x;
        motion.motion.y = float(touch.touches[0].y) * 1080.0f / range_y;
        Send(motion);
    }
    if (down != touching) {
        touching = down;
        SDL_Event click{};
        click.button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        click.button.button = SDL_BUTTON_LEFT;
        Send(click);
    }
}
} // namespace

bool WindowSDL::PollEvents() {
    GOW3_BRINGUP("window thread: polling");
    PollMenuInput();
    return !Gow3Overlay::QuitDue();
}

void WindowSDL::ApplyDisplayMode(int mode) { display_mode = mode; }
void WindowSDL::UpdateCursor() {}
void WindowSDL::UpdateTextTitle() {}

} // namespace Frontend
