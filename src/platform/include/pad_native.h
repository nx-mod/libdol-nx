#pragma once

// A GameCube port's controller, read by its own buttons and axes rather than
// through the PAD mapping: what rebinding listens to, what Dolphin-style
// expressions read, and what an L/R binding checks for a full pull.
//
// Buttons and axes keep Aurora's numbering (AURORA_GAMEPAD_BUTTON_*,
// AURORA_GAMEPAD_AXIS_*), which saved mappings use. On the Switch they are
// answered from native input (pad_sdk_switch.cpp), elsewhere from Aurora's
// gamepads (pad_native.cpp) - so nothing that calls these needs an Aurora
// gamepad in hand.

#include <cstdint>

// held now; false for a port with no controller
bool PADNativeButtonHeld(uint32_t port, uint32_t button);
// -32768..32767, sticks down-positive, triggers 0..32767; 0 with no controller
int16_t PADNativeAxisValue(uint32_t port, uint32_t axis);
// which controller is on the port, to notice it being swapped; 0 for none
uint32_t PADNativeControllerId(uint32_t port);
