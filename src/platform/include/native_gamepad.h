#pragma once

// The controller vocabulary the input layer and the settings share: buttons and axes in SDL3's order
// (SDL_GamepadButton, SDL_GamepadAxis), so a binding saved in config.toml means the same button it always
// did. Native only: nothing behind it but these tables, where Aurora's gamepad layer used to be.

#include <cstdint>

struct AuroraGamepad;
using AuroraControllerID = uint32_t;  // a controller instance, as SDL_JoystickID

// A stick's range, as SDL reports it (SDL_JOYSTICK_AXIS_MIN / MAX).
inline constexpr int AURORA_JOYSTICK_AXIS_MIN = -32768;
inline constexpr int AURORA_JOYSTICK_AXIS_MAX = 32767;

enum AuroraGamepadButton : int {
    AURORA_GAMEPAD_BUTTON_INVALID = -1,
    AURORA_GAMEPAD_BUTTON_SOUTH,
    AURORA_GAMEPAD_BUTTON_EAST,
    AURORA_GAMEPAD_BUTTON_WEST,
    AURORA_GAMEPAD_BUTTON_NORTH,
    AURORA_GAMEPAD_BUTTON_BACK,
    AURORA_GAMEPAD_BUTTON_GUIDE,
    AURORA_GAMEPAD_BUTTON_START,
    AURORA_GAMEPAD_BUTTON_LEFT_STICK,
    AURORA_GAMEPAD_BUTTON_RIGHT_STICK,
    AURORA_GAMEPAD_BUTTON_LEFT_SHOULDER,
    AURORA_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    AURORA_GAMEPAD_BUTTON_DPAD_UP,
    AURORA_GAMEPAD_BUTTON_DPAD_DOWN,
    AURORA_GAMEPAD_BUTTON_DPAD_LEFT,
    AURORA_GAMEPAD_BUTTON_DPAD_RIGHT,
    AURORA_GAMEPAD_BUTTON_MISC1,
    AURORA_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    AURORA_GAMEPAD_BUTTON_LEFT_PADDLE1,
    AURORA_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    AURORA_GAMEPAD_BUTTON_LEFT_PADDLE2,
    AURORA_GAMEPAD_BUTTON_TOUCHPAD,
    AURORA_GAMEPAD_BUTTON_MISC2,
    AURORA_GAMEPAD_BUTTON_MISC3,
    AURORA_GAMEPAD_BUTTON_MISC4,
    AURORA_GAMEPAD_BUTTON_MISC5,
    AURORA_GAMEPAD_BUTTON_MISC6,
    AURORA_GAMEPAD_BUTTON_COUNT
};

enum AuroraGamepadAxis : int {
    AURORA_GAMEPAD_AXIS_INVALID = -1,
    AURORA_GAMEPAD_AXIS_LEFTX,
    AURORA_GAMEPAD_AXIS_LEFTY,
    AURORA_GAMEPAD_AXIS_RIGHTX,
    AURORA_GAMEPAD_AXIS_RIGHTY,
    AURORA_GAMEPAD_AXIS_LEFT_TRIGGER,
    AURORA_GAMEPAD_AXIS_RIGHT_TRIGGER,
    AURORA_GAMEPAD_AXIS_COUNT
};

// The keys the settings overlay names, as SDL3's scancodes (USB HID usages).
enum AuroraScancode : int {
    AURORA_SCANCODE_A = 4, AURORA_SCANCODE_D = 7, AURORA_SCANCODE_E = 8, AURORA_SCANCODE_I = 12,
    AURORA_SCANCODE_J = 13, AURORA_SCANCODE_K = 14, AURORA_SCANCODE_L = 15, AURORA_SCANCODE_Q = 20,
    AURORA_SCANCODE_S = 22, AURORA_SCANCODE_W = 26, AURORA_SCANCODE_RETURN = 40, AURORA_SCANCODE_ESCAPE = 41,
    AURORA_SCANCODE_BACKSPACE = 42, AURORA_SCANCODE_SPACE = 44, AURORA_SCANCODE_BACKSLASH = 49,
    AURORA_SCANCODE_F10 = 67, AURORA_SCANCODE_DELETE = 76, AURORA_SCANCODE_RIGHT = 79, AURORA_SCANCODE_LEFT = 80,
    AURORA_SCANCODE_DOWN = 81, AURORA_SCANCODE_UP = 82, AURORA_SCANCODE_LSHIFT = 225, AURORA_SCANCODE_COUNT = 512
};

// The keyboard and mouse as the overlay reads them; native meanings in gpu/aurora_off_switch.cpp.
extern "C" bool aurora_keyboard_focused();
extern "C" const bool* aurora_keyboard_state(int* count);
extern "C" uint32_t aurora_mouse_buttons(float* x, float* y);
extern "C" const char* aurora_scancode_name(int scancode);
extern "C" void aurora_set_cursor_visible(bool visible);
extern "C" void aurora_set_threaded_gx(bool threaded);

// SDL's own names for them (SDL_GetGamepadStringForButton / ForAxis).
inline const char* aurora_gamepad_button_string(AuroraGamepadButton button) {
    static constexpr const char* kNames[AURORA_GAMEPAD_BUTTON_COUNT] = {
        "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
        "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3",
        "paddle4", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"};
    return button > AURORA_GAMEPAD_BUTTON_INVALID && button < AURORA_GAMEPAD_BUTTON_COUNT ? kNames[button] : nullptr;
}

inline const char* aurora_gamepad_axis_string(AuroraGamepadAxis axis) {
    static constexpr const char* kNames[AURORA_GAMEPAD_AXIS_COUNT] = {
        "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
    return axis > AURORA_GAMEPAD_AXIS_INVALID && axis < AURORA_GAMEPAD_AXIS_COUNT ? kNames[axis] : nullptr;
}
