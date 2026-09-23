#include "hle_stubs.h"
#include "memory.h"
#include "abi_bridge.h"
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "settings_overlay.h"
#include "fiber_manager.h"
#include "platform/host_platform.h"
#include "runtime_log.h"

#include <dolphin/vi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include "recomp_mod_loader.h"  // CurrentTranslatedExecutionAddress for the guest trace

void GxDlRecyclePackedArrays();  // gx_dl.cpp

#if defined(__SWITCH__)
#include <atomic>
#include <switch.h>
// Last host call the main thread entered that can block on the GPU/Aurora;
// read by the Switch freeze watchdog in main.cpp.
extern std::atomic<const char*> g_switchHostPhase;
// Lock-free mirrors of VI state for the freeze watchdog: reading g_vi under
// g_viMutex from the watchdog thread could block on whoever holds it.
std::atomic<uint32_t> g_viRetraceMirror{0};
std::atomic<uint32_t> g_viPollCalls{0};
std::atomic<uint32_t> g_viPollNotDue{0};
std::atomic<int64_t> g_viLastRetraceMs{0};
// Deadline of the next retrace, in steady-clock nanoseconds, so a caller can
// tell whether one is due without taking g_viMutex.
std::atomic<int64_t> g_viNextRetraceDueNs{0};

// Retraces should arrive at the interval the TV mode asks for (60 Hz, or 50 for
// PAL). A measured 160 Hz says something else is advancing them, so each path
// counts what it produced, and how far behind schedule it was.
std::atomic<uint32_t> g_viRetracesFromDeadline{0};
std::atomic<uint32_t> g_viRetracesForced{0};
std::atomic<uint64_t> g_viRetraceLatenessUs{0};

// Where a frame's wall clock goes when it is not running guest code. A profile
// put a third of the samples past GX's DrawDone and a quarter past the present,
// but those are "time since the last phase marker" and include guest code, so
// they cannot say whether the guest sleeps too often or we keep it waiting.
// These are written by the paths that do the waiting and reported per frame.
// Defined here because the [vi] line prints them; the writers declare them.
std::atomic<uint32_t> g_schedSleepCalls{0};       // OSSleepThread from the guest
std::atomic<uint32_t> g_schedIdleEntries{0};      // times the scheduler found nothing runnable
std::atomic<uint64_t> g_schedIdleUs{0};           // and how long it spun there
std::atomic<uint32_t> g_gxDrawDoneCalls{0};       // guest waits for the GP to drain
std::atomic<uint64_t> g_gxDrawDoneUs{0};
extern "C" {
extern uint64_t g_auroraRegWrites[3];
extern uint64_t g_auroraProcessCalls;
extern uint64_t g_auroraProcessBytes;
}
extern std::atomic<uint32_t> g_dlScanHits;
extern std::atomic<uint32_t> g_dlScanMisses;
extern std::atomic<uint32_t> g_dlScanDigests;
extern std::atomic<uint32_t> g_gxFifoCalls;
extern std::atomic<uint64_t> g_gxFifoTicks;
extern std::atomic<uint64_t> g_gxFifoSectionTicks[6];
extern std::atomic<uint32_t> g_gxFifoSectionCalls[6];
extern uint64_t g_gxDlSectionTicks[3];
extern "C" {
extern uint64_t g_auroraDrawTicks[6];
extern uint32_t g_auroraDrawCount;
extern uint64_t g_auroraStorageBytes;
extern uint64_t g_gxSyncWaitNs;
extern uint32_t g_gxSyncCount;
extern std::atomic<const char*> g_gxDecodePhase;
extern std::atomic<uint32_t> g_gxDecodeQueued;
void aurora_gx_sync_sites(char* out, size_t size);
}
std::atomic<uint32_t> g_viWaitsSkippedLate{0};    // VIWaitForRetrace skipped: frame already late
// The idle loop split by phase; defined in os_scheduler.cpp, which does the work.
extern std::atomic<uint64_t> g_idleRetraceUs;
extern std::atomic<uint64_t> g_idleSleepTimerUs;
extern std::atomic<uint64_t> g_idleAudioUs;
extern std::atomic<uint64_t> g_idleTimerUs;
extern std::atomic<uint64_t> g_idleAlarmUs;
extern std::atomic<uint64_t> g_idleWaitUs;
extern std::atomic<uint32_t> g_idleIterations;
// Inside the audio poll, which owns nearly all of that idle time.
extern std::atomic<uint64_t> g_audioJoinUs;
extern std::atomic<uint64_t> g_audioPushUs;
extern std::atomic<uint64_t> g_audioGuestUs;
extern std::atomic<uint64_t> g_audioDeferredUs;
extern std::atomic<uint32_t> g_audioBlocks;
#define SWITCH_PHASE(name) g_switchHostPhase.store(name, std::memory_order_relaxed)
#else
#define SWITCH_PHASE(name) ((void)0)
#endif

#if defined(__SWITCH__)
// Defined at global scope in main.cpp. Must be declared outside the anonymous
// namespace below, or it picks up internal linkage and fails to resolve.
void SwitchBootLogExternal(const char* text) noexcept;
bool SwitchDevLoggingEnabled() noexcept;
void SwitchTraceRing(const char* text) noexcept;

// An "enter" with no matching "return" names the callback that blocked.
static void SwitchRetraceCallbackTrace(const char* what, uint32_t address) {
    static std::atomic<int> count{0};
    const int index = count.fetch_add(1, std::memory_order_relaxed);
    if (index < 30 || (index % 500) == 0) {
        char trace[96];
        std::snprintf(trace, sizeof(trace), "[retrace] %s 0x%08X (#%d)", what, address, index);
        SwitchBootLogExternal(trace);
    }
}
#endif
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <aurora/aurora.h>

// Forward declaration for OSWakeupThread - used to wake threads on VI retrace queue
extern "C" void OSWakeupThread_HLE_801aaaa4(CpuContext* ctx);

// Forward declaration for OSSleepThread - used by VIWaitForRetrace HLE
extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* cpu);
extern "C" int g_gxFrameCount;
extern "C" int32_t OS__DisableInterrupts_801a65ac();
extern "C" int32_t OS__RestoreInterrupts_801a65d4(int32_t level);

// Aurora frame cycle tracking - needs external linkage for the GX HLE
// (declared in gx_internal.h, consumed by gx_frame.cpp). We need to call
// aurora_begin_frame() before GX commands and aurora_end_frame() after.
std::atomic_bool g_auroraFrameActive{false};
std::atomic_bool g_auroraFrameHadWork{false};

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGuestGxRenderModeCopyBytes = 0x39;

bool ReadGuestRenderModeObj(uint32_t guestPtr, GXRenderModeObj& out) {
    if (guestPtr == 0 || !Memory::Contains(guestPtr, kGuestGxRenderModeCopyBytes)) {
        return false;
    }

    try {
        out.viTVmode = static_cast<VITVMode>(Memory::Read32(guestPtr + 0x00));
        out.fbWidth = Memory::Read16(guestPtr + 0x04);
        out.efbHeight = Memory::Read16(guestPtr + 0x06);
        out.xfbHeight = Memory::Read16(guestPtr + 0x08);
        out.viXOrigin = Memory::Read16(guestPtr + 0x0a);
        out.viYOrigin = Memory::Read16(guestPtr + 0x0c);
        out.viWidth = Memory::Read16(guestPtr + 0x0e);
        out.viHeight = Memory::Read16(guestPtr + 0x10);
        out.xFBmode = static_cast<VIXFBMode>(Memory::Read32(guestPtr + 0x14));
        out.field_rendering = Memory::Read8(guestPtr + 0x18);
        out.aa = Memory::Read8(guestPtr + 0x19);

        for (size_t i = 0; i < 12; ++i) {
            out.sample_pattern[i][0] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2));
            out.sample_pattern[i][1] = Memory::Read8(guestPtr + 0x1a + static_cast<uint32_t>(i * 2) + 1);
        }
        for (size_t i = 0; i < 7; ++i) {
            out.vfilter[i] = Memory::Read8(guestPtr + 0x32 + static_cast<uint32_t>(i));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }

    return true;
}

