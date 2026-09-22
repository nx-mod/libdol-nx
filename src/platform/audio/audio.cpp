#include "memory.h"
#include "guest_interrupt_context.h"
#include "hle_stubs.h"
#include "ppc_runtime.h"
#include "audio_backend.h"
#include "ax_dsp.h"
#include "music_attenuation.h"
#include "runtime_log.h"
#include "mkw_thread_local.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <mutex>
#include <vector>

#if defined(__SWITCH__)
// 41 ms of every 113 ms frame is spent in this poll, measured on hardware, for
// audio that is currently silent. Split the block loop into its parts: waiting
// on the mix worker, pushing the DMA buffer, and the guest's own AI callback
// (__AXOutNewFrame, which is game code). Reported per frame on the [idle] line.
std::atomic<uint64_t> g_audioJoinUs{0};
std::atomic<uint64_t> g_audioPushUs{0};
std::atomic<uint64_t> g_audioGuestUs{0};
std::atomic<uint64_t> g_audioDeferredUs{0};
std::atomic<uint32_t> g_audioBlocks{0};
#define AUDIO_PHASE(counter, call)                                                       \
    do {                                                                                 \
        const auto audioPhaseStart = std::chrono::steady_clock::now();                   \
        call;                                                                            \
        (counter).fetch_add(                                                             \
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>( \
                std::chrono::steady_clock::now() - audioPhaseStart).count()),            \
            std::memory_order_relaxed);                                                  \
    } while (0)
#else
#define AUDIO_PHASE(counter, call) do { call; } while (0)
#endif

namespace {
constexpr uint32_t kDefaultSampleRate = 32000u;
constexpr uint32_t kAudioChannels = 2u;
constexpr uint32_t kBytesPerSample = 2u;
constexpr uint32_t kAIInitializedAddr = 0x80386448u;
constexpr uint32_t kAICallbackBusyAddr = 0x8038644Cu;
constexpr uint32_t kAICallbackStackSwitchAddr = 0x8038647Cu;
constexpr uint32_t kAIDmaCallbackAddr = 0x80386480u;

// Max completed 3 ms DMA blocks delivered per tick. This has to cover a whole frame:
// in a race the game rarely idles, so the tick runs about once per frame, and at the
// old cap of 4 (12 ms of audio per 40-70 ms frame) the mixer ran at a third of real
// time - measured as exactly 4.0 blocks per frame against 14-25 needed, from the
// moment the race loaded. The spiral this guarded against is now bounded by the
// backlog caps below (at most 0.5 s, ~166 blocks), so 64 per tick only ever runs
// what real time has actually earned.
constexpr int kMaxBlocksPerTick = 64;

// With catch-up off, how far behind real time the AI DMA clock may fall before
// the missed time is dropped (see Audio_HLE_Tick). The tick runs about once per
// frame, and a frame on a slow-but-fine screen is 50-125 ms, so this has to
// cover a whole frame: a first try at two blocks (6 ms) starved every screen.
constexpr double kMaxAudioBacklogSeconds = 0.150;

// With catch-up on, the backlog is still bounded. Unbounded, a slow stretch
// piled up seconds of audio that then drained only a little faster than real
// time - heard as sound that took a long time to come back in sync. Half a
// second rides out any ordinary hitch and recovers quickly from a real one.
constexpr double kMaxCatchUpSeconds = 0.500;

// The [audio] catch_up setting: replay the backlog instead of dropping it.
// On by default - it is what sounded right on hardware. Flipped live from the
// settings overlay, read once per tick.
std::atomic<bool> g_audioCatchUp{true};

struct AIDmaState {
    std::mutex mutex;
    uint32_t startAddr = 0;
    uint32_t registerStartAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    bool enabled = false;
    uint32_t sampleRate = kDefaultSampleRate;
    uint32_t bytesLeft = 0;
    double accumulatorSeconds = 0.0;
    bool tickActive = false;
    bool loggedBackendFailure = false;
    bool loggedMissingCallback = false;
    bool loggedAccessFailure = false;
};

AIDmaState g_ai{};

// Audio degradation is invisible to the player except as silence, so every
// notice below reaches stderr unconditionally. The ones that sit on the
// per-DMA-frame path keep their one-shot latch in g_ai.
void ReportAudioProblem(const char* who, const char* what) {
    RT_LOGF(RT_TAG_AUDIO, "%s: %s\n", who, what);
    std::fflush(stderr);
}

bool EnsureAudioBackend(uint32_t sampleRate) {
    return AudioBackend::Instance().Init(sampleRate, kAudioChannels);
}

uint32_t EncodeAIDmaStartRegister(uint32_t startAddr) {
    return startAddr & 0x1fffffe0u;
}

uint32_t EncodeAIDmaLengthRegister(uint32_t length) {
    return length & 0x000fffe0u;
}

bool PushAudioBlock(uint32_t startAddr, uint32_t length) {
    if (startAddr == 0 || length == 0) {
        return false;
    }
    const uint32_t bytes = length;
    const uint8_t* src = nullptr;
    try {
        src = static_cast<const uint8_t*>(Memory::GetPointer(startAddr, bytes));
    } catch (const Memory::AccessViolation&) {
        src = nullptr;
    }

    if (src) {
        return AudioBackend::Instance().PushWiiAiSamplesBE16(src, bytes);
    }

    const uint32_t sampleCount = bytes / kBytesPerSample;
    if (sampleCount == 0) {
        return false;
    }
    std::vector<int16_t> samples(sampleCount);
    try {
        for (uint32_t i = 0; i < sampleCount; ++i) {
            const uint32_t addr = startAddr + i * kBytesPerSample;
            samples[i] = static_cast<int16_t>(Memory::Read16(addr));
        }
    } catch (const Memory::AccessViolation&) {
        return false;
    }
    // Memory::Read16 has converted endianness, but the Wii AI frame order is
    // still right, left. Convert it to the host's left, right convention.
    for (uint32_t i = 0; i + 1 < sampleCount; i += 2) {
        std::swap(samples[i], samples[i + 1]);
    }
    return AudioBackend::Instance().PushSamplesLE16(samples.data(), samples.size());
}

} // namespace

