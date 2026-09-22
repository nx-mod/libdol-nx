// What libwii-nx needs from the program running the game.
//
// The library never owns the CPU or guest memory: the embedding runtime
// (wiicompiled-nx) does, and hands them over once at startup with set_host().
// That keeps every module testable on its own and free of any one runtime's
// headers.
#pragma once

#include "wiinx/core/types.hpp"

namespace wiinx {

// The guest CPU's state. Opaque here: only the host knows its layout.
struct Cpu;

struct Host {
    // Guest memory as one flat host mapping: guest address A lives at
    // memory + A. Every alias of a physical byte resolves the same way.
    u8* memory = nullptr;

    u32 (*gpr)(Cpu* cpu, int index) = nullptr;
    void (*set_gpr)(Cpu* cpu, int index, u32 value) = nullptr;

    // Calls guest function `target` as a guest call would, with the link
    // register set to `return_address` for callees that look at it.
    void (*call)(Cpu* cpu, GuestAddr target, GuestAddr return_address) = nullptr;

    // Tells the graphics layer that native code rewrote [addr, addr + size),
    // the way a DMA would - textures sampled from there must be re-read.
    void (*notify_write)(GuestAddr addr, u32 size) = nullptr;
};

void set_host(const Host& host) noexcept;
const Host& host() noexcept;

}  // namespace wiinx