struct ViState {
    bool initialized = false;
    uint32_t tvFormat = 0; // VIGetTvFormat returns VI_NTSC before VIInit
    uint32_t nextFrameBuffer = 0;
    uint32_t currentFrameBuffer = 0;
    uint32_t retraceCount = 0;
    bool black = false;
    uint32_t preRetraceCallback = 0;
    uint32_t postRetraceCallback = 0;
    uint32_t renderWidth = 640;
    uint32_t renderHeight = 480;
    uint32_t viXOrigin = 0;
    uint32_t viYOrigin = 0;
    uint32_t xfbWidth = 640;
    uint32_t xfbHeight = 480;
    bool fieldOdd = false;
    Clock::time_point lastRetrace = Clock::now();
    std::chrono::microseconds retraceInterval{16666us}; // ~60 Hz
    bool hasValidXfb = false; // True once we've received at least one GXCopyDisp
    uint32_t readyXfb = 0;    // XFB address from the most recent GXCopyDisp

    // VIConfigure/VISetNextFrameBuffer/VISetBlack only write pending values below; VIFlush arms them but
    // the commit happens at the next retrace, matching real VI hardware. A VIFlush called from a
    // pre-retrace callback therefore misses the imminent field and lands one field late.

    // Pending values (written by VISetNextFrameBuffer, VISetBlack, VIConfigure)
    uint32_t pendingNextFrameBuffer = 0;
    bool pendingBlack = false;
    // Matches the active-state default (NTSC before VIInit) and the pending
    // 16666us interval below; a flush before any VIConfigure must not commit
    // PAL timing onto an NTSC-interval state.
    uint32_t pendingTvFormat = 0;
    uint32_t pendingRenderWidth = 640;
    uint32_t pendingRenderHeight = 480;
    uint32_t pendingViXOrigin = 0;
    uint32_t pendingViYOrigin = 0;
    uint32_t pendingXfbWidth = 640;
    uint32_t pendingXfbHeight = 480;
    std::chrono::microseconds pendingRetraceInterval{16666us};
    
    // Set by VIFlush(); cleared after commit in AdvanceRetrace
    bool flushArmed = false;
};

std::mutex g_viMutex;
ViState g_vi;


// Guest-side state addresses used by the SDK's VI globals.
constexpr uint32_t kViInitializedFlagAddr   = 0x80386b38;
constexpr uint32_t kViTvFormatAddr          = 0x80386ba8;
constexpr uint32_t kViRenderWidthAddr       = 0x80350864;
constexpr uint32_t kViRenderHeightAddr      = 0x80350866;
constexpr uint32_t kViXfbWidthAddr          = 0x80350872;
constexpr uint32_t kViXfbHeightAddr         = 0x8035087c;
constexpr uint32_t kViRetraceCountAddr      = 0x80386be4; // matches VIWaitForRetrace/handler
constexpr uint32_t kViTimingGuardAddr       = 0x80386b44;
constexpr uint32_t kViPreRetraceCallback    = 0x80386bb8;
constexpr uint32_t kViPostRetraceCallback   = 0x80386bb4;
constexpr uint32_t kViNextFrameBufferAddr   = 0x80386ba0;
constexpr uint32_t kViNextFrameBufferHwAddr = 0x80350890;
constexpr uint32_t kViRetraceQueueAddr      = 0x80386bc0; // Thread queue for VIWaitForRetrace

// EGG::BaseSystem::sSystem pointer - must be non-null before post-retrace callback is valid
constexpr uint32_t kEggSSystemAddr = 0x80386F60;

std::chrono::microseconds IntervalForFormat(uint32_t tvFormat) {
    // NTSC-ish defaults to 60 Hz; PAL uses 50 Hz.
    return tvFormat == 1 ? 20000us : 16666us;
}

// Shared busy-wait budget for deadline-precise sleeps (matches Aurora's
// presenter spin window). Larger windows burn a core for no visible gain.
constexpr std::chrono::microseconds kFinalSpinWindow{500};

void SleepPreciselyUntil(Clock::time_point deadline, bool finishWithSpin = false,
                         std::chrono::microseconds spinWindow = 750us) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return;
    }
    const auto timerDeadline =
        finishWithSpin && deadline - now > spinWindow ? deadline - spinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    struct HighResolutionTimer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE);
        ~HighResolutionTimer() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    static MKW_THREAD_LOCAL HighResolutionTimer timer;
    if (timer.handle != nullptr) {
        const auto remaining100ns =
            std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(timerDeadline - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
            if (!finishWithSpin) {
                return;
            }
        }
    }
#endif
    if (!finishWithSpin) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    if (Clock::now() < timerDeadline) {
        std::this_thread::sleep_until(timerDeadline);
    }
    // This runs only for the final fraction of a VI interval. Do not yield the
    // host thread here: a scheduler quantum is larger than the remaining
    // budget and would recreate the 17-18 ms sawtooth this path removes.
    while (Clock::now() < deadline) {
#if defined(_WIN32)
        YieldProcessor();
#endif
    }
}

void ViSetR3(CpuContext* ctx, uint32_t value)
{
    if (ctx) {
        ctx->gpr[3] = value;
    }
}

void WriteGuestStateLocked() {
    try {
        Memory::Write8(kViInitializedFlagAddr, 1);
        Memory::Write8(kViTimingGuardAddr, 1);
        Memory::Write32(kViTvFormatAddr, g_vi.tvFormat);
        Memory::Write16(kViRenderWidthAddr, static_cast<uint16_t>(g_vi.renderWidth));
        Memory::Write16(kViRenderHeightAddr, static_cast<uint16_t>(g_vi.renderHeight));
        Memory::Write16(kViXfbWidthAddr, static_cast<uint16_t>(g_vi.xfbWidth));
        Memory::Write16(kViXfbHeightAddr, static_cast<uint16_t>(g_vi.xfbHeight));
        Memory::Write32(kViRetraceCountAddr, g_vi.retraceCount);
        Memory::Write32(kViPreRetraceCallback, g_vi.preRetraceCallback);
        Memory::Write32(kViPostRetraceCallback, g_vi.postRetraceCallback);
        // Write PENDING frame buffer to guest memory so SDK code sees the queued value
        Memory::Write32(kViNextFrameBufferAddr, g_vi.pendingNextFrameBuffer);
        Memory::Write32(kViNextFrameBufferHwAddr, g_vi.pendingNextFrameBuffer);
    } catch (const ::Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_VI, "WriteGuestStateLocked", e);
    }
}

void EnsureInitializedLocked() {
    if (g_vi.initialized) {
        return;
    }
    g_vi.initialized = true;
    g_vi.retraceInterval = IntervalForFormat(g_vi.tvFormat);
    g_vi.lastRetrace = Clock::now();
    WriteGuestStateLocked();
}

// GXRenderModeObj::viTVmode encodes the output family (0 NTSC, 1 PAL, 2 MPAL,
// 5 EURGB60) in bits [4:2].
uint32_t ExtractTvFormat(uint32_t tvMode) {
    return (tvMode >> 2) & 0x7;
}

// Re-entry guard to prevent AdvanceRetrace calling itself via OSWakeupThread -> SelectThread
static std::atomic<bool> s_inAdvanceRetrace{false};
#if defined(__SWITCH__)
// Who holds s_inAdvanceRetrace and since when, so a skipped retrace can name
// the callback that never returned.
static std::atomic<int64_t> s_guardSinceMs{0};
static std::atomic<uint32_t> s_guardCb{0};
static int64_t SwitchNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void SwitchGuardCallback(uint32_t address) {
    s_guardCb.store(address, std::memory_order_relaxed);
}
#endif

// Set while VI_HLE_PresentFrame runs its seal/pace/pre-warm sequence. Guest callbacks
// serviced during that window (pace-loop alarms, GX timing polls) still see the stale
// hasValidXfb/g_auroraFrameActive flags; a retrace-context present fired from them would
// end the freshly pre-warmed empty frame and show it as a black frame group.
static std::atomic<bool> s_presentSequenceActive{false};