extern "C" void AIClockInit_801A1138(uint32_t clock_mode)
{
    // clock_mode is unused: AID/DSP rate is controlled separately by AI state, and
    // treating it as a sample-rate switch would break Wii AX's normal 32 kHz cadence.
    (void)clock_mode;
    uint32_t rate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        rate = g_ai.sampleRate;
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("__AIClockInit", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(801A1138, AIClockInit_801A1138, (uint32_t clock_mode), (clock_mode));

extern "C" void OSInitAudioSystem_801A1358()
{
    AIClockInit_801A1138(1);
    AxDspHle::InitAram();
    AxDspHle::Init();
    if (!EnsureAudioBackend(kDefaultSampleRate)) {
        ReportAudioProblem("__OSInitAudioSystem", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(801A1358, OSInitAudioSystem_801A1358, (), ());

extern "C" void OSStopAudioSystem_801A1520()
{
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.enabled = false;
        g_ai.registerStartAddr = 0;
        g_ai.bytesLeft = 0;
        g_ai.accumulatorSeconds = 0.0;
    }
    AxDspHle::Stop();
}

PPC_NATIVE_OVERRIDE_VOID(801A1520, OSStopAudioSystem_801A1520, (), ());



// Do NOT stub Audio__Manager__Init_80717150 / Audio__Manager__InitSelf_8071724c: they must
// run translated to init AudioHandleHolder::sInstance, or createSceneSoundManager NULL-vtable crashes.


extern "C" void AIInit_801240b0(uint32_t callback_stack_switch)
{
    const uint32_t rate = kDefaultSampleRate;
    uint32_t initialized = 0;
    const bool alreadyInitialized = Memory::TryRead32(kAIInitializedAddr, initialized) && initialized == 1u;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.sampleRate = rate;
        if (!alreadyInitialized) {
            g_ai.callback = 0;
            g_ai.loggedMissingCallback = false;
            g_ai.loggedAccessFailure = false;
        }
    }
    if (!alreadyInitialized) {
        Memory::TryWrite32(kAIDmaCallbackAddr, 0);
        Memory::TryWrite32(kAICallbackBusyAddr, 0);
        Memory::TryWrite32(kAICallbackStackSwitchAddr, callback_stack_switch);
        Memory::TryWrite32(kAIInitializedAddr, 1);
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("AIInit", "audio backend init failed");
    } else {
        RT_LOG(RT_TAG_AUDIO) << "AIInit_801240b0 called: Audio subsystem initialized (HLE)" << std::endl;
    }
}

PPC_NATIVE_OVERRIDE_VOID(801240b0, AIInit_801240b0, (uint32_t callback_stack_switch), (callback_stack_switch));

extern "C" uint32_t AICheckInit_80124094()
{
    uint32_t initialized = 0;
    Memory::TryRead32(kAIInitializedAddr, initialized);
    return initialized;
}
REGISTER_NATIVE_FUNCTION(0x80124094, AICheckInit_80124094);



extern "C" void DSPInit_8015d444()
{
    AxDspHle::Init();
    RT_LOG(RT_TAG_AUDIO) << "DSPInit_8015d444 called: DSP hardware boundary initialized (HLE)" << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(8015d444, DSPInit_8015d444, (), ());

extern "C" uint32_t DSPCheckInit_8015d504()
{
    return AxDspHle::CheckInit();
}
REGISTER_NATIVE_FUNCTION(0x8015D504, DSPCheckInit_8015d504);

extern "C" uint32_t DSPAddTask_8015d50c(uint32_t task_ptr)
{
    return AxDspHle::AddTask(task_ptr);
}
REGISTER_NATIVE_FUNCTION(0x8015D50C, DSPAddTask_8015d50c);

extern "C" void __DSP_boot_task_8015dc60(uint32_t task_ptr)
{
    AxDspHle::AssertTask(task_ptr);
    RT_LOG(RT_TAG_AUDIO) << "__DSP_boot_task called: booted DSP task at 0x"
              << std::hex << task_ptr << std::dec << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(8015dc60, __DSP_boot_task_8015dc60, (uint32_t task_ptr), (task_ptr));


extern "C" void __AXOutInitDSP_801269bc(CpuContext* ctx)
{
    AxDspHle::InitForAXOut(ctx);
    RT_LOG(RT_TAG_AUDIO) << "__AXOutInitDSP called: native AX/DSP HLE initialized." << std::endl;
}

PPC_NATIVE_OVERRIDE_VOID(801269bc, __AXOutInitDSP_801269bc, (CpuContext* ctx), (ctx));



extern "C" void AIInitDMA_80123fcc(uint32_t start_addr, uint32_t length)
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    g_ai.startAddr = start_addr;
    g_ai.registerStartAddr = EncodeAIDmaStartRegister(start_addr);
    g_ai.length = EncodeAIDmaLengthRegister(length);
    g_ai.bytesLeft = g_ai.length;
}

PPC_NATIVE_OVERRIDE_VOID(80123fcc, AIInitDMA_80123fcc, (uint32_t start_addr, uint32_t length), (start_addr, length));



// AIRegisterDMACallback stores the callback in the guest global at 0x80386480.
// Returns the old callback pointer.
extern "C" uint32_t AIRegisterDMACallback_80123f88(uint32_t callback)
{
    uint32_t old_callback = 0;
    Memory::TryRead32(kAIDmaCallbackAddr, old_callback);
    Memory::TryWrite32(kAIDmaCallbackAddr, callback);
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.callback = callback;
        g_ai.loggedMissingCallback = false;
    }
    return old_callback;
}
PPC_NATIVE_OVERRIDE(80123f88, AIRegisterDMACallback_80123f88, uint32_t, (uint32_t callback), (callback));

// AIStartDMA toggles the AI DMA control register on hardware. Keep the guest-visible
// DMA state here and let the VI tick advance the hardware boundary.
extern "C" void AIStartDMA_80124048()
{
    const uint32_t rate = kDefaultSampleRate;
    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        g_ai.sampleRate = rate;
        g_ai.enabled = true;
        g_ai.bytesLeft = g_ai.length;
    }
    if (!EnsureAudioBackend(rate)) {
        ReportAudioProblem("AIStartDMA", "audio backend init failed");
    }
}

PPC_NATIVE_OVERRIDE_VOID(80124048, AIStartDMA_80124048, (), ());

extern "C" uint32_t AIGetDMABytesLeft_8012405c()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.bytesLeft;
}
PPC_NATIVE_OVERRIDE(8012405C, AIGetDMABytesLeft_8012405c, uint32_t, (), ());

extern "C" uint32_t AIGetDMAStartAddr_8012406c()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.registerStartAddr;
}
PPC_NATIVE_OVERRIDE(8012406C, AIGetDMAStartAddr_8012406c, uint32_t, (), ());

