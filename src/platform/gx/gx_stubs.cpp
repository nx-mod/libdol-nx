#include "gx_internal.h"

#include <cstdio>
#include <cstring>

#if defined(__SWITCH__)
#include <atomic>
#include <chrono>
// Last host call the main thread entered that can block on the GPU/Aurora;
// read by the Switch freeze watchdog in main.cpp.
extern std::atomic<const char*> g_switchHostPhase;
// How long the guest waits for the GP to drain, reported per frame by [vi].
extern std::atomic<uint32_t> g_gxDrawDoneCalls;
extern std::atomic<uint64_t> g_gxDrawDoneUs;
#define SWITCH_PHASE(name) g_switchHostPhase.store(name, std::memory_order_relaxed)
#else
#define SWITCH_PHASE(name) ((void)0)
#endif
#include "runtime_log.h"
#include "abi_bridge.h"
#include "native_bindings.h"
#include "native_guest_return.h"
#if defined(WIINX_NATIVE_GX)
#include "../gpu/gx_native.h"
#endif

extern "C" void __GXSetSUTexRegs();

// ============================================================================
// FIFO Write Helpers
// ============================================================================

#if defined(WIINX_NATIVE_GX)
namespace { void NativeFifoWrite(uint32_t value, uint32_t size); }
#endif
extern "C" void GX_HLE_FIFO_WriteFloat(float val) {
    u32 raw; std::memcpy(&raw, &val, 4);
#if defined(WIINX_NATIVE_GX)
    NativeFifoWrite(raw, 4);
#else
    try { HleFifoWrite(raw, 4); } catch (...) { RT_LOGF(RT_TAG_GX, "FIFO write float failed\n"); }
#endif
}

#if defined(WIINX_NATIVE_GX)
// libdol's own renderer takes the gather pipe's bytes as they are (gx_native.h)
// - unless GXBeginDisplayList is recording, when they go into the list, as the
// burst path already does: run here, a recorded list would have drawn at once
// and been empty when the game called it.
namespace {
void NativeFifoWrite(uint32_t value, uint32_t size) {
    if (IsDisplayListActive()) {
        WriteDisplayListData(value, size);
        return;
    }
    const uint8_t bytes[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                              static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    dol::gx_native::write(bytes + (4 - size), size);
}
}  // namespace
extern "C" void GX_HLE_FIFO_Write32(uint32_t val) { NativeFifoWrite(val, 4); }
extern "C" void GX_HLE_FIFO_Write16(uint16_t val) { NativeFifoWrite(val, 2); }
extern "C" void GX_HLE_FIFO_Write8(uint8_t val) { NativeFifoWrite(val, 1); }
#else
extern "C" void GX_HLE_FIFO_Write32(uint32_t val) { HleFifoWrite(val, 4); }
extern "C" void GX_HLE_FIFO_Write16(uint16_t val) { HleFifoWrite(static_cast<u32>(val), 2); }
extern "C" void GX_HLE_FIFO_Write8(uint8_t val) { HleFifoWrite(static_cast<u32>(val), 1); }
#endif

extern "C" void GX__SetDrawSync_8016ed08(uint32_t token) {
    (void)token;
    try { uint32_t gd = GuestGxData(); if (gd) {
        if (Memory::Read32(gd + 0x5FCu)) GX__SetDirtyState_8016ee78();
        Memory::Write16(gd + 2, 0);
    } } catch (...) {}
}

extern "C" void GX__SetDrawSync_8016e9fc(uint32_t token) { GX__SetDrawSync_8016ed08(token); }
PPC_NATIVE_OVERRIDE_VOID(8016e9fc, GX__SetDrawSync_8016e9fc, (uint32_t token), (token));

extern "C" void GX__FinishInterruptHandler_8016ed94() {
    try {
        uint32_t gd = GuestGxData();
        if (gd) Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(Memory::Read16(gd + 0x0Au) | 0x0008u));
        if (Gx().drawDone) Memory::Write8(Gx().drawDone, 1);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016ed94, GX__FinishInterruptHandler_8016ed94, (), ());

extern "C" void GX__DrawDone_8016eab0() {
    if (Gx().drawDone) try { Memory::Write8(Gx().drawDone, 0); } catch (...) {}
    SWITCH_PHASE("GX__DrawDone");
#if defined(__SWITCH__)
    const auto drawDoneStart = std::chrono::steady_clock::now();
#endif
#if defined(WIINX_NATIVE_GX)
    // (GXSetDrawDone ends in GXFlush: pending state goes out first)
    try {
        const uint32_t gd = GuestGxData();
        if (gd && Memory::Read32(gd + 0x5FCu)) GX__SetDirtyState_8016ee78();
    } catch (...) {}
    dol::gx_native::flush();  // every draw so far made; the GPU keeps their order
#else
    GXDrawDone();
#endif
    GX__FinishInterruptHandler_8016ed94();
#if defined(__SWITCH__)
    g_gxDrawDoneCalls.fetch_add(1, std::memory_order_relaxed);
    g_gxDrawDoneUs.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - drawDoneStart)
                                  .count()),
        std::memory_order_relaxed);
#endif
    SWITCH_PHASE("GX__DrawDone done");
}
PPC_NATIVE_OVERRIDE_VOID(8016eab0, GX__DrawDone_8016eab0, (), ());