void AdvanceRetrace(CpuContext* ctx, Clock::time_point retraceStamp, bool serviceAurora) {
    // Prevent re-entry - this can happen if OSWakeupThread triggers SelectThread
    // which goes idle and calls ProcessTimerEvents again
    if (s_inAdvanceRetrace.exchange(true)) {
#if defined(__SWITCH__)
        // If a game retrace callback blocks, its fiber parks while holding this
        // flag, and every later retrace is skipped here - which would stop
        // everything retrace-driven at once. Count the skips to prove it.
        static std::atomic<int> guardSkips{0};
        const int skip = guardSkips.fetch_add(1, std::memory_order_relaxed);
        if (skip < 5 || (skip % 200) == 0) {
            char trace[96];
            std::snprintf(trace, sizeof(trace), "[retrace] SKIPPED, guard held (#%d)", skip);
            SwitchBootLogExternal(trace);
        }
        // Held for over a second means a callback is stuck, not re-entering.
        static std::atomic<int64_t> lastStuckLog{0};
        const int64_t now = SwitchNowMs();
        const int64_t heldMs = now - s_guardSinceMs.load(std::memory_order_relaxed);
        if (heldMs > 1000 && now - lastStuckLog.load(std::memory_order_relaxed) > 2000) {
            lastStuckLog.store(now, std::memory_order_relaxed);
            char trace[128];
            std::snprintf(trace, sizeof(trace),
                          "[retrace] guard STUCK %lldms in cb 0x%08X (0=not in a callback) guest=0x%08X",
                          static_cast<long long>(heldMs), s_guardCb.load(std::memory_order_relaxed),
                          RecompMod::CurrentTranslatedExecutionAddress());
            SwitchBootLogExternal(trace);
        }
#endif
        return;
    }
#if defined(__SWITCH__)
    s_guardSinceMs.store(SwitchNowMs(), std::memory_order_relaxed);
    SwitchGuardCallback(0);
#endif
    
    uint32_t preCb = 0;
    uint32_t postCb = 0;
    uint32_t retraceValue = 0;
    bool hasXfbReady = false;
    uint32_t readyXfb = 0;
    uint32_t currentFb = 0;
    bool isBlack = false;
    int64_t intervalUs = 0;   // what the TV mode asks for, for the trace below
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        
        // Commit pending state if VIFlush armed it (see ViState).
        if (g_vi.flushArmed) {
            // Commit pending -> active
            g_vi.nextFrameBuffer = g_vi.pendingNextFrameBuffer;
            g_vi.black = g_vi.pendingBlack;
            g_vi.tvFormat = g_vi.pendingTvFormat;
            g_vi.renderWidth = g_vi.pendingRenderWidth;
            g_vi.renderHeight = g_vi.pendingRenderHeight;
            g_vi.viXOrigin = g_vi.pendingViXOrigin;
            g_vi.viYOrigin = g_vi.pendingViYOrigin;
            g_vi.xfbWidth = g_vi.pendingXfbWidth;
            g_vi.xfbHeight = g_vi.pendingXfbHeight;
            g_vi.retraceInterval = g_vi.pendingRetraceInterval;
            
            // Clear flush armed flag
            g_vi.flushArmed = false;
            
        }
        
        g_vi.retraceCount++;
#if defined(__SWITCH__)
        g_viRetraceMirror.store(g_vi.retraceCount, std::memory_order_relaxed);
        g_viLastRetraceMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count(),
                                std::memory_order_relaxed);
#endif
        g_vi.fieldOdd = !g_vi.fieldOdd;
        g_vi.currentFrameBuffer = g_vi.nextFrameBuffer;
        currentFb = g_vi.currentFrameBuffer;
        g_vi.lastRetrace = retraceStamp;
#if defined(__SWITCH__)
        g_viNextRetraceDueNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       (retraceStamp + g_vi.retraceInterval).time_since_epoch())
                                       .count(),
                                   std::memory_order_relaxed);
#endif
        retraceValue = g_vi.retraceCount;
        intervalUs = g_vi.retraceInterval.count();
        preCb = g_vi.preRetraceCallback;
        postCb = g_vi.postRetraceCallback;
        hasXfbReady = g_vi.hasValidXfb;
        readyXfb = g_vi.readyXfb;
        isBlack = g_vi.black;
        WriteGuestStateLocked();
    }

    // Wake up threads sleeping on the VI retrace queue (VIWaitForRetrace).
    // The retrace count has been incremented and written to guest memory.
#if defined(__SWITCH__)
    {
        static std::atomic<int> retraceWakeLog{0};
        const int index = retraceWakeLog.fetch_add(1, std::memory_order_relaxed);
        if (index < 6) {
            char trace[128];
            std::snprintf(trace, sizeof(trace), "[retrace] advance #%d ctx=%s", index,
                          ctx != nullptr ? "yes" : "NULL (wake skipped)");
            SwitchBootLogExternal(trace);
        }
    }
#endif
    if (ctx) {
        ctx->gpr[3] = kViRetraceQueueAddr;
        OSWakeupThread_HLE_801aaaa4(ctx);
    }

    if (serviceAurora) {
        // Process window events (but don't present - that happens in GXCopyDisp)
        UpdateAuroraAndProcessEvents();

        // Start a new Aurora frame if one isn't already active
        if (!g_auroraFrameActive.load(std::memory_order_acquire)) {
            if (BeginAuroraFrame()) {
                g_auroraFrameActive.store(true, std::memory_order_release);
            }
        }
    }

    if (ctx) {
        ctx->gpr[3] = retraceValue;
        if (preCb) {
#if defined(__SWITCH__)
            SwitchRetraceCallbackTrace("preCb enter", preCb);
            SwitchGuardCallback(preCb);
#endif
            InvokeIndirectCpu(preCb, ctx);
#if defined(__SWITCH__)
            SwitchRetraceCallbackTrace("preCb return", preCb);
            SwitchGuardCallback(0);
#endif
        }
        if (postCb) {
            // Guard: only invoke callback if sSystem is initialized
            // The callback dereferences sSystem which must be non-null
            uint32_t sSystemPtr = Memory::Read32(kEggSSystemAddr);
            if (sSystemPtr != 0) {
#if defined(__SWITCH__)
                SwitchRetraceCallbackTrace("postCb enter", postCb);
            SwitchGuardCallback(postCb);
#endif
                InvokeIndirectCpu(postCb, ctx);
#if defined(__SWITCH__)
                SwitchRetraceCallbackTrace("postCb return", postCb);
            SwitchGuardCallback(0);
#endif
            }
        }
    }

    // VISetBlack(TRUE) keeps frame submission running but shows only the clear color: GX render work is
    // skipped and Aurora's end_frame() clears to black, matching the hardware manual's "signal continues,
    // pixels go black" behavior. When not black, submission waits for hasXfbReady (GXCopyDisp done).
    if (serviceAurora && !s_presentSequenceActive.load(std::memory_order_acquire)) {
        const bool frameActive = g_auroraFrameActive.load(std::memory_order_acquire);
        const bool xfbMatches = (readyXfb != 0 && readyXfb == currentFb);
        const bool shouldPresentXfb = hasXfbReady && !isBlack && xfbMatches;
        const bool shouldPresentBlack = isBlack && frameActive;
        const bool shouldSubmit = frameActive && (shouldPresentXfb || shouldPresentBlack);

        if (shouldSubmit) {
            if (!isBlack || settings_overlay::StartupScreenVisible()) {
                // Normal presentation: draw overlay on top of GX content
                settings_overlay::Draw();
            }
            // Outside startup, VI black remains a pure black presentation.
            // Unpaced: this present already runs in retrace context.
            VI_HLE_PresentFrame(shouldPresentXfb, false);
        } else if (g_auroraFrameHadWork.load(std::memory_order_acquire) && !shouldPresentXfb && !isBlack) {
            // GX work is in progress but frame not complete - just poll window events
            // Don't call aurora_end_frame() as that would present incomplete work
            UpdateAuroraAndProcessEvents();
        }
    }

