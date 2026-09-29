// gx_init.cpp - GX Initialization and FIFO Management
#include "gx_internal.h"
#include "runtime_log.h"

#include <cstdio>

// Forward declarations for HLE functions used by GXInit
extern "C" void __GX__FifoInit_8016d180();
extern "C" void __GX__PEInit_8016ee14();
extern "C" void __GX__SetTmemConfig_80171458(uint32_t mode);
extern "C" void GX__InitFifoBase_8016c7c8(uint32_t fa, uint32_t ba, uint32_t s);
// GX__SetCPUFifo_8016c94c is already declared in gx_internal.h.
extern "C" void GX__SetGPFifo_8016cb2c(uint32_t fa);
extern "C" GXFifoObj* GXInit(void* base, u32 size);

// ============================================================================
// GXInit
// ============================================================================


/**
 * GXInit HLE - Initialize the Graphics subsystem
 * This replaces the translated function that writes to MMIO addresses.
 */
const GxGlobals& Gx() {
    using RuntimeGuestGlobals::find;
    static const GxGlobals globals{
        find("gx.__GXData"), find("gx.gxData"), find("gx.FifoObj"),
        find("gx.CPUFifo"), find("gx.GPFifo"),
        find("gx.CPUFifoReady"), find("gx.GPFifoReady"),
        find("gx.GXOverflowSuspendInProgress"), find("gx.__GXCurrentThread"),
        find("gx.FinishQueue"), find("gx.DrawDone"),
        find("gx.DisplayListFifo"), find("gx.__savedGXdata"), find("gx.OldCPUFifo"),
        find("gx.GXCPInterruptHandler"), find("gx.GXTokenInterruptHandler"),
        find("gx.GXFinishInterruptHandler"),
    };
    return globals;
}

extern "C" uint32_t GX__Init_8016b850(uint32_t fifoBase, uint32_t fifoSize)
{
    const GxGlobals& gx = Gx();
    const uint32_t kFifoObjAddr = gx.fifoObj;
    const uint32_t kGXDataAddr = gx.gxData;
    constexpr uint32_t kGXDataSize = 0x600u;

    GXInit(GuestToHostPtr(fifoBase, fifoSize), fifoSize);

    // Initialize GXData structure in guest memory
    if (kGXDataAddr != 0 && gx.gxDataPtr != 0) try {
        for (uint32_t offset = 0; offset < kGXDataSize; offset += 4) {
            Memory::Write32(kGXDataAddr + offset, 0);
        }
        Memory::Write8(kGXDataAddr + 0x5f8, 0);
        Memory::Write8(kGXDataAddr + 0x5f9, 1);
        Memory::Write8(kGXDataAddr + 0x5fa, 1);
        Memory::Write32(kGXDataAddr + 0x5e4, 0);
        Memory::Write32(kGXDataAddr + 0x5e8, 0);
        Memory::Write32(kGXDataAddr + 0x5fc, 0);
        Memory::Write32(gx.gxDataPtr, kGXDataAddr);
        // GXData+0x5F8 was just cleared; drop any stale recording shadow with it.
        BeginDisplayListRecording(0, 0);
    } catch (const ::Memory::AccessViolation& e) {
        RT_LOGF(RT_TAG_GX, "Memory access violation during GXData init: %s\n", e.what());
    }

    __GX__FifoInit_8016d180();
    if (kFifoObjAddr != 0) {
        GX__InitFifoBase_8016c7c8(kFifoObjAddr, fifoBase, fifoSize);
        GX__SetCPUFifo_8016c94c(kFifoObjAddr);
        GX__SetGPFifo_8016cb2c(kFifoObjAddr);
    }
    __GX__PEInit_8016ee14();

    try {
        uint32_t gd = GuestGxData();
        if (gd) {
            Memory::Write32(gd + 0x254, 0);
            Memory::Write32(gd + 0x174, 0x0f0000ff);
            Memory::Write32(gd + 0x7c, 0x22000000);
            Memory::Write32(gd + 0x170, 0x27000000);
            const uint32_t baseRegs[] = {0x30, 0x38};
            for (int i = 0; i < 2; ++i) {
                uint32_t r = baseRegs[i];
                Memory::Write32(gd + 0x108 + i * 0x10, r << 24);
                Memory::Write32(gd + 0x128 + i * 0x10, (r + 1) << 24);
                Memory::Write32(gd + 0x10c + i * 0x10, (r + 2) << 24);
                Memory::Write32(gd + 0x12c + i * 0x10, (r + 3) << 24);
                Memory::Write32(gd + 0x110 + i * 0x10, (r + 4) << 24);
                Memory::Write32(gd + 0x130 + i * 0x10, (r + 5) << 24);
                Memory::Write32(gd + 0x114 + i * 0x10, (r + 6) << 24);
                Memory::Write32(gd + 0x134 + i * 0x10, (r + 7) << 24);
            }
        }
    } catch (...) {}

    __GX__SetTmemConfig_80171458(2);

    RT_LOGF(RT_TAG_GX, "GX initialized, FIFO at 0x%08X (base=0x%08X size=0x%08X)\n",
            kFifoObjAddr, fifoBase, fifoSize);

    return kFifoObjAddr;
}
PPC_NATIVE_OVERRIDE(8016b850, GX__Init_8016b850, uint32_t, (uint32_t fifoBase, uint32_t fifoSize), (fifoBase, fifoSize));