extern "C" uint32_t AIGetDMALength_80124084()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    return g_ai.length;
}
PPC_NATIVE_OVERRIDE(80124084, AIGetDMALength_80124084, uint32_t, (), ());

extern "C" uint32_t AIGetDSPSampleRate_8012409c()
{
    std::lock_guard<std::mutex> lock(g_ai.mutex);
    // SDK AIGetDSPSampleRate returns AIDFR^1: 0 for 32 kHz, 1 for 48 kHz.
    return (g_ai.sampleRate == 48000u) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(8012409C, AIGetDSPSampleRate_8012409c, uint32_t, (), ());

extern "C" void DSPSendMailToDSP_8015d430(uint32_t mail)
{
    AxDspHle::SendMailToDSP(mail);
}
PPC_NATIVE_OVERRIDE_VOID(8015D430, DSPSendMailToDSP_8015d430, (uint32_t mail), (mail));

extern "C" void SoundPlayerSetVolume_800a35e0(uint32_t soundPlayer, float volume)
{
    MusicAttenuation::SetSoundPlayerVolume(soundPlayer, volume);
}

PPC_NATIVE_OVERRIDE_VOID(800A35E0, SoundPlayerSetVolume_800a35e0,
              (uint32_t soundPlayer, float volume), (soundPlayer, volume));

extern "C" uint32_t DSPCheckMailToDSP_8015d3fc()
{
    return AxDspHle::CheckMailToDSP();
}
PPC_NATIVE_OVERRIDE(8015D3FC, DSPCheckMailToDSP_8015d3fc, uint32_t, (), ());

extern "C" uint32_t DSPCheckMailFromDSP_8015d40c()
{
    return AxDspHle::CheckMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x8015D40C, DSPCheckMailFromDSP_8015d40c);

extern "C" uint32_t DSPReadMailFromDSP_8015d41c()
{
    return AxDspHle::ReadMailFromDSP();
}
REGISTER_NATIVE_FUNCTION(0x8015D41C, DSPReadMailFromDSP_8015d41c);

extern "C" uint32_t DSPAssertTask_8015d57c(uint32_t taskPtr)
{
    return AxDspHle::AssertTask(taskPtr);
}
PPC_NATIVE_OVERRIDE(8015D57C, DSPAssertTask_8015d57c, uint32_t, (uint32_t taskPtr), (taskPtr));

// Each delivered block runs the AI DMA callback and deferred AX task callbacks before the
// next block, preserving the SoundThread/DSP interleave order real hardware provides.
void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros)
{
    uint32_t startAddr = 0;
    uint32_t length = 0;
    uint32_t callback = 0;
    uint32_t sampleRate = kDefaultSampleRate;
    bool enabled = false;

    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        enabled = g_ai.enabled;
        startAddr = g_ai.startAddr;
        length = g_ai.length;
        callback = g_ai.callback;
        sampleRate = g_ai.sampleRate;

        if (enabled && startAddr != 0 && length != 0 && sampleRate != 0) {
            // SoundThread can hit the idle scheduler before the outer AXOut frame finishes;
            // retain elapsed time here rather than recursively entering the singleton AI/AX device.
            g_ai.accumulatorSeconds += static_cast<double>(deltaMicros) / 1'000'000.0;
            // With catch-up off, drop the time the game could not keep up with
            // rather than replaying it. The game mixes audio at its own speed, so
            // on a slow screen the backlog grows, and draining it later has the
            // game mix faster than real time - heard as audio racing past normal
            // speed once a screen gets fast again. Off trades that for choppy
            // audio while a screen is slow.
            g_ai.accumulatorSeconds = std::min(
                g_ai.accumulatorSeconds,
                g_audioCatchUp.load(std::memory_order_relaxed) ? kMaxCatchUpSeconds
                                                               : kMaxAudioBacklogSeconds);
            if (g_ai.tickActive) {
                return;
            }
            g_ai.tickActive = true;
        }
    }

    if (!enabled || startAddr == 0 || length == 0 || sampleRate == 0) {
        return;
    }

    // Resets tickActive on early return; the normal exit path disarms this and clears the
    // flag itself while already holding the mutex, avoiding a redundant lock acquisition.
    struct ActiveTickReset {
        bool armed = true;
        ~ActiveTickReset()
        {
            if (!armed) {
                return;
            }
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            g_ai.tickActive = false;
        }
    } activeTickReset;

    const double bytesPerSecond = static_cast<double>(sampleRate) * kAudioChannels * kBytesPerSample;
    const double blockDuration = static_cast<double>(length) / bytesPerSecond;
    if (blockDuration <= 0.0) {
        return;
    }

    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    CpuContextScope scope(cpu);

    int blocksCompleted = 0;
    while (true) {
        // Claim the block in one critical section; sample-then-consume separately gains
        // nothing since the callback below (the only reentrancy point) runs mutex-released.
        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (g_ai.accumulatorSeconds < blockDuration) {
                break;
            }
            g_ai.accumulatorSeconds -= blockDuration;
            g_ai.bytesLeft = 0;
        }

        // Everything below reads what the AX mix wrote (PushAudioBlock) or runs guest code
        // that reads its PB write-back and aux buffers (__AXOutNewFrame via the AI DMA
        // callback), so the mix worker must finish first; the join also publishes its
        // aux-out shadow.
        AUDIO_PHASE(g_audioJoinUs, AxDspHle::JoinMixWorker());

        if (!EnsureAudioBackend(sampleRate)) {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            if (!g_ai.loggedBackendFailure) {
                g_ai.loggedBackendFailure = true;
                ReportAudioProblem("Audio", "audio backend unavailable; dropping samples");
            }
        } else {
            bool pushed = false;
            AUDIO_PHASE(g_audioPushUs, pushed = PushAudioBlock(startAddr, length));
            if (!pushed) {
                std::lock_guard<std::mutex> lock(g_ai.mutex);
                if (!g_ai.loggedAccessFailure) {
                    g_ai.loggedAccessFailure = true;
                    ReportAudioProblem("Audio", "failed to read DMA buffer; disabling audio DMA");
                }
                g_ai.enabled = false;
                return;
            }
        }

        if (callback != 0) {
            // Pointer lookup avoids copying the registry record per audio block.
            const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(callback);
            if (!info || !info->rawCpuInvoker) {
                std::lock_guard<std::mutex> lock(g_ai.mutex);
                if (!g_ai.loggedMissingCallback) {
                    g_ai.loggedMissingCallback = true;
                    ReportAudioProblem("Audio", "AI DMA callback not registered; skipping");
                }
            } else {
                Memory::TryWrite32(kAICallbackBusyAddr, 1);
                AUDIO_PHASE(g_audioGuestUs, InvokeIndirectCpu(callback, cpu));
                Memory::TryWrite32(kAICallbackBusyAddr, 0);
                AUDIO_PHASE(g_audioDeferredUs, AxDspHle::ServiceDeferredCallbacks());
                g_audioBlocks.fetch_add(1, std::memory_order_relaxed);
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_ai.mutex);
            startAddr = g_ai.startAddr;
            length = g_ai.length;
            callback = g_ai.callback;
            sampleRate = g_ai.sampleRate;
            g_ai.bytesLeft = g_ai.length;
        }

        if (++blocksCompleted >= kMaxBlocksPerTick) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_ai.mutex);
        if (g_ai.length != 0) {
            const double bytesRemaining = g_ai.length * (g_ai.accumulatorSeconds / blockDuration);
            if (bytesRemaining < static_cast<double>(g_ai.length)) {
                g_ai.bytesLeft = g_ai.length - static_cast<uint32_t>(bytesRemaining);
            }
        }
        g_ai.tickActive = false;
        activeTickReset.armed = false;
    }
}