#if defined(__SWITCH__)
    // Guest-progress trace. The retrace path runs on the guest's own thread, so
    // this needs no extra thread or lock - which is what made the heartbeat
    // unsafe. Once a second at 60Hz; the address tells us where the guest is,
    // and gxcopies separates "never draws" from "draws but never presents".
    if ((retraceValue % 60) == 0 && SwitchDevLoggingEnabled()) {
        char trace[160];
        std::snprintf(trace, sizeof(trace),
                      "[vi] retrace=%u guest=0x%08X gxcopies=%d xfbReady=%d black=%d frameActive=%d "
                      "interval=%lldus deadline=%u forced=%u late=%lluus",
                      retraceValue, RecompMod::CurrentTranslatedExecutionAddress(), g_gxFrameCount,
                      hasXfbReady ? 1 : 0, isBlack ? 1 : 0,
                      g_auroraFrameActive.load(std::memory_order_acquire) ? 1 : 0,
                      static_cast<long long>(intervalUs),
                      g_viRetracesFromDeadline.load(std::memory_order_relaxed),
                      g_viRetracesForced.load(std::memory_order_relaxed),
                      static_cast<unsigned long long>(
                          g_viRetraceLatenessUs.load(std::memory_order_relaxed) /
                          std::max<uint32_t>(g_viRetracesFromDeadline.load(std::memory_order_relaxed), 1)));
        SwitchBootLogExternal(trace);

        // The same second, counted per frame rather than cumulatively: how often
        // the guest parked itself, how long we spun with nothing to run, and how
        // long it waited for the GP. Frames here are GX copies, not retraces -
        // the question is what a drawn frame costs.
        static uint32_t lastSleeps = 0, lastIdleEntries = 0, lastDrawDones = 0, lastFrames = 0;
        static uint64_t lastIdleUs = 0, lastDrawDoneUs = 0;
        const uint32_t sleeps = g_schedSleepCalls.load(std::memory_order_relaxed);
        const uint32_t idleEntries = g_schedIdleEntries.load(std::memory_order_relaxed);
        const uint64_t idleUs = g_schedIdleUs.load(std::memory_order_relaxed);
        const uint32_t drawDones = g_gxDrawDoneCalls.load(std::memory_order_relaxed);
        const uint64_t drawDoneUs = g_gxDrawDoneUs.load(std::memory_order_relaxed);
        const uint32_t frames = static_cast<uint32_t>(g_gxFrameCount);
        const uint32_t deltaFrames = std::max<uint32_t>(frames - lastFrames, 1);

        char sched[176];
        std::snprintf(sched, sizeof(sched),
                      "[sched] per frame over %u: sleeps=%.1f idle=%.1fx %lluus drawDone=%.1fx %lluus",
                      deltaFrames,
                      static_cast<double>(sleeps - lastSleeps) / deltaFrames,
                      static_cast<double>(idleEntries - lastIdleEntries) / deltaFrames,
                      static_cast<unsigned long long>((idleUs - lastIdleUs) / deltaFrames),
                      static_cast<double>(drawDones - lastDrawDones) / deltaFrames,
                      static_cast<unsigned long long>((drawDoneUs - lastDrawDoneUs) / deltaFrames));
        SwitchBootLogExternal(sched);

        static uint32_t lastFifoCalls = 0;
        static uint64_t lastFifoTicks = 0;
        const uint32_t fifoCalls = g_gxFifoCalls.load(std::memory_order_relaxed);
        const uint64_t fifoTicks = g_gxFifoTicks.load(std::memory_order_relaxed);
        char fifo[128];
        std::snprintf(fifo, sizeof(fifo), "[gxfifo] per frame: writes=%u decode=%lluus",
                      (fifoCalls - lastFifoCalls) / deltaFrames,
                      static_cast<unsigned long long>(armTicksToNs(fifoTicks - lastFifoTicks) / 1000ull /
                                                      deltaFrames));
        SwitchBootLogExternal(fifo);
        lastFifoCalls = fifoCalls;
        lastFifoTicks = fifoTicks;

        // bp, cp, xf, calldl, draw, vertex: packets per frame and us per frame.
        static uint64_t lastSectionTicks[6] = {};
        static uint32_t lastSectionCalls[6] = {};
        uint64_t sectionUs[6];
        uint32_t sectionCalls[6];
        for (int i = 0; i < 6; ++i) {
            const uint64_t ticks = g_gxFifoSectionTicks[i].load(std::memory_order_relaxed);
            const uint32_t calls = g_gxFifoSectionCalls[i].load(std::memory_order_relaxed);
            sectionUs[i] = armTicksToNs(ticks - lastSectionTicks[i]) / 1000ull / deltaFrames;
            sectionCalls[i] = (calls - lastSectionCalls[i]) / deltaFrames;
            lastSectionTicks[i] = ticks;
            lastSectionCalls[i] = calls;
        }
        char split[224];
        std::snprintf(split, sizeof(split),
                      "[gxfifo] split: bp=%ux %lluus cp=%ux %lluus xf=%ux %lluus calldl=%ux %lluus "
                      "draw=%ux %lluus vertex=%ux %lluus",
                      sectionCalls[0], (unsigned long long)sectionUs[0], sectionCalls[1],
                      (unsigned long long)sectionUs[1], sectionCalls[2], (unsigned long long)sectionUs[2],
                      sectionCalls[3], (unsigned long long)sectionUs[3], sectionCalls[4],
                      (unsigned long long)sectionUs[4], sectionCalls[5], (unsigned long long)sectionUs[5]);
        SwitchBootLogExternal(split);

        // Inside calldl: Aurora's decode/draw, the HLE state sync and array apply
        // (the remainder is the scan). Then Aurora's own per-draw split.
        static uint64_t lastDl[3] = {}, lastDraw[6] = {}, lastStorage = 0;
        static uint32_t lastDrawCount = 0;
        auto perFrameUs = [&](uint64_t now, uint64_t& last) {
            const uint64_t us = armTicksToNs(now - last) / 1000ull / deltaFrames;
            last = now;
            return static_cast<unsigned long long>(us);
        };
        const unsigned long long dlAurora = perFrameUs(g_gxDlSectionTicks[0], lastDl[0]);
        const unsigned long long dlSync = perFrameUs(g_gxDlSectionTicks[1], lastDl[1]);
        const unsigned long long dlApply = perFrameUs(g_gxDlSectionTicks[2], lastDl[2]);
        const unsigned long long drawTotal = perFrameUs(g_auroraDrawTicks[0], lastDraw[0]);
        const unsigned long long drawArrays = perFrameUs(g_auroraDrawTicks[1], lastDraw[1]);
        const unsigned long long drawPipeline = perFrameUs(g_auroraDrawTicks[2], lastDraw[2]);
        const unsigned long long drawTextures = perFrameUs(g_auroraDrawTicks[3], lastDraw[3]);
        const unsigned long long drawBind = perFrameUs(g_auroraDrawTicks[4], lastDraw[4]);
        const unsigned long long drawUniform = perFrameUs(g_auroraDrawTicks[5], lastDraw[5]);
        const uint32_t drawCount = g_auroraDrawCount;
        const uint64_t storage = g_auroraStorageBytes;
        // Per frame, like everything else on this line.
        static uint32_t lastScanHits = 0, lastScanMisses = 0, lastScanDigests = 0;
        const uint32_t hitsNow = g_dlScanHits.load(std::memory_order_relaxed);
        const uint32_t missesNow = g_dlScanMisses.load(std::memory_order_relaxed);
        const uint32_t digestsNow = g_dlScanDigests.load(std::memory_order_relaxed);
        const uint32_t scanHits = (hitsNow - lastScanHits) / deltaFrames;
        const uint32_t scanMisses = (missesNow - lastScanMisses) / deltaFrames;
        const uint32_t scanDigests = (digestsNow - lastScanDigests) / deltaFrames;
        lastScanHits = hitsNow; lastScanMisses = missesNow; lastScanDigests = digestsNow;

        char dl[256];
        std::snprintf(dl, sizeof(dl),
                      "[gxdl] per frame: scan hit=%u miss=%u digest=%u | aurora=%lluus sync=%lluus apply=%lluus | draws=%u total=%lluus "
                      "arrays=%lluus (%lluKB) pipeline=%lluus textures=%lluus bind=%lluus uniform=%lluus",
                      scanHits, scanMisses, scanDigests,
                      dlAurora, dlSync, dlApply, (drawCount - lastDrawCount) / deltaFrames, drawTotal,
                      drawArrays, static_cast<unsigned long long>((storage - lastStorage) / 1024u / deltaFrames),
                      drawPipeline, drawTextures, drawBind, drawUniform);
        SwitchBootLogExternal(dl);
        // What the decoder was actually asked to do, so a guess about volume
        // can be checked against a count.
        static uint64_t lastRegs[3] = {0, 0, 0};
        static uint64_t lastProcCalls = 0, lastProcBytes = 0;
        char regs[200];
        std::snprintf(regs, sizeof(regs),
                      "[gxregs] per frame: bp=%llu cp=%llu xf=%llu | process calls=%llu bytes=%lluKB",
                      static_cast<unsigned long long>((g_auroraRegWrites[0] - lastRegs[0]) / deltaFrames),
                      static_cast<unsigned long long>((g_auroraRegWrites[1] - lastRegs[1]) / deltaFrames),
                      static_cast<unsigned long long>((g_auroraRegWrites[2] - lastRegs[2]) / deltaFrames),
                      static_cast<unsigned long long>((g_auroraProcessCalls - lastProcCalls) / deltaFrames),
                      static_cast<unsigned long long>((g_auroraProcessBytes - lastProcBytes) / 1024u / deltaFrames));
        lastRegs[0] = g_auroraRegWrites[0]; lastRegs[1] = g_auroraRegWrites[1];
        lastRegs[2] = g_auroraRegWrites[2];
        lastProcCalls = g_auroraProcessCalls; lastProcBytes = g_auroraProcessBytes;
        SwitchBootLogExternal(regs);
        static uint64_t lastSyncWaitNs = 0;
        static uint32_t lastSyncCount = 0;
        char syncLine[160];
        std::snprintf(syncLine, sizeof(syncLine), "[gxthread] per frame: syncs=%u wait=%lluus | worker %s, queued %u",
                      (g_gxSyncCount - lastSyncCount) / deltaFrames,
                      static_cast<unsigned long long>((g_gxSyncWaitNs - lastSyncWaitNs) / 1000ull / deltaFrames),
                      g_gxDecodePhase.load(std::memory_order_relaxed),
                      g_gxDecodeQueued.load(std::memory_order_relaxed));
        SwitchBootLogExternal(syncLine);
        char sites[200];
        aurora_gx_sync_sites(sites, sizeof(sites));
        if (sites[0] != '\0') {
            char siteLine[240];
            std::snprintf(siteLine, sizeof(siteLine), "[gxthread] syncs from: %s", sites);
            SwitchBootLogExternal(siteLine);
        }
        lastSyncWaitNs = g_gxSyncWaitNs;
        lastSyncCount = g_gxSyncCount;
        lastDrawCount = drawCount;
        lastStorage = storage;

        // Inside that idle time: alarms, timer events and audio run guest
        // callbacks here, so only `wait` is the scheduler genuinely doing
        // nothing. Everything else is work that merely happens in this loop.
        static uint64_t lastRetraceUs = 0, lastSleepTimerUs = 0, lastAudioUs = 0;
        static uint64_t lastTimerUs = 0, lastAlarmUs = 0, lastWaitUs = 0;
        static uint32_t lastIterations = 0;
        const uint64_t retraceUs = g_idleRetraceUs.load(std::memory_order_relaxed);
        const uint64_t sleepTimerUs = g_idleSleepTimerUs.load(std::memory_order_relaxed);
        const uint64_t audioUs = g_idleAudioUs.load(std::memory_order_relaxed);
        const uint64_t timerUs = g_idleTimerUs.load(std::memory_order_relaxed);
        const uint64_t alarmUs = g_idleAlarmUs.load(std::memory_order_relaxed);
        const uint64_t waitUs = g_idleWaitUs.load(std::memory_order_relaxed);
        const uint32_t iterations = g_idleIterations.load(std::memory_order_relaxed);

        static uint64_t lastJoinUs = 0, lastPushUs = 0, lastGuestUs = 0, lastDeferredUs = 0;
        static uint32_t lastBlocks = 0;
        const uint64_t joinUs = g_audioJoinUs.load(std::memory_order_relaxed);
        const uint64_t pushUs = g_audioPushUs.load(std::memory_order_relaxed);
        const uint64_t guestUs = g_audioGuestUs.load(std::memory_order_relaxed);
        const uint64_t deferredUs = g_audioDeferredUs.load(std::memory_order_relaxed);
        const uint32_t blocks = g_audioBlocks.load(std::memory_order_relaxed);

        char idle[256];
        std::snprintf(idle, sizeof(idle),
                      "[idle] per frame: loops=%u retrace=%lluus timers=%lluus audio=%lluus "
                      "fibers=%lluus alarms=%lluus wait=%lluus",
                      (iterations - lastIterations) / deltaFrames,
                      static_cast<unsigned long long>((retraceUs - lastRetraceUs) / deltaFrames),
                      static_cast<unsigned long long>((sleepTimerUs - lastSleepTimerUs) / deltaFrames),
                      static_cast<unsigned long long>((audioUs - lastAudioUs) / deltaFrames),
                      static_cast<unsigned long long>((timerUs - lastTimerUs) / deltaFrames),
                      static_cast<unsigned long long>((alarmUs - lastAlarmUs) / deltaFrames),
                      static_cast<unsigned long long>((waitUs - lastWaitUs) / deltaFrames));
        SwitchBootLogExternal(idle);

        char audio[176];
        std::snprintf(audio, sizeof(audio),
                      "[audio] per frame: blocks=%.1f join=%lluus push=%lluus guestcb=%lluus "
                      "deferred=%lluus",
                      static_cast<double>(blocks - lastBlocks) / deltaFrames,
                      static_cast<unsigned long long>((joinUs - lastJoinUs) / deltaFrames),
                      static_cast<unsigned long long>((pushUs - lastPushUs) / deltaFrames),
                      static_cast<unsigned long long>((guestUs - lastGuestUs) / deltaFrames),
                      static_cast<unsigned long long>((deferredUs - lastDeferredUs) / deltaFrames));
        SwitchBootLogExternal(audio);

        lastJoinUs = joinUs;
        lastPushUs = pushUs;
        lastGuestUs = guestUs;
        lastDeferredUs = deferredUs;
        lastBlocks = blocks;

        lastRetraceUs = retraceUs;
        lastSleepTimerUs = sleepTimerUs;
        lastAudioUs = audioUs;
        lastTimerUs = timerUs;
        lastAlarmUs = alarmUs;
        lastWaitUs = waitUs;
        lastIterations = iterations;

        lastSleeps = sleeps;
        lastIdleEntries = idleEntries;
        lastIdleUs = idleUs;
        lastDrawDones = drawDones;
        lastDrawDoneUs = drawDoneUs;
        lastFrames = frames;
    }