// ============================================================================
// FIFO Management
// ============================================================================

extern "C" void GX__InitFifoBase_8016c7c8(uint32_t fa, uint32_t ba, uint32_t s) { GXInitFifoBase((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj)), GuestToHostPtr(ba, s), s); }

extern "C" void GX__SetCPUFifo_8016c94c(uint32_t fa) { GXSetCPUFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8016c94c, GX__SetCPUFifo_8016c94c, (uint32_t fa), (fa));

extern "C" void GX__SetGPFifo_8016cb2c(uint32_t fa) { GXSetGPFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8016cb2c, GX__SetGPFifo_8016cb2c, (uint32_t fa), (fa));

extern "C" void __GX__SaveFifo_8016cdbc(uint32_t fa) { GXSaveCPUFifo((GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj))); }
PPC_NATIVE_OVERRIDE_VOID(8016cdbc, __GX__SaveFifo_8016cdbc, (uint32_t fa), (fa));

extern "C" void GX__GetCPUFifo_8016cf10(uint32_t fa) { auto* d=(GXFifoObj*)GuestToHostPtr(fa, sizeof(GXFifoObj)); auto* s=GXGetCPUFifo(); if(d&&s) std::memcpy(d,s,sizeof(GXFifoObj)); }
PPC_NATIVE_OVERRIDE_VOID(8016cf10, GX__GetCPUFifo_8016cf10, (uint32_t fa), (fa));

extern "C" void __GX__FifoInit_8016d180()
{
    constexpr uint32_t kCpInterruptId = 0x11u;
    constexpr uint32_t kCpInterruptMask = 0x4000u;
    constexpr size_t kFifoObjSize = 0x24u;
    const GxGlobals& gx = Gx();

    if (gx.cpInterruptHandler) {
        __OSSetInterruptHandler_801a65f8_hle(kCpInterruptId, gx.cpInterruptHandler);
        __OSUnmaskInterrupts_801a69bc_hle(kCpInterruptMask);
    }

    auto write32 = [](uint32_t addr, uint32_t value) {
        if (addr) try { Memory::Write32(addr, value); } catch (const ::Memory::AccessViolation&) {}
    };
    auto write8 = [](uint32_t addr, uint8_t value) {
        if (addr) try { Memory::Write8(addr, value); } catch (const ::Memory::AccessViolation&) {}
    };
    write32(gx.currentThread, OS__GetCurrentThread_801a98b0_hle());
    write32(gx.overflowSuspend, 0);
    for (size_t offset = 0; offset < kFifoObjSize; offset += 4) {
        if (gx.cpuFifo) write32(gx.cpuFifo + static_cast<uint32_t>(offset), 0);
        if (gx.gpFifo) write32(gx.gpFifo + static_cast<uint32_t>(offset), 0);
    }
    write8(gx.cpuFifoReady, 0);
    write8(gx.gpFifoReady, 0);
}

