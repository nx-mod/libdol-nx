// The Switch's controllers on libnx HID (native_input.h; docs/native.md,
// section 2).
//
// Eight NPad slots, the handheld pair read as player 1's as well. A slot's
// style is read every frame, because a Joy-Con pair can be split or the
// console docked while a game runs; its motion sensors and vibration devices
// are set up again whenever it changes.

#include "native_input.h"

#include <switch.h>

#include <cstring>

namespace dol::input {
namespace {

struct Slot {
    PadState pad{};
    State state;
    uint32_t style_tags = 0; // the HidNpadStyleTag bits the sensors were set up for
    HidSixAxisSensorHandle sensors[2] = {};
    int sensor_count = 0;
    HidVibrationDeviceHandle vibration[2] = {};
    int vibration_count = 0;
};

Slot g_slots[kPlayers];

const HidNpadIdType kIds[kPlayers] = {
    HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
    HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
};

Style style_of(uint32_t tags) {
    if (tags & HidNpadStyleTag_NpadHandheld)
        return Style::Handheld;
    if (tags & HidNpadStyleTag_NpadFullKey)
        return Style::ProController;
    if (tags & HidNpadStyleTag_NpadJoyDual)
        return Style::JoyConPair;
    if (tags & HidNpadStyleTag_NpadJoyLeft)
        return Style::JoyConLeft;
    if (tags & HidNpadStyleTag_NpadJoyRight)
        return Style::JoyConRight;
    return tags ? Style::Other : Style::None;
}

// the one HidNpadStyleTag the sensor and vibration handles are asked for
HidNpadStyleTag handle_style(Style style) {
    switch (style) {
    case Style::Handheld: return HidNpadStyleTag_NpadHandheld;
    case Style::JoyConPair: return HidNpadStyleTag_NpadJoyDual;
    case Style::JoyConLeft: return HidNpadStyleTag_NpadJoyLeft;
    case Style::JoyConRight: return HidNpadStyleTag_NpadJoyRight;
    default: return HidNpadStyleTag_NpadFullKey;
    }
}

// the slot's HID id: player 1 reads the handheld pair while it is attached
HidNpadIdType id_of(int player, Style style) {
    return player == 0 && style == Style::Handheld ? HidNpadIdType_Handheld : kIds[player];
}

void stop_devices(Slot& slot) {
    for (int index = 0; index < slot.sensor_count; index++)
        hidStopSixAxisSensor(slot.sensors[index]);
    slot.sensor_count = 0;
    slot.vibration_count = 0;
}

// the slot's motion sensors and vibration devices, for the style it has now:
// two for a pair or the handheld (left, right), one otherwise
void start_devices(int player, Slot& slot) {
    Style style = slot.state.style;
    HidNpadIdType id = id_of(player, style);
    HidNpadStyleTag tag = handle_style(style);
    int count = style == Style::JoyConPair || style == Style::Handheld ? 2 : 1;

    stop_devices(slot);
    if (style == Style::None)
        return;
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(slot.sensors, count, id, tag))) {
        slot.sensor_count = count;
        for (int index = 0; index < count; index++)
            hidStartSixAxisSensor(slot.sensors[index]);
    }
    if (R_SUCCEEDED(hidInitializeVibrationDevices(slot.vibration, count, id, tag)))
        slot.vibration_count = count;
}

void read_motion(const HidSixAxisSensorHandle& handle, Motion& motion) {
    HidSixAxisSensorState sensor{};

    motion.valid = hidGetSixAxisSensorStates(handle, &sensor, 1) > 0;
    if (!motion.valid)
        return;
    motion.acceleration[0] = sensor.acceleration.x;
    motion.acceleration[1] = sensor.acceleration.y;
    motion.acceleration[2] = sensor.acceleration.z;
    motion.angular_velocity[0] = sensor.angular_velocity.x;
    motion.angular_velocity[1] = sensor.angular_velocity.y;
    motion.angular_velocity[2] = sensor.angular_velocity.z;
}

