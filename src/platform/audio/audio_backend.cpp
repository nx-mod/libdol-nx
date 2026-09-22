#include "audio_backend.h"

#include <atomic>
#include <chrono>

#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#if defined(__SWITCH__)
// Defined at global scope in main.cpp. Declared here rather than inside the
// anonymous namespace below, which would give it internal linkage and fail to
// resolve at link time.
void SwitchBootLogExternal(const char* text) noexcept;

namespace {
// audren rather than audout, for two reasons that were bugs in the audout path:
// a voice carries its own sample rate and the renderer resamples to the device's
// 48 kHz, so the Wii's 32 kHz needs no conversion here; and buffer ownership
// lives in AudioDriverWaveBuf::state, which only the driver writes, so a buffer
// still being played cannot be mistaken for a free one.
constexpr uint32_t kSwitchAudioBufferBytes = 0x1000;
constexpr uint32_t kSwitchAudioBufferSamples = kSwitchAudioBufferBytes / sizeof(int16_t);
// Eight 4 KiB buffers: 512 stereo frames each, about 128 ms at 32 kHz.
constexpr uint32_t kSwitchAudioBufferCount = 8;
constexpr int kSwitchVoiceId = 0;

// The mempool has to be page-aligned and a whole number of pages; this is both.
alignas(0x1000) int16_t s_audioSampleData[kSwitchAudioBufferCount][kSwitchAudioBufferSamples];
AudioDriverWaveBuf s_waveBufs[kSwitchAudioBufferCount];
AudioDriver s_audioDriver;
bool s_audrenReady = false;
bool s_driverReady = false;

constexpr AudioRendererConfig kRendererConfig = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = 4,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

// RT_LOG goes to stderr, which nothing on the device collects: an audren
// failure was invisible in boot.log, so the backend looked fine while no sound
// came out. Say it where the logs are read.
void AudioLog(const char* what, Result rc) {
    char line[128];
    if (rc == 0) {
        std::snprintf(line, sizeof(line), "[audio] %s", what);
    } else {
        std::snprintf(line, sizeof(line), "[audio] %s failed: 0x%08X", what,
                      static_cast<unsigned>(rc));
    }
    RT_LOGF(RT_TAG_AUDIO, "%s\n", line);
    SwitchBootLogExternal(line);
}

void TeardownAudren() {
    if (s_driverReady) {
        audrvVoiceStop(&s_audioDriver, kSwitchVoiceId);
        audrvUpdate(&s_audioDriver);
        audrvClose(&s_audioDriver);
        s_driverReady = false;
    }
    if (s_audrenReady) {
        audrenExit();
        s_audrenReady = false;
    }
}
}  // namespace
#else
#include <SDL3/SDL_init.h>
#endif  // __SWITCH__

AudioBackend& AudioBackend::Instance() {
    static AudioBackend instance;
    return instance;
}

float AudioBackend::EffectiveGainLocked() const {
    return m_muted ? 0.0f : m_masterVolume;
}

void AudioBackend::ApplyGainLocked() {
#if defined(__SWITCH__)
    if (!s_driverReady) {
        return;
    }
    audrvMixSetVolume(&s_audioDriver, AUDREN_FINAL_MIX_ID, EffectiveGainLocked());
    audrvUpdate(&s_audioDriver);
#else
    if (m_stream && !SDL_SetAudioStreamGain(m_stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
    }
#endif
}

void AudioBackend::SetMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    ApplyGainLocked();
}

void AudioBackend::SetMuted(bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
    ApplyGainLocked();
}