#endif

    // Clear re-entry guard
    s_inAdvanceRetrace.store(false);
}

bool AdvanceDueRetraces(CpuContext* ctx, int maxToProcess, bool serviceAurora)
{
    bool advancedAny = false;

    for (int catchUpCount = 0; catchUpCount < maxToProcess; ++catchUpCount) {
        Clock::time_point target;
        auto now = Clock::now();
#if defined(__SWITCH__)
        g_viPollCalls.fetch_add(1, std::memory_order_relaxed);
#endif
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            if (!g_vi.initialized) {
                return advancedAny;
            }
            target = g_vi.lastRetrace + g_vi.retraceInterval;
            if (now < target) {
#if defined(__SWITCH__)
                g_viPollNotDue.fetch_add(1, std::memory_order_relaxed);
#endif
                return advancedAny;
            }
#if defined(__SWITCH__)
            g_viRetracesFromDeadline.fetch_add(1, std::memory_order_relaxed);
            g_viRetraceLatenessUs.fetch_add(
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(now - target).count()),
                std::memory_order_relaxed);
#endif
            // Do NOT advance lastRetrace here. AdvanceRetrace sets it once the
            // retrace really happens, and it can decline: its re-entry guard
            // returns early when a guest callback is already inside one. Claiming
            // the interval up front threw those away instead of retrying them -
            // measured on hardware as 2630 deadlines claimed against 1140
            // retraces delivered, the guest running at 21 Hz instead of 33.
        }


        AdvanceRetrace(ctx, target, serviceAurora);
        advancedAny = true;
    }

    return advancedAny;
}

} // namespace

