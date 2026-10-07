// The PAD SDK on the Switch, over libdol's own controllers (native_input.h;
// docs/native.md, section 2).
//
// What Aurora's aurora_pad did, without Aurora: the GameCube pad the games'
// natives and the settings overlay read - button, alternate and axis mappings
// with their dead zones, the clamps, rumble, names - with dol::input as the
// controllers. Carried over from Aurora's lib/dolphin/pad/pad.cpp (MIT,
// Copyright the Aurora contributors) for what the Switch has: no keyboard or
// mouse bindings, no LED colour or battery, no VID/PID.
//
// Mappings keep Aurora's numbering (AURORA_GAMEPAD_BUTTON_*, _AXIS_*) and its
// file format and names ("<name>_0000_0000.controller" in the data folder), so
// mappings saved under Aurora carry over.
#include "native_input.h"
#include "runtime_config.h"

#include <aurora/gamepad.h>
#include <dolphin/pad.h>
#include <dolphin/si.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

// Aurora's own pads are still what the settings overlay reads (through
// PADGetGamepadForIndex) until it moves to dol::input with section 5; nothing
// but Aurora's PADRead polled them, so this one does.
namespace aurora::input {
void poll() noexcept;
}

namespace {

constexpr uint32_t kMagic = 'CTRL';  // as Aurora's SBIG('CTRL') reads back little-endian
constexpr uint32_t kMappingsFileVersion = 3;

// One table for every Switch style: Aurora's Pro Controller, Joy-Con and
// standard defaults are the same.
constexpr std::array<PADButtonMapping, PAD_BUTTON_COUNT> kDefaultButtons{{
    {AURORA_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A},
    {AURORA_GAMEPAD_BUTTON_EAST, PAD_BUTTON_B},
    {AURORA_GAMEPAD_BUTTON_WEST, PAD_BUTTON_X},
    {AURORA_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y},
    {AURORA_GAMEPAD_BUTTON_START, PAD_BUTTON_START},
    {AURORA_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_Z},
    {PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_L},
    {PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_R},
    {AURORA_GAMEPAD_BUTTON_DPAD_UP, PAD_BUTTON_UP},
    {AURORA_GAMEPAD_BUTTON_DPAD_DOWN, PAD_BUTTON_DOWN},
    {AURORA_GAMEPAD_BUTTON_DPAD_LEFT, PAD_BUTTON_LEFT},
    {AURORA_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_BUTTON_RIGHT},
}};

constexpr std::array<PADAxisMapping, PAD_AXIS_COUNT> kDefaultAxes{{
    {{AURORA_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_POSITIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_POS},
    {{AURORA_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_NEGATIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_NEG},
    // the gamepad y axis is down-positive, the GameCube's up-positive
    {{AURORA_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_NEGATIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_POS},
    {{AURORA_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_POSITIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_NEG},
    {{AURORA_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_POSITIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_POS},
    {{AURORA_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_NEGATIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_NEG},
    {{AURORA_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_NEGATIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_POS},
    {{AURORA_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_POSITIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_NEG},
    {{AURORA_GAMEPAD_AXIS_LEFT_TRIGGER, AXIS_SIGN_POSITIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_L},
    {{AURORA_GAMEPAD_AXIS_RIGHT_TRIGGER, AXIS_SIGN_POSITIVE}, AURORA_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_R},
}};

constexpr PADDeadZones kDefaultDeadZones{
    .emulateTriggers = true,
    .useDeadzones = true,
    .stickDeadZone = 8000,
    .substickDeadZone = 8000,
    .leftTriggerActivationZone = 31150,
    .rightTriggerActivationZone = 31150,
};

// The GameCube's clamp regions, as the SDK has them.
struct ClampRegion {
    uint8_t minTrigger, maxTrigger;
    int8_t minStick, maxStick, xyStick;
    int8_t minSubstick, maxSubstick, xySubstick;
    int8_t radStick, radSubstick;
};
constexpr ClampRegion kClamp{30, 180, 15, 72, 40, 15, 59, 31, 56, 44};

// What PAD keeps for a dol::input player (Aurora's GameController, less what
// belonged to SDL).
struct Controller {
    dol::input::Style style = dol::input::Style::None;  // what the mapping was loaded for
    bool mappingLoaded = false;
    PADDeadZones deadZones = kDefaultDeadZones;
    std::array<PADButtonMapping, PAD_BUTTON_COUNT> buttons{};
    // a second binding per button, never saved: the runtime re-applies it
    std::array<PADButtonMapping, PAD_BUTTON_COUNT> altButtons{};
    std::array<PADAxisMapping, PAD_AXIS_COUNT> axes{};
    uint16_t rumbleLow = 32767;
    uint16_t rumbleHigh = 32767;
};

std::array<Controller, dol::input::kPlayers> g_controllers;
// which player each GameCube port reads; -1 for none
std::array<int, PAD_CHANMAX> g_portPlayer{0, 1, 2, 3};

std::atomic_bool g_blockPAD{false};
bool g_suppressHeldOnRead = false;
std::array<PADButton, PAD_CHANMAX> g_suppressedButtons{};
std::array<bool, PAD_CHANMAX> g_suppressLeftTrigger{};
std::array<bool, PAD_CHANMAX> g_suppressRightTrigger{};

bool Connected(int player) {
    return player >= 0 && player < dol::input::kPlayers &&
           dol::input::state(player).style != dol::input::Style::None;
}

// Aurora's names for the Switch styles: part of the mapping files' names
const char* StyleName(dol::input::Style style) {
    switch (style) {
    case dol::input::Style::ProController: return "Pro Controller";
    case dol::input::Style::Handheld:
    case dol::input::Style::JoyConPair: return "Joy-Con (L/R)";
    case dol::input::Style::JoyConLeft: return "Joy-Con (L)";
    case dol::input::Style::JoyConRight: return "Joy-Con (R)";
    default: return "Controller";
    }
}

// the n-th connected player, as Aurora numbered its controller map
int PlayerForIndex(uint32_t index) {
    for (int player = 0; player < dol::input::kPlayers; ++player) {
        if (Connected(player) && index-- == 0) {
            return player;
        }
    }
    return -1;
}

int IndexForPlayer(int player) {
    int index = 0;
    for (int other = 0; other < player; ++other) {
        index += Connected(other) ? 1 : 0;
    }
    return index;
}

int PlayerForPort(uint32_t port) {
    if (port >= PAD_CHANMAX) {
        return -1;
    }
    const int player = g_portPlayer[port];
    return Connected(player) ? player : -1;
}

// a native button, by Aurora's numbering, read from dol::input
bool NativeButton(const dol::input::State& state, int button) {
    using namespace dol::input;
    uint32_t bit = 0;
    switch (button) {
    case AURORA_GAMEPAD_BUTTON_SOUTH: bit = ButtonSouth; break;
    case AURORA_GAMEPAD_BUTTON_EAST: bit = ButtonEast; break;
    case AURORA_GAMEPAD_BUTTON_WEST: bit = ButtonWest; break;
    case AURORA_GAMEPAD_BUTTON_NORTH: bit = ButtonNorth; break;
    case AURORA_GAMEPAD_BUTTON_BACK: bit = ButtonMinus; break;
    case AURORA_GAMEPAD_BUTTON_START: bit = ButtonPlus; break;
    case AURORA_GAMEPAD_BUTTON_LEFT_STICK: bit = ButtonLeftStick; break;
    case AURORA_GAMEPAD_BUTTON_RIGHT_STICK: bit = ButtonRightStick; break;
    case AURORA_GAMEPAD_BUTTON_LEFT_SHOULDER: bit = ButtonL; break;
    case AURORA_GAMEPAD_BUTTON_RIGHT_SHOULDER: bit = ButtonR; break;
    case AURORA_GAMEPAD_BUTTON_DPAD_UP: bit = ButtonDpadUp; break;
    case AURORA_GAMEPAD_BUTTON_DPAD_DOWN: bit = ButtonDpadDown; break;
    case AURORA_GAMEPAD_BUTTON_DPAD_LEFT: bit = ButtonDpadLeft; break;
    case AURORA_GAMEPAD_BUTTON_DPAD_RIGHT: bit = ButtonDpadRight; break;
    default: return false;
    }
    return (state.buttons & bit) != 0;
}

// a native axis, by Aurora's numbering: sticks down-positive, triggers 0..max
int16_t NativeAxis(const dol::input::State& state, int axis) {
    using namespace dol::input;
    const auto down = [](int16_t up) {
        return static_cast<int16_t>(up == AURORA_JOYSTICK_AXIS_MIN ? AURORA_JOYSTICK_AXIS_MAX : -up);
    };
    switch (axis) {
    case AURORA_GAMEPAD_AXIS_LEFTX: return state.axes[AxisLeftX];
    case AURORA_GAMEPAD_AXIS_LEFTY: return down(state.axes[AxisLeftY]);
    case AURORA_GAMEPAD_AXIS_RIGHTX: return state.axes[AxisRightX];
    case AURORA_GAMEPAD_AXIS_RIGHTY: return down(state.axes[AxisRightY]);
    case AURORA_GAMEPAD_AXIS_LEFT_TRIGGER: return state.axes[AxisLeftTrigger];
    case AURORA_GAMEPAD_AXIS_RIGHT_TRIGGER: return state.axes[AxisRightTrigger];
    default: return 0;
    }
}

bool BindingPressed(const dol::input::State& state, uint32_t binding) {
    if (PADIsAxisButton(binding)) {
        const uint32_t axis = PADAxisButtonAxis(binding);
        const uint32_t threshold = PADAxisButtonThreshold(binding);
        if (axis >= AURORA_GAMEPAD_AXIS_COUNT || threshold < 1 || threshold > 100) {
            return false;
        }
        int value = NativeAxis(state, static_cast<int>(axis));
        if (PADAxisButtonNegative(binding)) {
            value = -value;
        }
        return value > 0 && value * 100 >= static_cast<int>(threshold) * 32767;
    }
    return binding < AURORA_GAMEPAD_BUTTON_COUNT && NativeButton(state, static_cast<int>(binding));
}

void ResetAltButtons(Controller& controller) {
    for (size_t i = 0; i < PAD_BUTTON_COUNT; ++i) {
        controller.altButtons[i] = {PAD_NATIVE_BUTTON_INVALID, controller.buttons[i].padButton};
    }
}

void SetDefaults(Controller& controller) {
    controller.buttons = kDefaultButtons;
    controller.axes = kDefaultAxes;
    ResetAltButtons(controller);
}

std::filesystem::path MappingPath(dol::input::Style style) {
    return RuntimeConfigFile::ApplicationDataDirectory() /
           (std::string(StyleName(style)) + "_0000_0000.controller");
}

bool ReadU32(std::FILE* file, uint32_t* value) {
    uint8_t b[4];
    if (std::fread(b, 1, 4, file) != 4) {
        return false;
    }
    *value = b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24;
    return true;
}

void WriteU32(std::FILE* file, uint32_t value) {
    const uint8_t b[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    std::fwrite(b, 1, 4, file);
}

bool ValidNativeAxis(const PADSignedNativeAxis axis) {
    return axis.nativeAxis == -1 ||
           (axis.nativeAxis >= 0 && axis.nativeAxis < AURORA_GAMEPAD_AXIS_COUNT &&
            (axis.sign == AXIS_SIGN_POSITIVE || axis.sign == AXIS_SIGN_NEGATIVE));
}

// the player's saved mapping, or the defaults; again whenever its style changes
// (a Joy-Con pair split, the console docked), as Aurora did on a reconnect
Controller& Load(int player) {
    Controller& controller = g_controllers[player];
    const dol::input::Style style = dol::input::state(player).style;
    if (controller.mappingLoaded && controller.style == style) {
        return controller;
    }
    controller = Controller{};
    controller.style = style;
    controller.mappingLoaded = true;
    SetDefaults(controller);

    std::FILE* file = std::fopen(MappingPath(style).c_str(), "rb");
    if (file == nullptr) {
        return controller;
    }
    uint32_t magic = 0, version = 0;
    uint8_t isGameCube = 0;
    if (!ReadU32(file, &magic) || magic != kMagic || !ReadU32(file, &version) ||
        version != kMappingsFileVersion || std::fread(&isGameCube, 1, 1, file) != 1 || isGameCube) {
        std::fclose(file);
        return controller;
    }
    std::fseek(file, 32, SEEK_SET);  // the data starts at the next 32-byte boundary
    Controller loaded = controller;
    const bool complete =
        std::fread(&loaded.deadZones, sizeof(PADDeadZones), 1, file) == 1 &&
        std::fread(loaded.buttons.data(), sizeof(PADButtonMapping), PAD_BUTTON_COUNT, file) == PAD_BUTTON_COUNT &&
        std::fread(loaded.axes.data(), sizeof(PADAxisMapping), PAD_AXIS_COUNT, file) == PAD_AXIS_COUNT &&
        std::fread(&loaded.rumbleLow, sizeof(uint16_t), 1, file) == 1 &&
        std::fread(&loaded.rumbleHigh, sizeof(uint16_t), 1, file) == 1;
    std::fclose(file);
    if (!complete) {
        return controller;
    }

    // each part is taken only if it reads back sane, as Aurora checked them
    const PADDeadZones& zones = loaded.deadZones;
    if (zones.stickDeadZone <= AURORA_JOYSTICK_AXIS_MAX && zones.substickDeadZone <= AURORA_JOYSTICK_AXIS_MAX &&
        zones.leftTriggerActivationZone <= AURORA_JOYSTICK_AXIS_MAX &&
        zones.rightTriggerActivationZone <= AURORA_JOYSTICK_AXIS_MAX) {
        controller.deadZones = zones;
    }
    bool axesSane = true;
    for (uint32_t i = 0; i < PAD_AXIS_COUNT; ++i) {
        axesSane = axesSane && loaded.axes[i].padAxis == static_cast<PADAxis>(i) &&
                   ValidNativeAxis(loaded.axes[i].nativeAxis);
    }
    if (axesSane) {
        controller.axes = loaded.axes;
    }
    if (std::ranges::none_of(loaded.buttons, [](const auto& mapping) { return mapping.padButton == 0; })) {
        controller.buttons = loaded.buttons;
    }
    controller.rumbleLow = loaded.rumbleLow;
    controller.rumbleHigh = loaded.rumbleHigh;
    ResetAltButtons(controller);
    return controller;
}

Controller* ControllerForPort(uint32_t port) {
    const int player = PlayerForPort(port);
    return player < 0 ? nullptr : &Load(player);
}

int16_t AxisValue(const Controller& controller, const dol::input::State& state, PADAxis axis) {
    const auto it = std::ranges::find_if(controller.axes, [axis](const auto& mapping) { return mapping.padAxis == axis; });
    if (it == controller.axes.end()) {
        return 0;
    }
    if (it->nativeAxis.nativeAxis != -1) {
        const int16_t value = NativeAxis(state, it->nativeAxis.nativeAxis);
        if (it->nativeAxis.sign == AXIS_SIGN_POSITIVE) {
            return value > 0 ? value : 0;
        }
        if (value >= 0) {
            return 0;
        }
        return static_cast<int16_t>(value == AURORA_JOYSTICK_AXIS_MIN ? AURORA_JOYSTICK_AXIS_MAX : -value);
    }
    return it->nativeButton >= 0 && NativeButton(state, it->nativeButton) ? AURORA_JOYSTICK_AXIS_MAX : 0;
}

int8_t Stick(int16_t value, bool useDeadZones, uint16_t deadZone) {
    if (useDeadZones && std::abs(value) <= deadZone) {
        return 0;
    }
    return static_cast<int8_t>(value / 256);
}

void Neutralize(PADStatus& status) {
    const s8 err = status.err;
    std::memset(&status, 0, sizeof(status));
    status.err = err;
}

// After input is unblocked, what was already held stays released until let go,
// so closing a menu with A does not also press A in the game.
void SuppressHeld(PADStatus& status, uint32_t port, bool captureHeld) {
    if (captureHeld) {
        g_suppressedButtons[port] |= status.button;
        g_suppressLeftTrigger[port] = g_suppressLeftTrigger[port] || status.triggerLeft > kClamp.minTrigger;
        g_suppressRightTrigger[port] = g_suppressRightTrigger[port] || status.triggerRight > kClamp.minTrigger;
    }
    g_suppressedButtons[port] &= status.button;
    status.button &= ~g_suppressedButtons[port];
    for (auto [suppress, trigger] : {std::pair{&g_suppressLeftTrigger[port], &status.triggerLeft},
                                     std::pair{&g_suppressRightTrigger[port], &status.triggerRight}}) {
        if (!*suppress) {
            continue;
        }
        if (*trigger <= kClamp.minTrigger) {
            *suppress = false;
        } else {
            *trigger = 0;
        }
    }
}

void ReadPort(const Controller& controller, const dol::input::State& state, PADStatus& status) {
    bool leftTriggerSet = false;
    bool rightTriggerSet = false;
    const auto apply = [&](const PADButtonMapping& mapping, bool alt) {
        if (alt && mapping.nativeButton == PAD_NATIVE_BUTTON_INVALID) {
            return;
        }
        if (BindingPressed(state, mapping.nativeButton)) {
            status.button |= mapping.padButton;
        }
        if (mapping.nativeButton != PAD_NATIVE_BUTTON_INVALID) {
            leftTriggerSet = leftTriggerSet || mapping.padButton == PAD_TRIGGER_L;
            rightTriggerSet = rightTriggerSet || mapping.padButton == PAD_TRIGGER_R;
        }
    };
    for (const auto& mapping : controller.buttons) {
        apply(mapping, false);
    }
    for (const auto& mapping : controller.altButtons) {
        apply(mapping, true);
    }

    static constexpr std::array<std::pair<int, PADExtButton>, 4> kExtButtons{{
        {AURORA_GAMEPAD_BUTTON_BACK, PAD_BUTTON_BACK},
        {AURORA_GAMEPAD_BUTTON_GUIDE, PAD_BUTTON_GUIDE},
        {AURORA_GAMEPAD_BUTTON_RIGHT_STICK, PAD_BUTTON_RIGHT_STICK},
        {AURORA_GAMEPAD_BUTTON_LEFT_STICK, PAD_BUTTON_LEFT_STICK},
    }};
    for (const auto& [native, button] : kExtButtons) {
        if (NativeButton(state, native)) {
            status.extButton |= button;
        }
    }

    const PADDeadZones& zones = controller.deadZones;
    const auto axis = [&](PADAxis a) { return AxisValue(controller, state, a); };
    status.stickX = Stick(static_cast<int16_t>(axis(PAD_AXIS_LEFT_X_POS) - axis(PAD_AXIS_LEFT_X_NEG)),
                          zones.useDeadzones, zones.stickDeadZone);
    status.stickY = Stick(static_cast<int16_t>(axis(PAD_AXIS_LEFT_Y_POS) - axis(PAD_AXIS_LEFT_Y_NEG)),
                          zones.useDeadzones, zones.stickDeadZone);
    status.substickX = Stick(static_cast<int16_t>(axis(PAD_AXIS_RIGHT_X_POS) - axis(PAD_AXIS_RIGHT_X_NEG)),
                             zones.useDeadzones, zones.substickDeadZone);
    status.substickY = Stick(static_cast<int16_t>(axis(PAD_AXIS_RIGHT_Y_POS) - axis(PAD_AXIS_RIGHT_Y_NEG)),
                             zones.useDeadzones, zones.substickDeadZone);

    // A Switch pad's ZL/ZR are digital: an explicit L/R binding drives the
    // analog value too, or the trigger would still pull with L/R rebound.
    int16_t left = std::max<int16_t>(0, axis(PAD_AXIS_TRIGGER_L));
    int16_t right = std::max<int16_t>(0, axis(PAD_AXIS_TRIGGER_R));
    if (leftTriggerSet) {
        left = (status.button & PAD_TRIGGER_L) != 0 ? 32767 : 0;
    }
    if (rightTriggerSet) {
        right = (status.button & PAD_TRIGGER_R) != 0 ? 32767 : 0;
    }
    if (zones.emulateTriggers) {
        if (!leftTriggerSet && left > zones.leftTriggerActivationZone) {
            status.button |= PAD_TRIGGER_L;
        }
        if (!rightTriggerSet && right > zones.rightTriggerActivationZone) {
            status.button |= PAD_TRIGGER_R;
        }
    }
    status.triggerLeft = static_cast<u8>(left / 128);
    status.triggerRight = static_cast<u8>(right / 128);
}

void ClampTrigger(u8* trigger, u8 min, u8 max) {
    if (*trigger <= min) {
        *trigger = 0;
    } else {
        *trigger = static_cast<u8>(std::min(*trigger, max) - min);
    }
}

void ClampCircle(s8* px, s8* py, s8 radius, s8 min) {
    const auto shrink = [min](int v) { return -min < v && v < min ? 0 : v > 0 ? v - min : v + min; };
    int x = shrink(*px);
    int y = shrink(*py);
    if (const int squared = x * x + y * y; radius * radius < squared) {
        const auto length = static_cast<int>(std::sqrt(squared));
        x = x * radius / length;
        y = y * radius / length;
    }
    *px = static_cast<s8>(x);
    *py = static_cast<s8>(y);
}

void ClampStick(s8* px, s8* py, s8 max, s8 xy, s8 min) {
    int x = *px;
    int y = *py;
    const int signX = x < 0 ? -1 : 1;
    const int signY = y < 0 ? -1 : 1;
    x = std::abs(x);
    y = std::abs(y);
    x = x <= min ? 0 : x - min;
    y = y <= min ? 0 : y - min;
    if (x == 0 && y == 0) {
        *px = *py = 0;
        return;
    }
    x = x * max / (INT8_MAX - min);
    y = y * max / (INT8_MAX - min);
    const int d = xy * y <= xy * x ? xy * x + (max - xy) * y : xy * y + (max - xy) * x;
    if (xy * max < d) {
        x = xy * max * x / d;
        y = xy * max * y / d;
    }
    *px = static_cast<s8>(signX * x);
    *py = static_cast<s8>(signY * y);
}

constexpr std::array<std::pair<PADButton, const char*>, PAD_BUTTON_COUNT> kButtonNames{{
    {PAD_BUTTON_LEFT, "Left"}, {PAD_BUTTON_RIGHT, "Right"}, {PAD_BUTTON_DOWN, "Down"},
    {PAD_BUTTON_UP, "Up"},     {PAD_TRIGGER_Z, "Z"},        {PAD_TRIGGER_R, "R"},
    {PAD_TRIGGER_L, "L"},      {PAD_BUTTON_A, "A"},         {PAD_BUTTON_B, "B"},
    {PAD_BUTTON_X, "X"},       {PAD_BUTTON_Y, "Y"},         {PAD_BUTTON_START, "Start"},
}};
constexpr std::array<const char*, PAD_AXIS_COUNT> kAxisNames{
    "Left X+", "Left X-", "Left Y+", "Left Y-", "Right X+", "Right X-", "Right Y+", "Right Y-",
    "Trigger L", "Trigger R",
};
constexpr std::array<const char*, PAD_AXIS_COUNT> kAxisDirections{
    "Right", "Left", "Up", "Down", "Right", "Left", "Up", "Down", "N/A", "N/A",
};

}  // namespace

BOOL PADInit() { return TRUE; }
BOOL PADReset(u32) { return TRUE; }
BOOL PADRecalibrate(u32) { return TRUE; }
void PADSetSpec(u32) {}
void PADSetAnalogMode(u32) {}

u32 PADRead(PADStatus* status) {
    aurora::input::poll();
    const bool blocked = g_blockPAD.load(std::memory_order_acquire);
    const bool captureHeld = g_suppressHeldOnRead && !blocked;
    g_suppressHeldOnRead = false;

    u32 rumble = 0;
    for (u32 port = 0; port < PAD_CHANMAX; ++port) {
        std::memset(&status[port], 0, sizeof(PADStatus));
        const int player = PlayerForPort(port);
        if (player < 0) {
            status[port].err = PAD_ERR_NO_CONTROLLER;
            g_suppressedButtons[port] = 0;
            g_suppressLeftTrigger[port] = false;
            g_suppressRightTrigger[port] = false;
            continue;
        }
        status[port].err = PAD_ERR_NONE;
        ReadPort(Load(player), dol::input::state(player), status[port]);
        rumble |= PAD_CHAN0_BIT >> port;
        if (blocked) {
            Neutralize(status[port]);
        } else {
            SuppressHeld(status[port], port, captureHeld);
        }
    }
    return rumble;
}

void PADControlMotor(u32 chan, u32 cmd) {
    const int player = PlayerForPort(chan);
    if (player < 0) {
        return;
    }
    const Controller& controller = Load(player);
    if (cmd == PAD_MOTOR_RUMBLE) {
        dol::input::rumble(player, controller.rumbleLow / 65535.0f, controller.rumbleHigh / 65535.0f);
    } else {
        dol::input::rumble(player, 0.0f, 0.0f);
    }
}

void PADControlAllMotors(const u32* commands) {
    for (u32 port = 0; port < PAD_CHANMAX; ++port) {
        PADControlMotor(port, commands[port]);
    }
}

void PADClamp(PADStatus* status) {
    for (u32 port = 0; port < PAD_CHANMAX; ++port) {
        if (status[port].err != PAD_ERR_NONE) {
            continue;
        }
        ClampStick(&status[port].stickX, &status[port].stickY, kClamp.maxStick, kClamp.xyStick, kClamp.minStick);
        ClampStick(&status[port].substickX, &status[port].substickY, kClamp.maxSubstick, kClamp.xySubstick,
                   kClamp.minSubstick);
        ClampTrigger(&status[port].triggerLeft, kClamp.minTrigger, kClamp.maxTrigger);
        ClampTrigger(&status[port].triggerRight, kClamp.minTrigger, kClamp.maxTrigger);
    }
}

void PADClampCircle(PADStatus* status) {
    for (u32 port = 0; port < PAD_CHANMAX; ++port) {
        if (status[port].err != PAD_ERR_NONE) {
            continue;
        }
        ClampCircle(&status[port].stickX, &status[port].stickY, kClamp.radStick, kClamp.minStick);
        ClampCircle(&status[port].substickX, &status[port].substickY, kClamp.radSubstick, kClamp.minSubstick);
        ClampTrigger(&status[port].triggerLeft, kClamp.minTrigger, kClamp.maxTrigger);
        ClampTrigger(&status[port].triggerRight, kClamp.minTrigger, kClamp.maxTrigger);
    }
}

// ---------- controllers and ports

u32 PADCount() {
    u32 count = 0;
    for (int player = 0; player < dol::input::kPlayers; ++player) {
        count += Connected(player) ? 1 : 0;
    }
    return count;
}

const char* PADGetNameForControllerIndex(u32 index) {
    const int player = PlayerForIndex(index);
    return player < 0 ? nullptr : StyleName(dol::input::state(player).style);
}

void PADSetPortForIndex(u32 index, u32 port) {
    const int player = PlayerForIndex(index);
    if (player < 0 || port >= PAD_CHANMAX) {
        return;
    }
    for (int& other : g_portPlayer) {
        if (other == player) {
            other = -1;
        }
    }
    g_portPlayer[port] = player;
}

s32 PADGetIndexForPort(u32 port) {
    const int player = PlayerForPort(port);
    return player < 0 ? -1 : IndexForPlayer(player);
}

void PADClearPort(u32 port) {
    if (port < PAD_CHANMAX) {
        g_portPlayer[port] = -1;
    }
}

void PADGetVidPid(u32, u32* vid, u32* pid) {
    *vid = 0;
    *pid = 0;
}

const char* PADGetName(u32 port) {
    const int player = PlayerForPort(port);
    return player < 0 ? nullptr : StyleName(dol::input::state(player).style);
}

BOOL PADIsGCAdapter(u32) { return FALSE; }

AuroraGamepad* PADGetGamepadForIndex(u32 index) {
    const int player = PlayerForIndex(index);
    return player < 0 ? nullptr : aurora_gamepad_for_player(player);
}

// ---------- mappings

void PADSetButtonMapping(u32 port, PADButtonMapping mapping) {
    if (Controller* controller = ControllerForPort(port)) {
        for (auto& existing : controller->buttons) {
            if (existing.padButton == mapping.padButton) {
                existing = mapping;
            }
        }
    }
}

void PADSetAllButtonMappings(u32 port, PADButtonMapping buttons[PAD_BUTTON_COUNT]) {
    for (u32 i = 0; i < PAD_BUTTON_COUNT; ++i) {
        PADSetButtonMapping(port, buttons[i]);
    }
}

PADButtonMapping* PADGetButtonMappings(u32 port, u32* count) {
    Controller* controller = ControllerForPort(port);
    *count = controller ? PAD_BUTTON_COUNT : 0;
    return controller ? controller->buttons.data() : nullptr;
}

void PADSetAltButtonMapping(u32 port, PADButtonMapping mapping) {
    if (Controller* controller = ControllerForPort(port)) {
        for (auto& existing : controller->altButtons) {
            if (existing.padButton == mapping.padButton) {
                existing = mapping;
            }
        }
    }
}

PADButtonMapping* PADGetAltButtonMappings(u32 port, u32* count) {
    Controller* controller = ControllerForPort(port);
    *count = controller ? PAD_BUTTON_COUNT : 0;
    return controller ? controller->altButtons.data() : nullptr;
}

void PADSetAxisMapping(u32 port, PADAxisMapping mapping) {
    if (Controller* controller = ControllerForPort(port)) {
        for (auto& existing : controller->axes) {
            if (existing.padAxis == mapping.padAxis) {
                existing = mapping;
            }
        }
    }
}

void PADSetAllAxisMappings(u32 port, PADAxisMapping axes[PAD_AXIS_COUNT]) {
    for (u32 i = 0; i < PAD_AXIS_COUNT; ++i) {
        PADSetAxisMapping(port, axes[i]);
    }
}

PADAxisMapping* PADGetAxisMappings(u32 port, u32* count) {
    Controller* controller = ControllerForPort(port);
    *count = controller ? PAD_AXIS_COUNT : 0;
    return controller ? controller->axes.data() : nullptr;
}

PADDeadZones* PADGetDeadZones(u32 port) {
    Controller* controller = ControllerForPort(port);
    return controller ? &controller->deadZones : nullptr;
}

void PADRestoreDefaultMapping(u32 port) {
    if (Controller* controller = ControllerForPort(port)) {
        SetDefaults(*controller);
    }
}

// Each connected controller's mapping, in Aurora's file layout: header, then
// at 32 bytes the dead zones, buttons, axes and rumble strengths.
void PADSerializeMappings() {
    std::error_code error;
    std::filesystem::create_directories(RuntimeConfigFile::ApplicationDataDirectory(), error);
    for (int player = 0; player < dol::input::kPlayers; ++player) {
        if (!Connected(player)) {
            continue;
        }
        const Controller& controller = Load(player);
        std::FILE* file = std::fopen(MappingPath(controller.style).c_str(), "wb");
        if (file == nullptr) {
            continue;
        }
        WriteU32(file, kMagic);
        WriteU32(file, kMappingsFileVersion);
        const uint8_t header[32 - 8] = {};  // isGameCube = 0, then padding
        std::fwrite(header, 1, sizeof(header), file);
        std::fwrite(&controller.deadZones, sizeof(PADDeadZones), 1, file);
        std::fwrite(controller.buttons.data(), sizeof(PADButtonMapping), PAD_BUTTON_COUNT, file);
        std::fwrite(controller.axes.data(), sizeof(PADAxisMapping), PAD_AXIS_COUNT, file);
        std::fwrite(&controller.rumbleLow, sizeof(uint16_t), 1, file);
        std::fwrite(&controller.rumbleHigh, sizeof(uint16_t), 1, file);
        std::fclose(file);
    }
}

// ---------- the keyboard: the Switch has none

BOOL PADSetKeyButtonBinding(u32, PADKeyButtonBinding) { return FALSE; }
BOOL PADSetKeyButtonBindings(u32, PADKeyButtonBinding[PAD_BUTTON_COUNT]) { return FALSE; }
PADKeyButtonBinding* PADGetKeyButtonBindings(u32, u32* count) {
    *count = 0;
    return nullptr;
}
BOOL PADSetKeyAxisBinding(u32, PADKeyAxisBinding) { return FALSE; }
BOOL PADSetKeyAxisBindings(u32, PADKeyAxisBinding[PAD_BUTTON_COUNT]) { return FALSE; }
PADKeyAxisBinding* PADGetKeyAxisBindings(u32, u32* count) {
    *count = 0;
    return nullptr;
}
void PADClearKeyBindings(u32) {}
void PADSetKeyboardActive(u32, BOOL) {}

// ---------- names, rebinding, blocking

const char* PADGetButtonName(PADButton button) {
    for (const auto& [value, name] : kButtonNames) {
        if (value == button) {
            return name;
        }
    }
    return nullptr;
}

const char* PADGetAxisName(PADAxis axis) { return axis < PAD_AXIS_COUNT ? kAxisNames[axis] : nullptr; }
const char* PADGetAxisDirectionLabel(PADAxis axis) {
    return axis < PAD_AXIS_COUNT ? kAxisDirections[axis] : nullptr;
}
const char* PADGetNativeButtonName(u32 button) {
    return aurora_gamepad_button_string(static_cast<AuroraGamepadButton>(button));
}
const char* PADGetNativeAxisName(PADSignedNativeAxis axis) {
    return aurora_gamepad_axis_string(static_cast<AuroraGamepadAxis>(axis.nativeAxis));
}

s32 PADGetNativeButtonPressed(u32 port) {
    const int player = PlayerForPort(port);
    if (player < 0) {
        return -1;
    }
    for (int button = 0; button < AURORA_GAMEPAD_BUTTON_COUNT; ++button) {
        if (NativeButton(dol::input::state(player), button)) {
            return button;
        }
    }
    return -1;
}

PADSignedNativeAxis PADGetNativeAxisPulled(u32 port) {
    const int player = PlayerForPort(port);
    if (player >= 0) {
        for (int axis = 0; axis < AURORA_GAMEPAD_AXIS_COUNT; ++axis) {
            const int16_t value = NativeAxis(dol::input::state(player), axis);
            if (value >= 16384) {
                return {axis, AXIS_SIGN_POSITIVE};
            }
            // (the triggers have no negative direction)
            if (value <= -16384 && axis != AURORA_GAMEPAD_AXIS_LEFT_TRIGGER &&
                axis != AURORA_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                return {axis, AXIS_SIGN_NEGATIVE};
            }
        }
    }
    return {-1, AXIS_SIGN_POSITIVE};
}

void PADBlockInput(bool block) {
    if (g_blockPAD.exchange(block, std::memory_order_acq_rel) && !block) {
        g_suppressHeldOnRead = true;
    }
}

// ---------- rumble strength, sensors and the rest of the header

BOOL PADSetRumbleIntensity(u32 port, u16 low, u16 high) {
    Controller* controller = ControllerForPort(port);
    if (controller == nullptr) {
        return FALSE;
    }
    controller->rumbleLow = low;
    controller->rumbleHigh = high;
    return TRUE;
}

BOOL PADGetRumbleIntensity(u32 port, u16* low, u16* high) {
    const Controller* controller = ControllerForPort(port);
    if (controller == nullptr) {
        return FALSE;
    }
    *low = controller->rumbleLow;
    *high = controller->rumbleHigh;
    return TRUE;
}

BOOL PADSupportsRumbleIntensity(u32 port) { return PlayerForPort(port) >= 0; }

BOOL PADHasSensor(u32 port, PADSensorType sensor) {
    return PlayerForPort(port) >= 0 && sensor != PAD_SENSOR_INVALID && sensor != PAD_SENSOR_UNKNOWN;
}

BOOL PADSetSensorEnabled(u32 port, PADSensorType sensor, BOOL) { return PADHasSensor(port, sensor); }

// accelerometer in g, gyro in turns a second (libnx's units), three values
BOOL PADGetSensorData(u32 port, PADSensorType sensor, f32* data, int count) {
    const int player = PlayerForPort(port);
    if (player < 0 || count < 3) {
        return FALSE;
    }
    const dol::input::State& state = dol::input::state(player);
    const bool left = sensor == PAD_SENSOR_ACCEL_LEFT || sensor == PAD_SENSOR_GYRO_LEFT;
    const dol::input::Motion& motion = left ? state.left_motion : state.right_motion;
    if (!motion.valid) {
        return FALSE;
    }
    const bool gyro = sensor == PAD_SENSOR_GYRO || sensor == PAD_SENSOR_GYRO_LEFT || sensor == PAD_SENSOR_GYRO_RIGHT;
    std::memcpy(data, gyro ? motion.angular_velocity : motion.acceleration, 3 * sizeof(f32));
    return TRUE;
}

BOOL PADSetColor(u32, u8, u8, u8) { return FALSE; }
BOOL PADGetColor(u32, u8*, u8*, u8*) { return FALSE; }

PADBatteryState PADGetBatteryState(u32, f32* percent) {
    if (percent) {
        *percent = 0.0f;
    }
    return PAD_BATTERYSTATE_UNKNOWN;
}

PADControllerType PADGetControllerType(u32 port) {
    const int player = PlayerForPort(port);
    switch (player < 0 ? dol::input::Style::None : dol::input::state(player).style) {
    case dol::input::Style::ProController: return PAD_TYPE_SWITCH_PROCON;
    case dol::input::Style::Handheld:
    case dol::input::Style::JoyConPair: return PAD_TYPE_JOYCON_PAIR;
    case dol::input::Style::JoyConLeft: return PAD_TYPE_JOYCON_LEFT;
    case dol::input::Style::JoyConRight: return PAD_TYPE_JOYCON_RIGHT;
    case dol::input::Style::Other: return PAD_TYPE_STANDARD;
    default: return PAD_TYPE_UNKNOWN;
    }
}

PADControllerType PADGetControllerTypeForIndex(u32 index) {
    const int player = PlayerForIndex(index);
    for (u32 port = 0; port < PAD_CHANMAX; ++port) {
        if (player >= 0 && g_portPlayer[port] == player) {
            return PADGetControllerType(port);
        }
    }
    return PAD_TYPE_UNKNOWN;
}

// (one table serves every style, so a title's own defaults have nowhere to go)
void PADSetDefaultMapping(const PADDefaultMapping*, PADControllerType) {}

// ---------- SI: which ports have a pad

u32 SIProbe(s32 chan) {
    return chan >= 0 && PlayerForPort(static_cast<u32>(chan)) >= 0 ? SI_GC_CONTROLLER : SI_ERROR_NO_RESPONSE;
}