bool AudioBackend::EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels) {
    if (m_initialized && m_sampleRate == sampleRate && m_channels == channels) {
        return true;
    }

    m_initialized = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_reportedDroppedBlock = false;

#if defined(__SWITCH__)
    // Stereo int16 is what the Wii's AI DMA delivers and what every caller here
    // converts to; the voice is told the guest's rate and audren resamples.
    if (channels != 2) {
        RT_LOG(RT_TAG_AUDIO) << "audren voice is stereo; requested " << channels
                  << " ch ignored" << std::endl;
        channels = 2;
    }

    TeardownAudren();

    Result rc = audrenInitialize(&kRendererConfig);
    if (R_FAILED(rc)) {
        AudioLog("audrenInitialize", rc);
        return false;
    }
    s_audrenReady = true;

    rc = audrvCreate(&s_audioDriver, &kRendererConfig, 2);
    if (R_FAILED(rc)) {
        AudioLog("audrvCreate", rc);
        TeardownAudren();
        return false;
    }
    s_driverReady = true;

    const int poolId = audrvMemPoolAdd(&s_audioDriver, s_audioSampleData, sizeof(s_audioSampleData));
    if (poolId < 0 || !audrvMemPoolAttach(&s_audioDriver, poolId)) {
        AudioLog("audrvMemPoolAdd/Attach", 1);
        TeardownAudren();
        return false;
    }

    static const u8 kSinkChannels[] = {0, 1};
    if (audrvDeviceSinkAdd(&s_audioDriver, AUDREN_DEFAULT_DEVICE_NAME, 2, kSinkChannels) < 0) {
        AudioLog("audrvDeviceSinkAdd", 1);
        TeardownAudren();
        return false;
    }

    rc = audrvUpdate(&s_audioDriver);
    if (R_FAILED(rc)) {
        AudioLog("audrvUpdate", rc);
        TeardownAudren();
        return false;
    }

    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) {
        AudioLog("audrenStartAudioRenderer", rc);
        TeardownAudren();
        return false;
    }

    if (!audrvVoiceInit(&s_audioDriver, kSwitchVoiceId, static_cast<int>(channels),
                        PcmFormat_Int16, static_cast<int>(sampleRate))) {
        AudioLog("audrvVoiceInit", 1);
        TeardownAudren();
        return false;
    }
    audrvVoiceSetDestinationMix(&s_audioDriver, kSwitchVoiceId, AUDREN_FINAL_MIX_ID);
    // Straight through: source channel 0 to the left of the final mix, 1 to the right.
    audrvVoiceSetMixFactor(&s_audioDriver, kSwitchVoiceId, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&s_audioDriver, kSwitchVoiceId, 1.0f, 1, 1);
    audrvVoiceStart(&s_audioDriver, kSwitchVoiceId);

    for (uint32_t i = 0; i < kSwitchAudioBufferCount; ++i) {
        s_waveBufs[i] = AudioDriverWaveBuf{};
        s_waveBufs[i].data_raw = s_audioSampleData[i];
        s_waveBufs[i].size = kSwitchAudioBufferBytes;
        s_waveBufs[i].start_sample_offset = 0;
        s_waveBufs[i].end_sample_offset = 0;
        s_waveBufs[i].state = AudioDriverWaveBufState_Free;
        std::memset(s_audioSampleData[i], 0, sizeof(s_audioSampleData[i]));
    }
    armDCacheFlush(s_audioSampleData, sizeof(s_audioSampleData));
    m_nxWriteSlot = 0;
    m_nxWriteBytes = 0;

    ApplyGainLocked();
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_initialized = true;
    return true;
#else
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = static_cast<int>(channels);
    spec.freq = static_cast<int>(sampleRate);

    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_OpenAudioDeviceStream failed: " << SDL_GetError() << std::endl;
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(stream)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_ResumeAudioStreamDevice failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        return false;
    }

    if (!SDL_SetAudioStreamGain(stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        return false;
    }

    m_stream = stream;
    m_spec = spec;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_initialized = true;
    return true;
#endif  // __SWITCH__
}

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return EnsureInitializedLocked(sampleRate, channels);
}

