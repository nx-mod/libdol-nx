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
#if defined(WIINX_NATIVE_GX)
#include "../gpu/gx_native.h"
#endif

extern "C" void __GXSetSUTexRegs();

// ============================================================================
// FIFO Write Helpers
// ============================================================================

#if !defined(WIINX_NATIVE_GX)
#if defined(__SWITCH__)
extern int g_gxTraceBegins;
void SwitchBootLogExternal(const char* text) noexcept;
// The raw words of the first eight draws' vertices (see GX__Begin), as hex and
// as floats: one line per draw, flushed when the next draw starts.
static void TraceVertexWord(uint32_t val) {
    static int draw = -1, words = 0, at = 0;
    static char line[400];
    if (g_gxTraceBegins == 0 || g_gxTraceBegins > 8) return;
    if (draw != g_gxTraceBegins) {
        if (at > 0) SwitchBootLogExternal(line);
        draw = g_gxTraceBegins; words = 0;
        at = std::snprintf(line, sizeof(line), "[gx] draw #%d words:", draw - 1);
    }
    if (words++ < 24 && at < (int)sizeof(line) - 24) {
        float f; std::memcpy(&f, &val, 4);
        at += std::snprintf(line + at, sizeof(line) - at, " %08X(%.4g)", val, f);
    }
}
#else
static void TraceVertexWord(uint32_t) {}
#endif
#endif

extern "C" void GX_HLE_FIFO_WriteFloat(float val) {
    u32 raw; std::memcpy(&raw, &val, 4);
#if defined(WIINX_NATIVE_GX)
    const uint8_t bytes[4] = {static_cast<uint8_t>(raw >> 24), static_cast<uint8_t>(raw >> 16),
                              static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw)};
    dol::gx_native::write(bytes, 4);
#else
    TraceVertexWord(raw);
    try { HleFifoWrite(raw, 4); } catch (...) { RT_LOGF(RT_TAG_GX, "FIFO write float failed\n"); }
#endif
}

#if defined(WIINX_NATIVE_GX)
// libdol's own renderer takes the gather pipe's bytes as they are (gx_native.h)
extern "C" void GX_HLE_FIFO_Write32(uint32_t val) {
    const uint8_t bytes[4] = {static_cast<uint8_t>(val >> 24), static_cast<uint8_t>(val >> 16),
                              static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val)};
    dol::gx_native::write(bytes, 4);
}
extern "C" void GX_HLE_FIFO_Write16(uint16_t val) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val)};
    dol::gx_native::write(bytes, 2);
}
extern "C" void GX_HLE_FIFO_Write8(uint8_t val) { dol::gx_native::write(&val, 1); }
#else
extern "C" void GX_HLE_FIFO_Write32(uint32_t val) { TraceVertexWord(val); HleFifoWrite(val, 4); }
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
    GXDrawDone(); GX__FinishInterruptHandler_8016ed94();
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
PPC_NATIVE_OVERRIDE_VOID(8016b720, __GX__InitRevisionBits_8016b720, (), ());

// ============================================================================
// Texture State Management - Aurora handles internally
// ============================================================================

extern "C" void __GX__SetSUTexRegs_801712f0() {
    __GXSetSUTexRegs();
    try { uint32_t gd = GuestGxData(); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(801712f0, __GX__SetSUTexRegs_801712f0, (), ());

extern "C" void __GX__SetTmemConfig_80171458(uint32_t mode) {
    // TMEM layout configuration - Aurora manages internally
    (void)mode;
}
PPC_NATIVE_OVERRIDE_VOID(80171458, __GX__SetTmemConfig_80171458, (uint32_t mode), (mode));

extern "C" void __GX__FlushTextureState_80171c28() {
    // BP texture state flush - Aurora handles via API
    try { uint32_t gd = GuestGxData(); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(80171c28, __GX__FlushTextureState_80171c28, (), ());

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
PPC_NATIVE_OVERRIDE_VOID(8016f5f8, GX__SetDispCopyFrame2Field_8016f5f8, (uint32_t f), (f));

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
PPC_NATIVE_OVERRIDE_VOID(8016f618, GX__SetCopyClamp_8016f618, (uint32_t c), (c));

extern "C" void GX__ClearBoundingBox_8016fecc() {
    GXClearBoundingBox();
    try {
        const uint32_t gd = GuestGxData();
        if (gd) Memory::Write16(gd + 2, 0);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016fecc, GX__ClearBoundingBox_8016fecc, (), ());

// ============================================================================
// FIFO/State Management - No-ops
// ============================================================================

extern "C" void GX__SetDirtyState_8016ee78() {
    try { uint32_t gd = GuestGxData(); if (gd) Memory::Write32(gd + 0x5FCu, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016ee78, GX__SetDirtyState_8016ee78, (), ());

extern "C" void GX__ResetWriteGatherPipe_8016e6b0() {
    // WPAR reset - not needed on host
}
PPC_NATIVE_OVERRIDE_VOID(8016e6b0, GX__ResetWriteGatherPipe_8016e6b0, (), ());