extern "C" void GX__PixModeSync_8016eb70() {
    try { uint32_t gd = GuestGxData(); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
    GXPixModeSync();
}
PPC_NATIVE_OVERRIDE_VOID(8016eb70, GX__PixModeSync_8016eb70, (), ());

// ============================================================================
// Hardware Revision / Thread Query - No-ops
// ============================================================================

extern "C" void __GX__InitRevisionBits_8016b720() {}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code sets this (docs/deko3d.md)
PPC_NATIVE_OVERRIDE_VOID(8016b720, __GX__InitRevisionBits_8016b720, (), ());
#endif

// ============================================================================
// Texture State Management - Aurora handles internally
// ============================================================================

extern "C" void __GX__SetSUTexRegs_801712f0() {
    __GXSetSUTexRegs();
    try { uint32_t gd = GuestGxData(); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code sets this (docs/deko3d.md)
PPC_NATIVE_OVERRIDE_VOID(801712f0, __GX__SetSUTexRegs_801712f0, (), ());
#endif

extern "C" void __GX__SetTmemConfig_80171458(uint32_t mode) {
    // TMEM layout configuration - Aurora manages internally
    (void)mode;
}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code sets this (docs/deko3d.md)
PPC_NATIVE_OVERRIDE_VOID(80171458, __GX__SetTmemConfig_80171458, (uint32_t mode), (mode));
#endif

extern "C" void __GX__FlushTextureState_80171c28() {
    // BP texture state flush - Aurora handles via API
    try { uint32_t gd = GuestGxData(); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code sets this (docs/deko3d.md)
PPC_NATIVE_OVERRIDE_VOID(80171c28, __GX__FlushTextureState_80171c28, (), ());
#endif

// ============================================================================
// Copy Configuration - No-ops for features Aurora doesn't use
// ============================================================================

extern "C" void GX__SetDispCopyFrame2Field_8016f5f8(uint32_t f) {
    GXSetDispCopyFrame2Field(f);
    try {
        const uint32_t gd = GuestGxData();
        if (gd) {
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFCFFFu) | ((f & 3u) << 12));
            Memory::Write32(gd + 0x24Cu, Memory::Read32(gd + 0x24Cu) & 0xFFFFCFFFu);
        }
    } catch (...) {}
}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code sets this (docs/deko3d.md)
PPC_NATIVE_OVERRIDE_VOID(8016f5f8, GX__SetDispCopyFrame2Field_8016f5f8, (uint32_t f), (f));
#endif

extern "C" void GX__SetCopyClamp_8016f618(uint32_t c) {
    GXSetCopyClamp(static_cast<GXFBClamp>(c));
    try {
        const uint32_t gd = GuestGxData();
        if (gd) {
            const uint32_t clamp = c & 3u;
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFFFFCu) | clamp);
            Memory::Write32(gd + 0x24Cu, (Memory::Read32(gd + 0x24Cu) & 0xFFFFFFFCu) | clamp);
        }
    } catch (...) {}
}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code sets this (docs/deko3d.md)
PPC_NATIVE_OVERRIDE_VOID(8016f618, GX__SetCopyClamp_8016f618, (uint32_t c), (c));
#endif

extern "C" void GX__ClearBoundingBox_8016fecc() {
    GXClearBoundingBox();
    try {
        const uint32_t gd = GuestGxData();
        if (gd) Memory::Write16(gd + 2, 0);
    } catch (...) {}
}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code sets this (docs/deko3d.md)
PPC_NATIVE_OVERRIDE_VOID(8016fecc, GX__ClearBoundingBox_8016fecc, (), ());
#endif

// ============================================================================
// FIFO/State Management - No-ops
// ============================================================================

extern "C" void GX__SetDirtyState_8016ee78() {
#if defined(WIINX_NATIVE_GX)
    // The game's own __GXSetDirtyState: what GXSetProjection, GXSetViewport and
    // the other deferred setters left pending is written to the FIFO, where the
    // renderer reads it. Natives call this before work that needs the state
    // current (a display list, a draw sync, recording a list). Only clearing
    // the bits, as Aurora's state tracking allowed, dropped every pending
    // projection: models drawn from display lists kept the 2D one and were
    // clipped away whole (missing models, a black flyover and race intro).
    try {
        const uint32_t gd = GuestGxData();
        if (!gd || Memory::Read32(gd + 0x5FCu) == 0) {
            return;
        }
        static const uint32_t target = [] {
            const uint32_t bound = NativeBindings::Resolve(0x8016ee78u);
            return bound ? bound : NativeBindings::IsReferenceGame() ? 0x8016ee78u : 0u;
        }();
        CpuContext* cpu = TryGetCpuContext();
        if (target && cpu) {
            // (the binding's return address in lr, as wiinx::Host::call does: a
            // callee dispatched as a jump resumes through it)
            if (const uint32_t resume = RuntimeNativeGuestReturn::Current(); resume != 0) {
                cpu->lr = resume;
            }
            InvokeIndirectCpu(target, cpu);
        }
    } catch (...) {}
#else
    try { uint32_t gd = GuestGxData(); if (gd) Memory::Write32(gd + 0x5FCu, 0); } catch (...) {}
#endif
}
#if !defined(WIINX_NATIVE_GX)  // the game's own SDK code sets this (docs/deko3d.md)
PPC_NATIVE_OVERRIDE_VOID(8016ee78, GX__SetDirtyState_8016ee78, (), ());
#endif

extern "C" void GX__ResetWriteGatherPipe_8016e6b0() {
    // WPAR reset - not needed on host
}
PPC_NATIVE_OVERRIDE_VOID(8016e6b0, GX__ResetWriteGatherPipe_8016e6b0, (), ());