// Force one retrace boundary to pass, whether or not its wall-clock deadline
// has arrived, so the guest's retrace callbacks (AsyncDisplay's counters and
// friends) run. VI_HLE_PollRetrace below is the time-driven counterpart.
void VI_HLE_ForceRetrace(CpuContext* ctx) {
#if defined(__SWITCH__)
    g_viRetracesForced.fetch_add(1, std::memory_order_relaxed);
#endif
    Clock::time_point target;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        target = g_vi.lastRetrace + g_vi.retraceInterval;
    }
    AdvanceRetrace(ctx, target, true);
}

bool VI_HLE_IsAdvancingRetrace() {
    return s_inAdvanceRetrace.load(std::memory_order_acquire);
}

// Advance every retrace whose interval has already elapsed. Safe to call from
// busy loops (GX drawing, the scheduler's idle spin) to keep VBlank ticking.
void VI_HLE_PollRetrace(CpuContext* ctx) {
    AdvanceDueRetraces(ctx, 8, true);
}

#if defined(__SWITCH__)
// Starvation rescue, not the normal VBlank path. Retraces normally come from
// VIWaitForRetrace and the scheduler's idle loop; but when two guest threads
// hand control back and forth, the scheduler's pending mask is never empty, so
// its idle loop exits before its poll and VBlank stops entirely - and the
// thread waiting on the retrace queue can then never wake. Boot livelocked
// exactly there.
//
// Advancing one retrace per call, and only once VBlank is overdue by a wide
// margin, keeps this off the common path: retracing on every scheduler entry
// wakes the retrace-queue thread each time, so the scheduler keeps picking it
// and every other thread starves (boot then never even reads the disc).
constexpr auto kRetraceRescueOverdue = std::chrono::milliseconds{50};

void VI_HLE_PollRetraceIfDue(CpuContext* ctx) {
    const int64_t due = g_viNextRetraceDueNs.load(std::memory_order_relaxed);
    if (due == 0) {
        return;
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now().time_since_epoch())
                            .count();
    const int64_t overdueNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(kRetraceRescueOverdue).count();
    if (now < due + overdueNs) {
        return;
    }
    AdvanceDueRetraces(ctx, 1, true);
}
#endif

void VI_HLE_ProcessRetracesDeferred(int maxToProcess) {
    if (maxToProcess <= 0 || !OS_HLE_InterruptsEnabled()) {
        return;
    }

    // This entry point runs from the middle of an arbitrary translated function
    // (GX__Begin's timing service, the host frame loop). Retrace callbacks are
    // an interrupt from that function's point of view, so they get a private
    // copy of its register file exactly as the hardware interrupt would.
    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    // Host renderer ownership is not a guest critical section. Deliver due VI
    // callbacks at wall-clock cadence while suppressing thread switches and
    // all recursive Aurora/event work until the native GX call has unwound.
    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        AdvanceDueRetraces(cpu, maxToProcess, false);
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

void VI_HLE_WaitForNextRetracePoll() {
    Clock::time_point retraceDeadline;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        retraceDeadline = g_vi.lastRetrace + g_vi.retraceInterval;
    }

    const auto now = Clock::now();
    if (now >= retraceDeadline) {
        return;
    }
    // Audio DMA and alarm queues still need regular service even when the VI
    // deadline is farther away. The high-resolution wait removes the 1 ms
    // scheduler overshoot when retrace is the next event.
    const bool retraceIsNext = retraceDeadline <= now + 1ms;
    SleepPreciselyUntil(retraceIsNext ? retraceDeadline : now + 1ms, retraceIsNext,
                        kFinalSpinWindow);
}

namespace {

// Retrace count consumed by the most recent paced present. This is a slot memo
// against the VI timeline, not a second clock: it only answers "did a retrace
// already elapse while this frame was being produced?". Producer-thread only.
uint32_t s_lastPacedRetraceCount = ~0u;

// Last presentation anchor handed to Aurora, in nanoseconds on the VI retrace
// grid. Guarantees consecutive sealed frames never share an anchor (see the
// comment at the stamping site). Producer-thread only, like the memo above.
uint64_t s_lastPresentAnchorNanos = 0;

// Sleeps to the same VI retrace boundary VIWaitForRetrace targets, servicing alarms every 1 ms so audio
// DMA and timers keep running, then delivers that retrace so guest logic starts exactly on the grid.
void PaceToRetraceBoundary(Clock::time_point deadline) {
    constexpr auto kServiceSlice = 1ms;
    for (;;) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        if (deadline - now > kServiceSlice + kFinalSpinWindow) {
            SleepPreciselyUntil(now + kServiceSlice);
            OS_HLE_ProcessAlarmsDeferred(8);
            // Audio DMA is a 3 ms cadence and this wait is up to a full display
            // period long. Without a pump here the blocks that came due during
            // the wait are all delivered at once when the guest next reaches
            // idle, which the guest observes as AI slack jitter.
            Audio_HLE_PollDeferred();
            continue;
        }
        SleepPreciselyUntil(deadline, true, kFinalSpinWindow);
        break;
    }
    VI_HLE_ProcessRetracesDeferred(1);
}

} // namespace

// Single owner of the Aurora frame presentation sequence: seals the active frame, optionally paces the
// producer to the VI retrace boundary, and pre-warms the next frame. Paced from GXCopyDisp; unpaced for
// the retrace-context black/boot present path in AdvanceRetrace.
static std::atomic<uint32_t> g_debugPresentCount{0};

// Read by the Switch heartbeat thread in main.cpp (see StartSwitchHeartbeat).
uint32_t VI_HLE_DebugPresentCount() {
    return g_debugPresentCount.load(std::memory_order_relaxed);
}

uint32_t VI_HLE_DebugRetraceCount() {
    std::lock_guard<std::mutex> lock(g_viMutex);
    return g_vi.retraceCount;
}

