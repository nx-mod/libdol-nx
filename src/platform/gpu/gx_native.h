#pragma once

// libdol's own GX renderer, fed what the game feeds the hardware
// (docs/deko3d.md, "How it replaces Aurora").
//
// The game's FIFO bytes - the gather pipe's, in the order written - and the
// display lists it calls go to one wiinx::gx::CommandProcessor over guest
// memory; its draws go to dk::draw_gx on the EFB pass that is open. Built on
// the Switch with WIINX_NATIVE_GX; off, Aurora draws as before.

#include "wiinx/format/gx/state.hpp"

#include <cstddef>
#include <cstdint>

namespace dol::gx_native {

// FIFO bytes in the order the game wrote them; a packet cut off at the end
// waits for the rest
void write(const uint8_t* data, size_t size);
// a display list at a guest address
void call_list(uint32_t address, uint32_t size);

// The EFB pass the draws go into, `width` x `height` (the target's size; GX's
// 640-wide EFB space is scaled onto it). Draws with no pass open are counted
// and dropped.
void begin_efb(uint32_t width, uint32_t height);
void end_efb();

// A frame on the screen: the screen's next image as the EFB, with a depth
// buffer of its size, both cleared as GX's copy clear registers say; then
// presented. (The EFB is the screen for now - one EFB a frame, the display
// copy implied; copies to textures come later.)
void begin_frame();
void end_frame();

wiinx::gx::State& state();

struct Counts {
    uint32_t draws = 0;    // made
    uint32_t refused = 0;  // the shaders do not do what the state asks yet
    uint32_t dropped = 0;  // no EFB pass open
    uint32_t stalled = 0;  // bytes the command processor would not take
};
// this frame's, and reset
Counts take_counts();

} // namespace dol::gx_native
