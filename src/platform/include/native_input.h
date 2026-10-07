#pragma once

// The Switch's controllers, natively (docs/native.md, section 2).
//
// libnx HID read once a frame, for everything that reads a controller: the
// PAD SDK a GameCube game (or a Wii game's GameCube port) uses, libwii-nx's Wii
// Remote (a Joy-Con as the remote, the left Joy-Con as its Nunchuk), and the
// settings overlay. Buttons, sticks, rumble and motion; nothing in between.

#include <cstdint>

namespace dol::input {

// players 1-8; player 1 is also the handheld pair
inline constexpr int kPlayers = 8;

// Buttons, by position as a GameCube or SDL layout names them: South is the
// bottom face button (B on a Switch pad), East the right (A), West the left (Y),
// North the top (X). Saved mappings use these positions.
enum Button : uint32_t {
    ButtonSouth = 1u << 0,
    ButtonEast = 1u << 1,
    ButtonWest = 1u << 2,
    ButtonNorth = 1u << 3,
    ButtonMinus = 1u << 4,
    ButtonHome = 1u << 5,
    ButtonPlus = 1u << 6,
    ButtonLeftStick = 1u << 7,
    ButtonRightStick = 1u << 8,
    ButtonL = 1u << 9,
    ButtonR = 1u << 10,
    ButtonDpadUp = 1u << 11,
    ButtonDpadDown = 1u << 12,
    ButtonDpadLeft = 1u << 13,
    ButtonDpadRight = 1u << 14,
    ButtonCapture = 1u << 15,
    ButtonZL = 1u << 16,
    ButtonZR = 1u << 17,
    // a single Joy-Con held sideways: its rail buttons
    ButtonLeftSL = 1u << 18,
    ButtonLeftSR = 1u << 19,
    ButtonRightSL = 1u << 20,
    ButtonRightSR = 1u << 21,
};

enum Axis : int {
    AxisLeftX,
    AxisLeftY, // +y up
    AxisRightX,
    AxisRightY,
    AxisLeftTrigger, // ZL: digital on Switch pads, 0 or 32767
    AxisRightTrigger,
    AxisCount,
};

// what is in a player's slot
enum class Style : uint8_t {
    None,
    Handheld,
    ProController,
    JoyConPair,     // a left and a right Joy-Con together
    JoyConLeft,     // one left Joy-Con, held sideways
    JoyConRight,    // one right Joy-Con, held sideways
    Other,          // GameCube adapter pads and the like, through HID's generic styles
};

// one sensor's reading: acceleration in g, rotation in turns a second (libnx's
// units), as the console's frame of reference has them
struct Motion {
    bool valid = false;
    float acceleration[3] = {};
    float angular_velocity[3] = {};
};

struct State {
    Style style = Style::None;
    uint32_t buttons = 0;       // Button bits held
    uint32_t pressed = 0;       // ... pressed since the last frame
    int16_t axes[AxisCount] = {};
    // the left and right halves' sensors (a Pro Controller or handheld has one,
    // reported as the right)
    Motion left_motion;
    Motion right_motion;
};

// HID set up for kPlayers pads and their motion sensors
void initialize();
// every pad read: once a frame, from the main loop
void update();
const State& state(int player);
// a player's rumble, 0..1 for the low and high frequency motors; 0, 0 stops it
void rumble(int player, float low, float high);
// for the settings overlay: "Pro Controller", "Joy-Con (L/R)", "Handheld", ...
const char* name(int player);

} // namespace dol::input
