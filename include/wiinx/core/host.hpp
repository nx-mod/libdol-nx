// What libwii-nx needs from the program running the game.
//
// Natives never own the CPU or guest memory: the CPU runtime does, and hands
// them over once at startup with set_host().
// That keeps every module testable on its own and free of any one runtime's
// headers.
#pragma once

#include "wiinx/core/types.hpp"

namespace wiinx {

// The guest CPU's state. Opaque here: only the host knows its layout.
struct Cpu;

struct Host {
    // Where a run of guest memory really is, or nullptr when it is not
    // readable. A runtime may keep parts of the guest's address space outside
    // any flat mapping - aliases, sparse regions, memory a game never
    // configured - so natives ask rather than compute: a pointer worked out by
    // adding to a base is right until it is not, and then it faults.
    u8* (*pointer)(GuestAddr addr, u32 size) = nullptr;
    // A flat mapping, when the runtime has one and it covers every address:
    // used only when `pointer` is absent (the library's own tests set this).
    u8* memory = nullptr;
    // Whether [addr, addr + size) is real guest memory natives may touch.
    bool (*valid)(GuestAddr addr, u32 size) = nullptr;

    u32 (*gpr)(Cpu* cpu, int index) = nullptr;
    void (*set_gpr)(Cpu* cpu, int index, u32 value) = nullptr;

    // Floating-point registers, as doubles: that is how the ABI passes a float
    // argument, and how a float return leaves a function. Absent in a host
    // that has no use for them, so a native that reads floats checks first.
    f64 (*fpr)(Cpu* cpu, int index) = nullptr;
    void (*set_fpr)(Cpu* cpu, int index, f64 value) = nullptr;

    // Calls guest function `target` as a guest call would. Arguments are
    // whatever set_gpr put in r3.. beforehand. The host chooses the link
    // register: only it knows where the calling native is bound in this game.
    void (*call)(Cpu* cpu, GuestAddr target) = nullptr;

    // Tells the graphics layer that native code rewrote [addr, addr + size),
    // the way a DMA would - textures sampled from there must be re-read.
    void (*notify_write)(GuestAddr addr, u32 size) = nullptr;

    // Optional: where natives send an occasional status line.
    void (*log)(const char* line) = nullptr;
};

void set_host(const Host& host) noexcept;
const Host& host() noexcept;

}  // namespace wiinx