extern "C" void __GX__PEInit_8016ee14()
{
    constexpr uint32_t kPeTokenInterruptId = 0x12u;
    constexpr uint32_t kPeFinishInterruptId = 0x13u;
    constexpr uint32_t kPeTokenInterruptMask = 0x1000u;
    constexpr uint32_t kPeFinishInterruptMask = 0x2000u;
    const GxGlobals& gx = Gx();

    if (gx.tokenInterruptHandler) {
        __OSSetInterruptHandler_801a65f8_hle(kPeTokenInterruptId, gx.tokenInterruptHandler);
        __OSUnmaskInterrupts_801a69bc_hle(kPeTokenInterruptMask);
    }
    if (gx.finishInterruptHandler) {
        __OSSetInterruptHandler_801a65f8_hle(kPeFinishInterruptId, gx.finishInterruptHandler);
        __OSUnmaskInterrupts_801a69bc_hle(kPeFinishInterruptMask);
    }

    if (gx.finishQueue) try { Memory::Write32(gx.finishQueue, 0); } catch (const ::Memory::AccessViolation&) {}
    try {
        const uint32_t gd = GuestGxData();
        if (gd) {
            const uint16_t cur = Memory::Read16(gd + 0x0Au);
            Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(cur | 0x000Fu));
        }
    } catch (const ::Memory::AccessViolation&) {}
}
PPC_NATIVE_OVERRIDE_VOID(8016ee14, __GX__PEInit_8016ee14, (), ());

// ============================================================================
// Display List Recording
// ============================================================================

extern "C" void GX__BeginDisplayList_80172e00(uint32_t la, uint32_t s) {
    try {
        const GxGlobals& gx = Gx();
        const uint32_t fifo = gx.displayListFifo;
        uint32_t gd = GuestGxData();
        if (!gd || !fifo || !gx.savedGxData || !gx.oldCpuFifo) return;
        if (Memory::Read32(gd + 0x5FCu)) GX__SetDirtyState_8016ee78();
        if (Memory::Read8(gd + 0x5F9u)) std::memcpy(Memory::GetPointer(gx.savedGxData, 0x600), Memory::GetPointer(gd, 0x600), 0x600);
        Memory::Write32(fifo + kFifoTop, la + s - 4u);
        Memory::Write32(fifo + kFifoCount, 0);
        Memory::Write32(fifo, la);
        Memory::Write32(fifo + kFifoSize, s);
        Memory::Write32(fifo + kFifoReadPtr, la);
        Memory::Write32(fifo + kFifoWritePtr, la);
        Memory::Write8(gd + 0x5F8u, 1u);
        // Mirror the guest fifo-object fields the FIFO write path consumes so
        // HleFifoWrite never has to read them back out of guest memory.
        BeginDisplayListRecording(la, s);
        GXFlush();
        GX__GetCPUFifo_8016cf10(gx.oldCpuFifo);
        GX__SetCPUFifo_8016c94c(fifo);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(80172e00, GX__BeginDisplayList_80172e00, (uint32_t la, uint32_t s), (la, s));

extern "C" uint32_t GX__EndDisplayList_80172eb4() {
    try {
        const GxGlobals& gx = Gx();
        const uint32_t fifo = gx.displayListFifo;
        if (!fifo || !gx.savedGxData || !gx.oldCpuFifo) {
            EndDisplayListRecording();
            return 0;
        }
        GXFlush();
        GX__GetCPUFifo_8016cf10(fifo);
        const uint8_t wrapped = Memory::Read8(fifo + kFifoWrap);
        GX__SetCPUFifo_8016c94c(gx.oldCpuFifo);

        const uint32_t gd = GuestGxData();
        if (gd) {
            if (Memory::Read8(gd + 0x5F9u) != 0) {
                const int32_t interruptLevel = OS__DisableInterrupts_801a65ac();
                const uint32_t savedWord8 = Memory::Read32(gd + 0x08u);
                std::memcpy(Memory::GetPointer(gd, 0x600),
                            Memory::GetPointer(gx.savedGxData, 0x600),
                            0x600);
                Memory::Write32(gd + 0x08u, savedWord8);
                OS__RestoreInterrupts_801a65d4(interruptLevel);
            }
            Memory::Write8(gd + 0x5F8u, 0);
        }

        // Publish the cached cursor/count before the count is read back below.
        // Done unconditionally: the original read of GXData+0x5F8 returned
        // "inactive" whenever the GXData pointer was null, so recording must
        // stop here even on the gd == 0 path.
        EndDisplayListRecording();

        return wrapped == 0 ? Memory::Read32(fifo + kFifoCount) : 0;
    } catch (...) {
        // Leaving the shadow active would silently swallow every subsequent
        // FIFO write into a dead list.
        EndDisplayListRecording();
        return 0;
    }
}
PPC_NATIVE_OVERRIDE(80172EB4, GX__EndDisplayList_80172eb4, uint32_t, (), ());

// ============================================================================
// Flush
// ============================================================================

extern "C" void GX__Flush_8016e654() { GXFlush(); }
PPC_NATIVE_OVERRIDE_VOID(8016e654, GX__Flush_8016e654, (), ());