void AudioBackend::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(__SWITCH__)
    if (!m_initialized) {
        return;
    }
    if (s_driverReady) {
        audrvMixSetVolume(&s_audioDriver, AUDREN_FINAL_MIX_ID, 0.0f);
        audrvUpdate(&s_audioDriver);
    }
    TeardownAudren();
#else
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
#endif  // __SWITCH__
    m_initialized = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_nxWriteSlot = 0;
    m_nxWriteBytes = 0;
    m_reportedDroppedBlock = false;
    m_convertBuffer.clear();
}

uint32_t AudioBackend::QueuedBytesLocked() const {
#if defined(__SWITCH__)
    // Wavebuf states only change when audrvUpdate() reads them back from the
    // renderer. Counting them without refreshing first deadlocked: once the
    // ring filled, every push was dropped here before reaching the code that
    // updates, so the ring looked full forever - measured at 99.7% of blocks
    // dropped while the mix itself was producing full-scale audio.
    if (s_driverReady) {
        audrvUpdate(&s_audioDriver);
    }
    uint32_t queued = m_nxWriteBytes;   // the buffer still being filled
    for (uint32_t i = 0; i < kSwitchAudioBufferCount; ++i) {
        const AudioDriverWaveBufState state = s_waveBufs[i].state;
        if (state == AudioDriverWaveBufState_Waiting ||
            state == AudioDriverWaveBufState_Queued ||
            state == AudioDriverWaveBufState_Playing) {
            queued += kSwitchAudioBufferBytes;
        }
    }
    return queued;
#else
    if (!m_stream) {
        return 0;
    }
    const int queued = SDL_GetAudioStreamQueued(m_stream);
    return queued > 0 ? static_cast<uint32_t>(queued) : 0;
#endif  // __SWITCH__
}

uint32_t AudioBackend::QueueLimitBytesLocked() const {
    constexpr uint32_t queueMs = 120;

    const uint64_t bytesPerSecond = static_cast<uint64_t>(m_sampleRate != 0 ? m_sampleRate : 48000) *
                                    static_cast<uint64_t>(m_channels != 0 ? m_channels : 2) *
                                    sizeof(int16_t);
    return static_cast<uint32_t>((bytesPerSecond * queueMs) / 1000u);
}

bool AudioBackend::QueueHasCapacityLocked(int incomingBytes) {
#if defined(__SWITCH__)
    const uint32_t maxQueued = QueueLimitBytesLocked();
    // 120 ms at 32 kHz stereo is 15,360 bytes - under the ring's 32 KiB, so
    // this is the backpressure point, and it depends on QueuedBytesLocked()
    // reading fresh wavebuf states.
    if (static_cast<uint64_t>(QueuedBytesLocked()) + static_cast<uint64_t>(std::max(incomingBytes, 0)) >
        maxQueued) {
        if (!m_reportedDroppedBlock) {
            m_reportedDroppedBlock = true;
            RT_LOG(RT_TAG_AUDIO) << "output queue full; dropping blocks to preserve continuity" << std::endl;
        }
        return false;
    }
    return true;
#else
    if (!m_stream) {
        return false;
    }
    const int queued = SDL_GetAudioStreamQueued(m_stream);
    if (queued < 0) {
        return true;
    }
    const uint32_t maxQueued = QueueLimitBytesLocked();
    if (static_cast<uint64_t>(queued) + static_cast<uint64_t>(std::max(incomingBytes, 0)) > maxQueued) {
        // Match Dolphin's FIFO overflow behavior: preserve the continuous audio
        // already queued and discard the new block.  Clearing SDL's entire
        // stream creates an audible discontinuity (the severe crackle seen when
        // VI-batched DMA briefly outran playback).
        // A drop is audible; report the first one so it is not invisible.
        if (!m_reportedDroppedBlock) {
            m_reportedDroppedBlock = true;
            RT_LOG(RT_TAG_AUDIO) << "output queue full (" << queued << "/" << maxQueued
                      << " bytes); dropping blocks to preserve continuity" << std::endl;
        }
        return false;
    }
    return true;
#endif  // __SWITCH__
}

