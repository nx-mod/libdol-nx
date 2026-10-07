// pad_native.h through Aurora's gamepads, off the Switch (which answers it from
// native input in pad_sdk_switch.cpp).
#if !defined(__SWITCH__)
#include "pad_native.h"

#include <aurora/gamepad.h>
#include <dolphin/pad.h>

namespace {

AuroraGamepad* GamepadForPort(uint32_t port) {
    const s32 index = PADGetIndexForPort(port);
    return index < 0 ? nullptr : PADGetGamepadForIndex(static_cast<u32>(index));
}

}  // namespace

bool PADNativeButtonHeld(uint32_t port, uint32_t button) {
    AuroraGamepad* gamepad = GamepadForPort(port);
    return gamepad != nullptr && button < AURORA_GAMEPAD_BUTTON_COUNT &&
           aurora_gamepad_button(gamepad, static_cast<AuroraGamepadButton>(button));
}

int16_t PADNativeAxisValue(uint32_t port, uint32_t axis) {
    AuroraGamepad* gamepad = GamepadForPort(port);
    return gamepad != nullptr && axis < AURORA_GAMEPAD_AXIS_COUNT
               ? aurora_gamepad_axis(gamepad, static_cast<AuroraGamepadAxis>(axis))
               : 0;
}

uint32_t PADNativeControllerId(uint32_t port) {
    AuroraGamepad* gamepad = GamepadForPort(port);
    return gamepad != nullptr ? static_cast<uint32_t>(aurora_gamepad_id(gamepad)) : 0;
}
#endif