void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace) {
    g_debugPresentCount.fetch_add(1, std::memory_order_relaxed);
    if (s_presentSequenceActive.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    struct SequenceGuard {
        ~SequenceGuard() { s_presentSequenceActive.store(false, std::memory_order_release); }
    } sequenceGuard;
    Clock::time_point paceDeadline{};
    bool paceThisFrame = false;
    if (paceToRetrace) {
        uint64_t baseNanos = 0;
        uint64_t intervalNanos = 0;
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            paceDeadline = g_vi.lastRetrace + g_vi.retraceInterval;
            retraceCount = g_vi.retraceCount;
            baseNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    g_vi.lastRetrace.time_since_epoch())
                    .count());
            intervalNanos = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(g_vi.retraceInterval)
                    .count());
        }
        // Hold the frame to its boundary only when no retrace elapsed during
        // its production. A frame that missed its boundary presents
        // immediately: hardware would quantize down to the next vblank here,
        // but free-running late frames matches the previous pacer and keeps a
        // heavy scene at e.g. 50 fps instead of hard 30.
        const uint32_t retracesElapsed = retraceCount - s_lastPacedRetraceCount;
        paceThisFrame = retracesElapsed == 0;
        s_lastPacedRetraceCount = retraceCount;
        // aurora_report_producer_paced needs a different signal than the pace-wait above: the guest
        // self-paces via VIWaitForRetrace, so one retrace per produced frame is the healthy locked-60
        // cadence, and zero only happens when production outruns VI. "Kept up" means <=1 retrace
        // elapsed; 2+ means a boundary was missed, so Aurora seals that frame without its interpolated
        // slots (a windowed backstop lowers the slot target only under sustained overload).
        aurora_report_producer_paced(retracesElapsed <= 1);
        // Stamp the sealed frame's presentation schedule so Aurora paces interpolated slots against
        // this same VI timeline. Anchor to the NEXT retrace boundary, not the period just produced,
        // since slots anchored to the current period would already be expired by seal time. Encoding
        // overruns are corrected by sliding the whole slot group forward onto a later boundary of this
        // same grid, so this stays the single cadence authority. Anchors must also be strictly
        // monotonic: two frames sealed before lastRetrace advances would collide on one boundary and
        // burst-present, so a colliding anchor steps onto the next grid point instead of repeating it.
        uint64_t anchorNanos = baseNanos + intervalNanos;
        if (s_lastPresentAnchorNanos != 0 && anchorNanos <= s_lastPresentAnchorNanos) {
            anchorNanos = s_lastPresentAnchorNanos + intervalNanos;
        }
        s_lastPresentAnchorNanos = anchorNanos;
        aurora_set_present_schedule(anchorNanos, intervalNanos);
    } else {
        // Retrace-context presents (VI black, boot) have no display period of
        // their own to subdivide; present as soon as the frame is ready. The
        // schedule grid is gone, so the anchor cursor must not constrain the
        // next paced frame.
        s_lastPresentAnchorNanos = 0;
        aurora_set_present_schedule(0, 0);
    }

    SWITCH_PHASE("aurora_end_frame");
    aurora_end_frame();
    // The whole frame is decoded now (end_frame syncs a threaded decoder), so
    // the packed vertex arrays its draws pointed at are free again.
    GxDlRecyclePackedArrays();
    SWITCH_PHASE("after aurora_end_frame");
    if (paceThisFrame) {
        SWITCH_PHASE("PaceToRetraceBoundary");
        PaceToRetraceBoundary(paceDeadline);
        SWITCH_PHASE("after PaceToRetraceBoundary");
        std::lock_guard<std::mutex> lock(g_viMutex);
        s_lastPacedRetraceCount = g_vi.retraceCount;
    }
    settings_overlay::AdvancePresentedFrame();
    g_auroraFrameActive.store(false, std::memory_order_release);
    g_auroraFrameHadWork.store(false, std::memory_order_release);
    if (presentedXfb) {
        std::lock_guard<std::mutex> lock(g_viMutex);
        g_vi.hasValidXfb = false;
        g_vi.readyXfb = 0;
    }
    // Pre-warm the next frame so subsequent GX work has a valid frame context.
    {
        SWITCH_PHASE("prewarm UpdateAuroraAndProcessEvents");
        UpdateAuroraAndProcessEvents();
        SWITCH_PHASE("prewarm BeginAuroraFrame");
        if (BeginAuroraFrame()) {
            g_auroraFrameActive.store(true, std::memory_order_release);
        }
        SWITCH_PHASE("present done");
    }
}

// -----------------------------------------------------------------------------
// VI_HLE_SetXfbReady - Called by GXCopyDisp to signal EFB->XFB copy completed.
// This marks that we now have valid framebuffer data to present.
// -----------------------------------------------------------------------------
void VI_HLE_SetXfbReady(uint32_t xfbAddr) {
    std::lock_guard<std::mutex> lock(g_viMutex);
    g_vi.hasValidXfb = true;
    g_vi.readyXfb = xfbAddr;
    if (g_vi.currentFrameBuffer == 0 && g_vi.nextFrameBuffer == 0) {
        g_vi.currentFrameBuffer = xfbAddr;
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
        WriteGuestStateLocked();
    } else if (g_vi.nextFrameBuffer != xfbAddr) {
        g_vi.nextFrameBuffer = xfbAddr;
        g_vi.pendingNextFrameBuffer = xfbAddr;
    }
}

// VIInit (0x801B94A4) and its lower-level helper __VIInit (0x801B9294) both
// program MMIO at 0xCC0020xx on hardware. We skip all hardware access and seed
// the same defaults instead, so the two entry points share one body.
static void SeedViStateForInit(CpuContext* ctx, const char* who)
{
    RT_LOG(RT_TAG_VI) << who << " called: seeding VI state (HLE)" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
    }
    ViSetR3(ctx, 0);
}

extern "C" void VIInit_HLE_801b94a4(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "VIInit_801b94a4");
}
PPC_NATIVE_OVERRIDE_VOID(801B94A4, VIInit_HLE_801b94a4, (CpuContext* ctx), (ctx));

extern "C" void __VIInit_HLE_801b9294(CpuContext* ctx)
{
    SeedViStateForInit(ctx, "__VIInit_801b9294");
}
PPC_NATIVE_OVERRIDE_VOID(801B9294, __VIInit_HLE_801b9294, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// Helper stubs referenced by VIInit switch cases (case D variants).
// These are hardware-specific; treat as no-ops to keep control flow intact.
// -----------------------------------------------------------------------------
extern "C" void VIInit_caseD_0_HLE_801b9934(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_0_801b9934 stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B9934, VIInit_caseD_0_HLE_801b9934, (CpuContext* ctx), (ctx));

extern "C" void VIInit_caseD_1_HLE_801b993c(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_1_801b993c stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B993C, VIInit_caseD_1_HLE_801b993c, (CpuContext* ctx), (ctx));

extern "C" void VIInit_caseD_2_HLE_801b9944(CpuContext* ctx)
{
    (void)ctx;
    RT_LOG(RT_TAG_VI) << "VIInit_caseD_2_801b9944 stubbed" << std::endl;
}
PPC_NATIVE_OVERRIDE_VOID(801B9944, VIInit_caseD_2_HLE_801b9944, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPreRetraceCallback (0x801B90F4)
// -----------------------------------------------------------------------------
extern "C" void VISetPreRetraceCallback_HLE_801b90f4(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.preRetraceCallback;
        g_vi.preRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(801B90F4, VISetPreRetraceCallback_HLE_801b90f4, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VISetPostRetraceCallback (0x801B9138)
// -----------------------------------------------------------------------------
extern "C" void VISetPostRetraceCallback_HLE_801b9138(CpuContext* ctx)
{
    const uint32_t newCb = ctx ? ctx->gpr[3] : 0;
    uint32_t prev = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        prev = g_vi.postRetraceCallback;
        g_vi.postRetraceCallback = newCb;
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, prev);
}
PPC_NATIVE_OVERRIDE_VOID(801B9138, VISetPostRetraceCallback_HLE_801b9138, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIGetDTVStatus (0x801BAD38)
// Reads DTV status from VI hardware (MMIO 0xCC00206E). Stub to "not ready".
// -----------------------------------------------------------------------------
extern "C" void VIGetDTVStatus_HLE_801bad38(CpuContext* ctx)
{
    ViSetR3(ctx, 0); // return 0 -> not ready / disabled
}
PPC_NATIVE_OVERRIDE_VOID(801BAD38, VIGetDTVStatus_HLE_801bad38, (CpuContext* ctx), (ctx));

// -----------------------------------------------------------------------------
// VIConfigure & related helpers: translate GXRenderModeObj into guest globals.
// -----------------------------------------------------------------------------
extern "C" void VIConfigure_HLE_801b9f6c(CpuContext* ctx)
{
    // One read of the guest GXRenderModeObj serves both the VI pending state and
    // aurora, rather than unpacking the same 0x39 bytes twice.
    const uint32_t renderModePtr = ctx ? ctx->gpr[3] : 0;
    GXRenderModeObj renderMode{};
    if (!ReadGuestRenderModeObj(renderModePtr, renderMode)) {
        RT_LOG(RT_TAG_VI) << "VIConfigure: invalid GXRenderModeObj pointer 0x"
                  << std::hex << renderModePtr << std::dec << std::endl;
        ViSetR3(ctx, 0);
        return;
    }

    const uint32_t decodedTvFormat =
        ExtractTvFormat(static_cast<uint32_t>(renderMode.viTVmode));
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingTvFormat = decodedTvFormat;
        g_vi.pendingRetraceInterval = IntervalForFormat(g_vi.pendingTvFormat);
        g_vi.pendingRenderWidth =
            renderMode.viWidth != 0 ? renderMode.viWidth : renderMode.fbWidth;
        g_vi.pendingRenderHeight =
            renderMode.viHeight != 0 ? renderMode.viHeight : renderMode.xfbHeight;
        g_vi.pendingViXOrigin = renderMode.viXOrigin;
        g_vi.pendingViYOrigin = renderMode.viYOrigin;
        g_vi.pendingXfbWidth = renderMode.fbWidth;
        g_vi.pendingXfbHeight =
            renderMode.xfbHeight != 0 ? renderMode.xfbHeight : renderMode.efbHeight;
    }

    ::VIConfigure(&renderMode);

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801B9F6C, VIConfigure_HLE_801b9f6c, (CpuContext* ctx), (ctx));

extern "C" void VIFlush_HLE_801ba9a4(CpuContext* ctx)
{
    uint32_t guestNextFb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();

        if (g_vi.pendingNextFrameBuffer == 0) {
            try {
                guestNextFb = Memory::Read32(kViNextFrameBufferAddr);
            } catch (const Memory::AccessViolation&) {
                guestNextFb = 0;
            }
            if (guestNextFb == 0) {
                try {
                    guestNextFb = Memory::Read32(kViNextFrameBufferHwAddr);
                } catch (const Memory::AccessViolation&) {
                    guestNextFb = 0;
                }
            }
            if (guestNextFb != 0) {
                g_vi.pendingNextFrameBuffer = guestNextFb;
            }
        }
        
        // Arm only; the commit happens at the next retrace (see ViState).
        g_vi.flushArmed = true;
#if defined(__SWITCH__)
        {
            static std::atomic<int> flushLog{0};
            if (flushLog.fetch_add(1, std::memory_order_relaxed) < 20) {
                char trace[96];
                std::snprintf(trace, sizeof(trace), "[vi] VIFlush pendingBlack=%d", g_vi.pendingBlack ? 1 : 0);
                SwitchBootLogExternal(trace);
            }
        }
#endif
    }

    // NOTE: We do NOT set hasValidXfb here. Frame readiness is signaled ONLY by
    // GXCopyDisp (which sets hasValidXfb = true), as that's when the EFB->XFB
    // copy is complete and we have a valid frame to present.

    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BA9A4, VIFlush_HLE_801ba9a4, (CpuContext* ctx), (ctx));

extern "C" void VISetNextFrameBuffer_HLE_801baab8(CpuContext* ctx)
{
    const uint32_t fbPtr = ctx ? ctx->gpr[3] : 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingNextFrameBuffer = fbPtr;
        // Also update guest memory for SDK code that reads this directly
        WriteGuestStateLocked();
    }
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BAAB8, VISetNextFrameBuffer_HLE_801baab8, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextFrameBuffer_HLE_801bab24(CpuContext* ctx)
{
    uint32_t fb = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Return PENDING value - what was set by VISetNextFrameBuffer
        fb = g_vi.pendingNextFrameBuffer;
    }
    ViSetR3(ctx, fb);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BAB24, VIGetNextFrameBuffer_HLE_801bab24, (CpuContext* ctx), (ctx));

extern "C" void VISetBlack_HLE_801bab2c(CpuContext* ctx)
{
    const bool makeBlack = ctx ? (ctx->gpr[3] != 0) : false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        // Write to PENDING state - will be committed on next retrace after VIFlush
        g_vi.pendingBlack = makeBlack;
    }
#if defined(__SWITCH__)
    {
        static std::atomic<int> blackLog{0};
        if (blackLog.fetch_add(1, std::memory_order_relaxed) < 20) {
            char trace[96];
            std::snprintf(trace, sizeof(trace), "[vi] VISetBlack(%d) retrace=%u", makeBlack ? 1 : 0,
                          VI_HLE_DebugRetraceCount());
            SwitchBootLogExternal(trace);
        }
    }
#endif
    ViSetR3(ctx, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801BAB2C, VISetBlack_HLE_801bab2c, (CpuContext* ctx), (ctx));

extern "C" void VIGetRetraceCount_HLE_801baba4(CpuContext* ctx)
{
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        count = g_vi.retraceCount;
    }
    ViSetR3(ctx, count);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BABA4, VIGetRetraceCount_HLE_801baba4, (CpuContext* ctx), (ctx));