#if defined(__SWITCH__)
bool AudioBackend::AppendSamplesLocked(const int16_t* samples, size_t sampleCount) {
    if (!s_driverReady) {
        return false;
    }

    // Report once when the renderer starts accepting audio, so a silent run can
    // be told from one that never got here.
    static bool loggedFirstSubmit = false;

    size_t srcIndex = 0;
    while (srcIndex < sampleCount) {
        AudioDriverWaveBuf& wave = s_waveBufs[m_nxWriteSlot];

        // Only the driver writes `state`, so this cannot mistake a buffer that
        // is still being played for a free one - which is exactly what the old
        // audout path did by clearing its own data_size after submitting.
        if (wave.state != AudioDriverWaveBufState_Free &&
            wave.state != AudioDriverWaveBufState_Done) {
            audrvUpdate(&s_audioDriver);
            if (wave.state != AudioDriverWaveBufState_Free &&
                wave.state != AudioDriverWaveBufState_Done) {
                // Every buffer is still in flight: the guest produces faster
                // than the renderer consumes. Drop the rest of this push rather
                // than overwrite audio the renderer is reading - but report
                // success, because the caller treats false as "this DMA buffer
                // is unreadable" and disables audio for the rest of the run.
                return true;
            }
        }

        const size_t roomSamples = kSwitchAudioBufferSamples - (m_nxWriteBytes / sizeof(int16_t));
        const size_t copySamples = std::min(roomSamples, sampleCount - srcIndex);
        std::memcpy(&s_audioSampleData[m_nxWriteSlot][m_nxWriteBytes / sizeof(int16_t)],
                    &samples[srcIndex], copySamples * sizeof(int16_t));
        m_nxWriteBytes += static_cast<uint32_t>(copySamples * sizeof(int16_t));
        srcIndex += copySamples;

        if (m_nxWriteBytes < kSwitchAudioBufferBytes) {
            continue;   // partially filled; the next push tops it up
        }

        // end_sample_offset counts frames, not samples: stereo int16 is 4 bytes
        // a frame. Getting this wrong plays a fraction of each buffer.
        wave.start_sample_offset = 0;
        wave.end_sample_offset =
            static_cast<s32>(m_nxWriteBytes / (sizeof(int16_t) * m_channels));
        armDCacheFlush(s_audioSampleData[m_nxWriteSlot], m_nxWriteBytes);

        if (!audrvVoiceAddWaveBuf(&s_audioDriver, kSwitchVoiceId, &wave)) {
            AudioLog("audrvVoiceAddWaveBuf", 1);
            return false;
        }
        if (!audrvVoiceIsPlaying(&s_audioDriver, kSwitchVoiceId)) {
            audrvVoiceStart(&s_audioDriver, kSwitchVoiceId);
        }
        if (!loggedFirstSubmit) {
            loggedFirstSubmit = true;
            AudioLog("first wavebuf submitted to audren", 0);
        }
        audrvUpdate(&s_audioDriver);

        m_nxWriteSlot = (m_nxWriteSlot + 1) % kSwitchAudioBufferCount;
        m_nxWriteBytes = 0;
    }

    return true;
}
#else
bool AudioBackend::AppendSamplesLocked(const int16_t* samples, size_t sampleCount) {
    const int lenBytes = static_cast<int>(sampleCount * sizeof(int16_t));
    if (!SDL_PutAudioStreamData(m_stream, samples, lenBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
        return false;
    }
    return true;
}
#endif  // __SWITCH__

#if defined(__SWITCH__)
// Defined at global scope in main.cpp.
void SwitchBootLogExternal(const char* text) noexcept;
bool SwitchDevLoggingEnabled() noexcept;

// No sound came out of a backend that reported a clean start, so count what
// actually reaches it: pushes from the mixer, samples submitted to audout, and
// blocks dropped for lack of queue space.
namespace {
std::atomic<uint64_t> g_audioPushes{0};
std::atomic<uint64_t> g_audioSamples{0};
std::atomic<uint64_t> g_audioDropped{0};
// Loudest sample since the last report. Zero means the mix upstream renders
// silence, and the backend is playing it faithfully.
std::atomic<int32_t> g_audioPeak{0};
std::atomic<int64_t> g_audioLastReport{0};

void AudioStatsTick(size_t samples, bool dropped) {
    g_audioPushes.fetch_add(1, std::memory_order_relaxed);
    g_audioSamples.fetch_add(samples, std::memory_order_relaxed);
    if (dropped) {
        g_audioDropped.fetch_add(1, std::memory_order_relaxed);
    }
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const int64_t last = g_audioLastReport.load(std::memory_order_relaxed);
    if (now - last < 3000 || !SwitchDevLoggingEnabled()) {
        return;
    }
    g_audioLastReport.store(now, std::memory_order_relaxed);
    char line[160];
    std::snprintf(line, sizeof(line), "[audio] pushes=%llu samples=%llu dropped=%llu peak=%d",
                  static_cast<unsigned long long>(g_audioPushes.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_audioSamples.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_audioDropped.load(std::memory_order_relaxed)),
                  static_cast<int>(g_audioPeak.exchange(0, std::memory_order_relaxed)));
    SwitchBootLogExternal(line);
}
}  // namespace
#endif

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes) {
#if defined(__SWITCH__)
    AudioStatsTick(bytes / sizeof(int16_t), false);
#endif
    if (!data || bytes == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        return false;
    }

    const size_t sampleCount = bytes / sizeof(int16_t);
    if (sampleCount == 0) {
        return false;
    }

    if (m_convertBuffer.size() < sampleCount) {
        m_convertBuffer.resize(sampleCount);
    }

    const size_t frameCount = sampleCount / 2;
#if defined(__SWITCH__)
    int32_t peak = 0;
#endif
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const size_t rightOffset = frame * 4;
        const size_t leftOffset = rightOffset + 2;
        const uint16_t right = static_cast<uint16_t>(data[rightOffset]) << 8 |
                               static_cast<uint16_t>(data[rightOffset + 1]);
        const uint16_t left = static_cast<uint16_t>(data[leftOffset]) << 8 |
                              static_cast<uint16_t>(data[leftOffset + 1]);
        m_convertBuffer[frame * 2] = static_cast<int16_t>(left);
        m_convertBuffer[frame * 2 + 1] = static_cast<int16_t>(right);
#if defined(__SWITCH__)
        const int32_t l = static_cast<int16_t>(left), r = static_cast<int16_t>(right);
        peak = std::max(peak, std::max(l < 0 ? -l : l, r < 0 ? -r : r));
#endif
    }
#if defined(__SWITCH__)
    for (int32_t seen = g_audioPeak.load(std::memory_order_relaxed);
         peak > seen && !g_audioPeak.compare_exchange_weak(seen, peak, std::memory_order_relaxed);) {
    }
#endif

    const size_t lenSamples = frameCount * 2;
    const int lenBytes = static_cast<int>(lenSamples * sizeof(int16_t));
    if (!QueueHasCapacityLocked(lenBytes)) {
#if defined(__SWITCH__)
        // This drop used to be silent - it returned before anything counted it.
        g_audioDropped.fetch_add(1, std::memory_order_relaxed);
#endif
        return true;
    }
    return AppendSamplesLocked(m_convertBuffer.data(), lenSamples);
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount) {
#if defined(__SWITCH__)
    AudioStatsTick(sampleCount, false);
#endif
    if (!samples || sampleCount == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        return false;
    }

    const int lenBytes = static_cast<int>(sampleCount * sizeof(int16_t));
    if (!QueueHasCapacityLocked(lenBytes)) {
        return true;
    }
    return AppendSamplesLocked(samples, sampleCount);
}