uint32_t buttons_of(uint64_t held) {
    uint32_t buttons = 0;

    // (positional: the bottom face button is South whatever it is labelled)
    if (held & HidNpadButton_B) buttons |= ButtonSouth;
    if (held & HidNpadButton_A) buttons |= ButtonEast;
    if (held & HidNpadButton_Y) buttons |= ButtonWest;
    if (held & HidNpadButton_X) buttons |= ButtonNorth;
    if (held & HidNpadButton_Minus) buttons |= ButtonMinus;
    if (held & HidNpadButton_Plus) buttons |= ButtonPlus;
    if (held & HidNpadButton_StickL) buttons |= ButtonLeftStick;
    if (held & HidNpadButton_StickR) buttons |= ButtonRightStick;
    if (held & HidNpadButton_L) buttons |= ButtonL;
    if (held & HidNpadButton_R) buttons |= ButtonR;
    if (held & HidNpadButton_ZL) buttons |= ButtonZL;
    if (held & HidNpadButton_ZR) buttons |= ButtonZR;
    if (held & HidNpadButton_Up) buttons |= ButtonDpadUp;
    if (held & HidNpadButton_Down) buttons |= ButtonDpadDown;
    if (held & HidNpadButton_Left) buttons |= ButtonDpadLeft;
    if (held & HidNpadButton_Right) buttons |= ButtonDpadRight;
    if (held & HidNpadButton_LeftSL) buttons |= ButtonLeftSL;
    if (held & HidNpadButton_LeftSR) buttons |= ButtonLeftSR;
    if (held & HidNpadButton_RightSL) buttons |= ButtonRightSL;
    if (held & HidNpadButton_RightSR) buttons |= ButtonRightSR;
    return buttons;
}

const State kNone{};

} // namespace

void initialize() {
    // every style, so a single Joy-Con held sideways is a pad of its own
    padConfigureInput(kPlayers, HidNpadStyleSet_NpadStandard);
    for (int player = 0; player < kPlayers; player++) {
        if (player == 0)
            padInitialize(&g_slots[player].pad, HidNpadIdType_No1, HidNpadIdType_Handheld);
        else
            padInitialize(&g_slots[player].pad, kIds[player]);
    }
}

void update() {
    static bool initialized;

    // (set up at the first frame's read: nothing calls initialize() before)
    if (!initialized) {
        initialized = true;
        initialize();
    }
    for (int player = 0; player < kPlayers; player++) {
        Slot& slot = g_slots[player];
        uint32_t previous = slot.state.buttons;

        padUpdate(&slot.pad);
        uint32_t tags = padIsConnected(&slot.pad) ? padGetStyleSet(&slot.pad) : 0;
        if (tags != slot.style_tags) {
            slot.style_tags = tags;
            slot.state.style = style_of(tags);
            start_devices(player, slot);
        }
        if (slot.state.style == Style::None) {
            slot.state = State{};
            continue;
        }
        uint64_t held = padGetButtons(&slot.pad);
        HidAnalogStickState left = padGetStickPos(&slot.pad, 0);
        HidAnalogStickState right = padGetStickPos(&slot.pad, 1);

        slot.state.buttons = buttons_of(held);
        slot.state.pressed = slot.state.buttons & ~previous;
        slot.state.axes[AxisLeftX] = static_cast<int16_t>(left.x);
        slot.state.axes[AxisLeftY] = static_cast<int16_t>(left.y);
        slot.state.axes[AxisRightX] = static_cast<int16_t>(right.x);
        slot.state.axes[AxisRightY] = static_cast<int16_t>(right.y);
        slot.state.axes[AxisLeftTrigger] = (held & HidNpadButton_ZL) ? 32767 : 0;
        slot.state.axes[AxisRightTrigger] = (held & HidNpadButton_ZR) ? 32767 : 0;
        // (two sensors: left half, right half; one: the right, as a Pro
        // Controller's or a single Joy-Con's)
        if (slot.sensor_count == 2) {
            read_motion(slot.sensors[0], slot.state.left_motion);
            read_motion(slot.sensors[1], slot.state.right_motion);
        } else if (slot.sensor_count == 1) {
            slot.state.left_motion = Motion{};
            read_motion(slot.sensors[0], slot.state.right_motion);
        }
    }
}

const State& state(int player) { return player >= 0 && player < kPlayers ? g_slots[player].state : kNone; }

void rumble(int player, float low, float high) {
    if (player < 0 || player >= kPlayers || !g_slots[player].vibration_count)
        return;
    Slot& slot = g_slots[player];
    HidVibrationValue values[2];

    for (int index = 0; index < slot.vibration_count; index++) {
        // the console's own default frequencies: 160 Hz low, 320 Hz high
        values[index].amp_low = low;
        values[index].freq_low = 160.0f;
        values[index].amp_high = high;
        values[index].freq_high = 320.0f;
    }
    hidSendVibrationValues(slot.vibration, values, slot.vibration_count);
}

const char* name(int player) {
    switch (state(player).style) {
    case Style::Handheld: return "Handheld";
    case Style::ProController: return "Pro Controller";
    case Style::JoyConPair: return "Joy-Con (L/R)";
    case Style::JoyConLeft: return "Joy-Con (L)";
    case Style::JoyConRight: return "Joy-Con (R)";
    case Style::Other: return "Controller";
    default: return "None";
    }
}

} // namespace dol::input