extern "C" void VIGetNextField_HLE_801babac(CpuContext* ctx)
{
    bool fieldOdd = false;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        fieldOdd = g_vi.fieldOdd;
    }
    ViSetR3(ctx, fieldOdd ? 1 : 0);
    VI_HLE_PollRetrace(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(801BABAC, VIGetNextField_HLE_801babac, (CpuContext* ctx), (ctx));

extern "C" void VIGetCurrentLine_HLE_801bac48(CpuContext* ctx)
{
    uint32_t height = 480;
    std::chrono::microseconds interval{16666us};
    Clock::time_point last;
    {
        std::lock_guard<std::mutex> lock(g_viMutex);
        EnsureInitializedLocked();
        height = g_vi.xfbHeight;
        interval = g_vi.retraceInterval;
        last = g_vi.lastRetrace;
    }
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - last);
    uint32_t line = 0;
    if (interval.count() > 0 && height > 0) {
        const uint64_t scaled = static_cast<uint64_t>(elapsed.count()) * height;
        line = static_cast<uint32_t>(std::min<uint64_t>(height - 1, scaled / interval.count()));
    }
    ViSetR3(ctx, line);
}
PPC_NATIVE_OVERRIDE_VOID(801BAC48, VIGetCurrentLine_HLE_801bac48, (CpuContext* ctx), (ctx));

extern "C" void VIWaitForRetrace_HLE_801b99ec(CpuContext* ctx)
{
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    
    if (Fiber::GuestFiberManager::IsInitialized()) {
        const int32_t irqState = OS__DisableInterrupts_801a65ac();
        uint32_t retraceCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            retraceCount = g_vi.retraceCount;
        }

        // Don't make a late frame later. On a Wii at full speed this wait is how
        // a game paces itself to vblank; running below full speed, a frame ends
        // somewhere between two retraces and then sleeps until the next one -
        // half a retrace on average, measured as 11.6% of a race frame. If two or
        // more retraces have passed since the caller last waited, the frame has
        // already overrun by a whole vblank and there is nothing to pace: return.
        // A game keeping up never trips this, and it can never run anything
        // faster than real time. Guest threads are fibers on one host thread,
        // so plain statics are safe here.
        static uint32_t s_lastWaitReturn = 0;
        static bool s_waitedBefore = false;
        if (s_waitedBefore && retraceCount - s_lastWaitReturn >= 2u) {
            s_lastWaitReturn = retraceCount;
#if defined(__SWITCH__)
            const uint32_t skipped = g_viWaitsSkippedLate.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipped == 1 || (skipped % 600) == 0) {
                char trace[96];
                std::snprintf(trace, sizeof(trace), "[viwait] late frame, wait skipped (#%u)", skipped);
                SwitchBootLogExternal(trace);
            }
#endif
            OS__RestoreInterrupts_801a65d4(irqState);
            ViSetR3(cpu, 0);
            return;
        }

        do {
            cpu->gpr[3] = kViRetraceQueueAddr;
            OSSleepThread_HLE_801aa9b8(cpu);
#if defined(__SWITCH__)
            {
                static std::atomic<int> wakeReturnLog{0};
                const int index = wakeReturnLog.fetch_add(1, std::memory_order_relaxed);
                if (index < 10) {
                    char trace[128];
                    std::snprintf(trace, sizeof(trace), "[viwait] sleep returned #%d", index);
                    SwitchBootLogExternal(trace);
                }
            }
#endif

            {
                std::lock_guard<std::mutex> lock(g_viMutex);
                EnsureInitializedLocked();
                if (g_vi.retraceCount != retraceCount) {
                    s_lastWaitReturn = g_vi.retraceCount;
                    break;
                }
            }
        } while (true);
        s_waitedBefore = true;

        OS__RestoreInterrupts_801a65d4(irqState);
    } else {
        std::chrono::microseconds interval{16666us};
        Clock::time_point target;
        {
            std::lock_guard<std::mutex> lock(g_viMutex);
            EnsureInitializedLocked();
            interval = g_vi.retraceInterval;
            target = g_vi.lastRetrace + interval;
        }

        const auto now = Clock::now();
        if (now < target) {
            SleepPreciselyUntil(target, true);
        }
        AdvanceRetrace(cpu, target, true);
    }
    ViSetR3(cpu, 0);
}
PPC_NATIVE_OVERRIDE_VOID(801B99EC, VIWaitForRetrace_HLE_801b99ec, (CpuContext* ctx), (ctx));