namespace {

// Wall-clock delta since the previous poll, from whichever pump ran it. All
// pumps live on the guest thread, so the one thread_local cursor is shared and
// no interval is ever counted twice or dropped between them.
int64_t ConsumeAudioPollDeltaMicros()
{
    using Clock = std::chrono::steady_clock;
    static MKW_THREAD_LOCAL Clock::time_point lastPoll = Clock::now();

    const Clock::time_point now = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - lastPoll).count();
    lastPoll = now;

    if (elapsed < 0) {
        elapsed = 0;
    }

    // Cap the catch-up interval after a debugger pause or host stall; backlog drains via
    // kMaxBlocksPerTick per pass. Never return early on a zero delta: two scheduler passes
    // can land in the same microsecond and the backlog still needs servicing.
    constexpr int64_t kMaxPollDeltaMicros = 100'000;
    return std::min(elapsed, kMaxPollDeltaMicros);
}

} // namespace

namespace {

// The AI DMA model advances in 3 ms blocks, and Dolphin services it at 4 kHz.
// The scheduler's idle loop spins far faster than that, and every pass took
// g_ai.mutex and recomputed the elapsed interval for nothing: 9.5% of a
// profiled run sat in this poll. Skipping a pass loses no time - the interval
// stays unconsumed and the next poll sees all of it - so hold to the cadence
// the model actually has. 200 us is an order of magnitude under a block.
bool AudioPollDue()
{
    constexpr auto kMinInterval = std::chrono::microseconds(200);
    // Every pump runs on the guest thread, like the delta cursor above.
    static std::chrono::steady_clock::time_point lastPoll{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll < kMinInterval) {
        return false;
    }
    lastPoll = now;
    return true;
}

} // namespace

void Audio_HLE_Poll(CpuContext* ctx)
{
    if (!AudioPollDue()) {
        return;
    }
    MusicAttenuation::TickGuest();
    Audio_HLE_Tick(ctx, static_cast<uint32_t>(ConsumeAudioPollDeltaMicros()));
}

void Audio_HLE_PollDeferred()
{
    if (!OS_HLE_InterruptsEnabled()) {
        // Leave the elapsed interval unconsumed so the next poll still sees it.
        return;
    }

    GuestInterruptCallbackContext interrupt;
    CpuContext* cpu = interrupt.get();

    OS_HLE_BeginDeferredGuestCallbacks();
    try {
        Audio_HLE_Tick(cpu, static_cast<uint32_t>(ConsumeAudioPollDeltaMicros()));
    } catch (...) {
        OS_HLE_EndDeferredGuestCallbacks();
        throw;
    }
    OS_HLE_EndDeferredGuestCallbacks();
}

void Audio_HLE_SetCatchUp(bool enabled)
{
    g_audioCatchUp.store(enabled, std::memory_order_relaxed);
}
