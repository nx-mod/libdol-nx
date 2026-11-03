#include "abi_bridge.h"
#include "isa/big_endian.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "guest_helper_calls.h"


extern "C" __attribute__((weak)) void func_8012B830(CpuContext* ctx);

// PowerPC uses discrete fmuls/fadds; a fused multiply-add would change sample rounding.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace {
namespace ReverbStd {

constexpr uint32_t kSamplesPerFrame = 96;
constexpr uint32_t kChannels = 3;

// .sdata2 constants the guest function loads through r2.
// The SDK's own constants for this path, the same in every game.
constexpr float kOne = 1.0f;
constexpr float kSendScale = 0.6f;  // send pre-scale

// AXFX_REVERBSTD_EXP field offsets (byte offsets into the struct in r4).
constexpr uint32_t kFieldPreDelayCoef = 0x18;
constexpr uint32_t kFieldEarlyLength = 0x2C;
constexpr uint32_t kFieldComb1Coef = 0x64;
constexpr uint32_t kFieldComb2Coef = 0x68;
constexpr uint32_t kFieldAllpassCoef = 0x9C;
constexpr uint32_t kFieldLastAllpass = 0xA0; // + channel * 4
constexpr uint32_t kFieldDamping = 0xAC;
constexpr uint32_t kFieldFlags = 0xB0;
constexpr uint32_t kFieldDryPreScale = 0xD0;
constexpr uint32_t kFieldWetPreScale = 0xD4;
constexpr uint32_t kFieldAuxInputBuffers = 0xD8;
constexpr uint32_t kFieldAuxOutputBuffers = 0xDC;
constexpr uint32_t kFieldMainOutGain = 0xE0;
constexpr uint32_t kFieldAuxOutGain = 0xE4;
constexpr uint32_t kStateStructBytes = 0xE8;

enum RingId : uint32_t {
    kRingPreDelay = 0,
    kRingEarly,
    kRingComb1,
    kRingComb2,
    kRingAllpass1,
    kRingAllpass2,
    kRingCount,
};

struct RingLayout {
    uint32_t bufferField;   // Channel 0 buffer pointer.
    uint32_t channelStride; // Byte stride between channel buffer pointers.
    uint32_t indexField;
    uint32_t lengthField;
};

constexpr RingLayout kRingLayout[kRingCount] = {
    {0x00, 4, 0x0C, 0x10}, // Pre-delay comb.
    {0x1C, 4, 0x28, 0x2C}, // Early reflection tap (optional).
    {0x34, 8, 0x4C, 0x54}, // Comb 1 (per-channel pointers interleave with comb 2).
    {0x38, 8, 0x50, 0x58}, // Comb 2.
    {0x6C, 8, 0x84, 0x8C}, // Allpass 1 (interleaves with allpass 2).
    {0x70, 8, 0x88, 0x90}, // Allpass 2.
};

struct Frame {
    uint8_t* ring[kRingCount][kChannels]{};
    uint32_t ringIndex[kRingCount]{};
    uint32_t ringLength[kRingCount]{};
    uint8_t* main[kChannels]{};
    const uint8_t* auxIn[kChannels]{};
    uint8_t* auxOut[kChannels]{};
    float lastAllpass[kChannels]{};
    float preDelayCoef = 0.0f;
    float comb1Coef = 0.0f;
    float comb2Coef = 0.0f;
    float allpassCoef = 0.0f;
    float damping = 0.0f;
    float oneMinusDamping = 0.0f;
    float dryScale = 0.0f;
    float wetScale = 0.0f;
    float mainGain = 0.0f;
    float auxGain = 0.0f;
    bool hasEarly = false;
    bool hasAuxIn = false;
    bool hasAuxOut = false;
};

inline float LoadFloat(const uint8_t* host) {
    return BigEndian::ReadFloat32(host);
}

inline void StoreFloat(uint8_t* host, float value) {
    BigEndian::WriteFloat32(host, value);
}

inline int32_t LoadS32(const uint8_t* host) {
    return static_cast<int32_t>(BigEndian::Read32(host));
}

inline void StoreS32(uint8_t* host, int32_t value) {
    BigEndian::Write32(host, static_cast<uint32_t>(value));
}

// PowerPC fctiwz: round toward zero, saturating out-of-range and NaN exactly the
// way runtime/src/fpu_helpers.cpp does for the translated form.
inline int32_t ConvertToIntegerWord(float value) {
    const double wide = static_cast<double>(value);
    if (std::isnan(wide)) {
        return static_cast<int32_t>(0x80000000u);
    }
    if (wide >= 2147483647.0) {
        return 2147483647;
    }
    if (wide <= -2147483648.0) {
        return static_cast<int32_t>(0x80000000u);
    }
    return static_cast<int32_t>(wide);
}

// Guest-thread-only range resolver. Deliberately NOT the mix's MixResolveRange: this
// callback must materialize deferred GX reads through the page table, which the
// worker-safe resolver refuses to do by design.
uint8_t* ResolveGuestThreadRange(uint32_t addr, size_t bytes) {
    if (addr == 0 || bytes == 0) {
        return nullptr;
    }
    if (uint8_t* fast = MemoryInline::GetPointerFast(addr, bytes)) {
        return fast;
    }
    // A ring buffer may straddle the inline page granularity; the region lookup
    // still returns one contiguous host mapping for the whole range.
    try {
        return Memory::GetPointer(addr, bytes);
    } catch (const Memory::AccessViolation&) {
        return nullptr;
    }
}

// Collects everything the render loop needs. Returns false when the layout is
// not one this port can serve bit-exactly, in which case the caller must run the
// translated function instead.
bool BuildFrame(uint32_t buffersAddr, uint32_t stateAddr, Frame& frame) {
    if (buffersAddr == 0 || stateAddr == 0) {
        return false;
    }
    if (!Memory::Contains(stateAddr, kStateStructBytes)) {
        return false;
    }

    const uint32_t auxInputBuffers = Memory::Read32(stateAddr + kFieldAuxInputBuffers);
    const uint32_t auxOutputBuffers = Memory::Read32(stateAddr + kFieldAuxOutputBuffers);
    frame.hasAuxIn = auxInputBuffers != 0;
    frame.hasAuxOut = auxOutputBuffers != 0;

    constexpr size_t kFrameBytes = kSamplesPerFrame * sizeof(int32_t);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.main[channel] =
            ResolveGuestThreadRange(Memory::Read32(buffersAddr + channel * 4), kFrameBytes);
        if (!frame.main[channel]) {
            return false;
        }
        if (frame.hasAuxIn) {
            frame.auxIn[channel] =
                ResolveGuestThreadRange(Memory::Read32(auxInputBuffers + channel * 4), kFrameBytes);
            if (!frame.auxIn[channel]) {
                return false;
            }
        }
        if (frame.hasAuxOut) {
            frame.auxOut[channel] =
                ResolveGuestThreadRange(Memory::Read32(auxOutputBuffers + channel * 4), kFrameBytes);
            if (!frame.auxOut[channel]) {
                return false;
            }
        }
    }

    frame.hasEarly = Memory::Read32(stateAddr + kFieldEarlyLength) != 0;
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        const RingLayout& layout = kRingLayout[ring];
        frame.ringLength[ring] = Memory::Read32(stateAddr + layout.lengthField);
        frame.ringIndex[ring] = Memory::Read32(stateAddr + layout.indexField);
        if (ring == kRingEarly && !frame.hasEarly) {
            continue;
        }
        // The guest maintains index < length; anything else means the struct is
        // not initialized the way this port assumes.
        if (frame.ringLength[ring] == 0 || frame.ringIndex[ring] >= frame.ringLength[ring]) {
            return false;
        }
        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            frame.ring[ring][channel] = ResolveGuestThreadRange(
                Memory::Read32(stateAddr + layout.bufferField + channel * layout.channelStride),
                static_cast<size_t>(frame.ringLength[ring]) * sizeof(float));
            if (!frame.ring[ring][channel]) {
                return false;
            }
        }
    }

    const float sendScale = kSendScale;
    const float one = kOne;
    frame.damping = Memory::ReadFloat32(stateAddr + kFieldDamping);
    frame.oneMinusDamping = one - frame.damping;
    frame.dryScale = sendScale * Memory::ReadFloat32(stateAddr + kFieldDryPreScale);
    frame.wetScale = sendScale * Memory::ReadFloat32(stateAddr + kFieldWetPreScale);
    frame.preDelayCoef = Memory::ReadFloat32(stateAddr + kFieldPreDelayCoef);
    frame.comb1Coef = Memory::ReadFloat32(stateAddr + kFieldComb1Coef);
    frame.comb2Coef = Memory::ReadFloat32(stateAddr + kFieldComb2Coef);
    frame.allpassCoef = Memory::ReadFloat32(stateAddr + kFieldAllpassCoef);
    frame.mainGain = Memory::ReadFloat32(stateAddr + kFieldMainOutGain);
    frame.auxGain = Memory::ReadFloat32(stateAddr + kFieldAuxOutGain);
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        frame.lastAllpass[channel] = Memory::ReadFloat32(stateAddr + kFieldLastAllpass + channel * 4);
    }
    return true;
}

void Render(uint32_t stateAddr, Frame& frame) {
    uint32_t index[kRingCount];
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        index[ring] = frame.ringIndex[ring];
    }

    for (uint32_t sample = 0; sample < kSamplesPerFrame; ++sample) {
        const uint32_t preDelayOffset = index[kRingPreDelay] * 4u;
        const uint32_t earlyOffset = index[kRingEarly] * 4u;
        const uint32_t comb1Offset = index[kRingComb1] * 4u;
        const uint32_t comb2Offset = index[kRingComb2] * 4u;
        const uint32_t allpass1Offset = index[kRingAllpass1] * 4u;
        const uint32_t allpass2Offset = index[kRingAllpass2] * 4u;
        const uint32_t frameOffset = sample * 4u;

        for (uint32_t channel = 0; channel < kChannels; ++channel) {
            uint8_t* const mainSlot = frame.main[channel] + frameOffset;
            int32_t rawInput = LoadS32(mainSlot);
            if (frame.hasAuxIn) {
                rawInput = static_cast<int32_t>(
                    static_cast<uint32_t>(rawInput) +
                    static_cast<uint32_t>(LoadS32(frame.auxIn[channel] + frameOffset)));
            }
            const float input = static_cast<float>(rawInput);

            // Pre-delay comb: the tap that leaves the buffer also feeds the dry
            // (early) send. Every product is its own statement so the host
            // compiler cannot fuse a multiply into the following add.
            uint8_t* const preDelaySlot = frame.ring[kRingPreDelay][channel] + preDelayOffset;
            const float preDelayTap = LoadFloat(preDelaySlot);
            const float preDelayFeedback = preDelayTap * frame.preDelayCoef;
            StoreFloat(preDelaySlot, input + preDelayFeedback);

            float excite = input;
            if (frame.hasEarly) {
                uint8_t* const earlySlot = frame.ring[kRingEarly][channel] + earlyOffset;
                excite = LoadFloat(earlySlot);
                StoreFloat(earlySlot, input);
            }

            const float dry = preDelayTap * frame.dryScale;

            uint8_t* const comb1Slot = frame.ring[kRingComb1][channel] + comb1Offset;
            const float comb1Tap = LoadFloat(comb1Slot);
            const float comb1Feedback = comb1Tap * frame.comb1Coef;
            StoreFloat(comb1Slot, excite + comb1Feedback);

            uint8_t* const comb2Slot = frame.ring[kRingComb2][channel] + comb2Offset;
            const float comb2Tap = LoadFloat(comb2Slot);
            const float comb2Feedback = comb2Tap * frame.comb2Coef;
            const float combSum = comb1Tap + comb2Tap;
            StoreFloat(comb2Slot, excite + comb2Feedback);

            uint8_t* const allpass1Slot = frame.ring[kRingAllpass1][channel] + allpass1Offset;
            const float allpass1Tap = LoadFloat(allpass1Slot);
            const float allpass1Feedback = allpass1Tap * frame.allpassCoef;
            const float allpass1Store = combSum + allpass1Feedback;
            StoreFloat(allpass1Slot, allpass1Store);
            const float allpass1Feedforward = allpass1Store * frame.allpassCoef;
            const float allpass1Out = allpass1Tap - allpass1Feedforward;

            const float dampedNew = frame.oneMinusDamping * allpass1Out;
            const float dampedOld = frame.damping * frame.lastAllpass[channel];
            const float damped = dampedNew + dampedOld;
            frame.lastAllpass[channel] = damped;

            uint8_t* const allpass2Slot = frame.ring[kRingAllpass2][channel] + allpass2Offset;
            const float allpass2Tap = LoadFloat(allpass2Slot);
            const float allpass2Feedback = allpass2Tap * frame.allpassCoef;
            const float allpass2Store = damped + allpass2Feedback;
            StoreFloat(allpass2Slot, allpass2Store);
            const float allpass2Feedforward = allpass2Store * frame.allpassCoef;
            const float allpass2Out = allpass2Tap - allpass2Feedforward;

            const float wet = allpass2Out * frame.wetScale;
            const float mixed = dry + wet;
            const float mainSample = mixed * frame.mainGain;
            StoreS32(mainSlot, ConvertToIntegerWord(mainSample));
            if (frame.hasAuxOut) {
                const float auxSample = mixed * frame.auxGain;
                StoreS32(frame.auxOut[channel] + frameOffset, ConvertToIntegerWord(auxSample));
            }
        }

        for (uint32_t ring = 0; ring < kRingCount; ++ring) {
            if (ring == kRingEarly && !frame.hasEarly) {
                continue;
            }
            const uint32_t next = index[ring] + 1u;
            index[ring] = next < frame.ringLength[ring] ? next : 0u;
        }
    }

    // The guest writes these back every sample; nothing can observe the
    // intermediate values, so one store per field at the end is equivalent.
    for (uint32_t ring = 0; ring < kRingCount; ++ring) {
        if (ring == kRingEarly && !frame.hasEarly) {
            continue;
        }
        Memory::Write32(stateAddr + kRingLayout[ring].indexField, index[ring]);
    }
    for (uint32_t channel = 0; channel < kChannels; ++channel) {
        Memory::WriteFloat32(stateAddr + kFieldLastAllpass + channel * 4,
                             static_cast<double>(frame.lastAllpass[channel]));
    }
}

} // namespace ReverbStd

// AXFX_REVERBHI_EXP, the "Hi" reverb nw4r's FxReverbHi runs every audio frame
// (Bomberman Blast spent a third of its time in it, translated): early
// reflections, an optional pre-delay, three combs, two allpasses shared in
// coefficient, a low-pass, a third allpass per channel, then crosstalk between
// the three channels. Read from the SDK's callback instruction by instruction;
// every product and sum is its own single-precision step, as the guest's are.
namespace ReverbHi {

constexpr uint32_t kSamplesPerFrame = 96;
constexpr uint32_t kChannels = 3;
constexpr float kOne = 1.0f;
constexpr float kWetScale = 0.6f;       // .sdata2: the wet send's pre-scale
constexpr float kCrosstalkScale = 0.5f; // .sdata2: the crosstalk's

// field offsets into the struct in r4
constexpr uint32_t kEarlyLines = 0x00;      // + channel * 4
constexpr uint32_t kEarlyPos = 0x0C;        // three taps, + tap * 4
constexpr uint32_t kEarlyLength = 0x18;
constexpr uint32_t kEarlyCoef = 0x20;       // + tap * 4
constexpr uint32_t kPreDelayLines = 0x2C;   // + channel * 4
constexpr uint32_t kPreDelayPos = 0x38;
constexpr uint32_t kPreDelayLength = 0x3C;  // 0: no pre-delay
constexpr uint32_t kCombLines = 0x44;       // + channel * 12 + comb * 4
constexpr uint32_t kCombPos = 0x68;         // + comb * 4
constexpr uint32_t kCombLength = 0x74;      // + comb * 4
constexpr uint32_t kCombCoef = 0x8C;        // + comb * 4
constexpr uint32_t kAllpassLines = 0x98;    // + channel * 8 + allpass * 4
constexpr uint32_t kAllpassPos = 0xB0;      // + allpass * 4
constexpr uint32_t kAllpassLength = 0xB8;   // + allpass * 4
constexpr uint32_t kLastLines = 0xC8;       // + channel * 4: the third allpass, one per channel
constexpr uint32_t kLastPos = 0xD4;         // + channel * 4
constexpr uint32_t kLastLength = 0xE0;      // + channel * 4
constexpr uint32_t kAllpassCoef = 0xF8;
constexpr uint32_t kLowPassHistory = 0xFC;  // + channel * 4
constexpr uint32_t kLowPassCoef = 0x108;
constexpr uint32_t kFlags = 0x10C;
constexpr uint32_t kCrosstalk = 0x12C;
constexpr uint32_t kWet = 0x134;
constexpr uint32_t kBusIn = 0x138;
constexpr uint32_t kBusOut = 0x13C;
constexpr uint32_t kOutGain = 0x140;
constexpr uint32_t kBusOutGain = 0x144;
constexpr uint32_t kStructBytes = 0x148;

struct Line {
    uint8_t* data = nullptr;
    uint32_t pos = 0;
    uint32_t length = 0;
};

struct Frame {
    uint8_t* main[kChannels]{};
    const uint8_t* busIn[kChannels]{};
    uint8_t* busOut[kChannels]{};
    uint8_t* early[kChannels]{};
    uint32_t earlyPos[3]{};
    uint32_t earlyLength = 0;
    float earlyCoef[3]{};
    uint8_t* preDelay[kChannels]{};
    uint32_t preDelayPos = 0, preDelayLength = 0;
    uint8_t* comb[kChannels][3]{};
    uint32_t combPos[3]{}, combLength[3]{};
    float combCoef[3]{};
    uint8_t* allpass[kChannels][2]{};
    uint32_t allpassPos[2]{}, allpassLength[2]{};
    Line last[kChannels];
    float allpassCoef = 0.0f;
    float lowPass[kChannels]{};
    float lowPassCoef = 0.0f;
    float crosstalk = 0.0f, wet = 0.0f, outGain = 0.0f, busOutGain = 0.0f;
};

// A line of `length` floats at the guest address in `field`, its position below its length.
uint8_t* LineAt(uint32_t field, uint32_t length) {
    return length == 0 ? nullptr
                       : ReverbStd::ResolveGuestThreadRange(Memory::Read32(field), static_cast<size_t>(length) * sizeof(float));
}

bool BuildFrame(uint32_t buffersAddr, uint32_t rev, Frame& f) {
    if (buffersAddr == 0 || rev == 0 || !Memory::Contains(rev, kStructBytes)) {
        return false;
    }
    constexpr size_t kFrameBytes = kSamplesPerFrame * sizeof(int32_t);
    const uint32_t busIn = Memory::Read32(rev + kBusIn), busOut = Memory::Read32(rev + kBusOut);
    for (uint32_t ch = 0; ch < kChannels; ++ch) {
        if (!(f.main[ch] = ReverbStd::ResolveGuestThreadRange(Memory::Read32(buffersAddr + ch * 4), kFrameBytes))) {
            return false;
        }
        if (busIn && !(f.busIn[ch] = ReverbStd::ResolveGuestThreadRange(Memory::Read32(busIn + ch * 4), kFrameBytes))) {
            return false;
        }
        if (busOut && !(f.busOut[ch] = ReverbStd::ResolveGuestThreadRange(Memory::Read32(busOut + ch * 4), kFrameBytes))) {
            return false;
        }
    }
    f.earlyLength = Memory::Read32(rev + kEarlyLength);
    f.preDelayLength = Memory::Read32(rev + kPreDelayLength);
    f.preDelayPos = Memory::Read32(rev + kPreDelayPos);
    if (f.preDelayLength != 0 && f.preDelayPos >= f.preDelayLength) {
        return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
        f.earlyPos[i] = Memory::Read32(rev + kEarlyPos + i * 4);
        f.earlyCoef[i] = Memory::ReadFloat32(rev + kEarlyCoef + i * 4);
        f.combPos[i] = Memory::Read32(rev + kCombPos + i * 4);
        f.combLength[i] = Memory::Read32(rev + kCombLength + i * 4);
        f.combCoef[i] = Memory::ReadFloat32(rev + kCombCoef + i * 4);
        if (f.earlyPos[i] >= f.earlyLength || f.combPos[i] >= f.combLength[i]) {
            return false;
        }
    }
    for (uint32_t i = 0; i < 2; ++i) {
        f.allpassPos[i] = Memory::Read32(rev + kAllpassPos + i * 4);
        f.allpassLength[i] = Memory::Read32(rev + kAllpassLength + i * 4);
        if (f.allpassPos[i] >= f.allpassLength[i]) {
            return false;
        }
    }
    for (uint32_t ch = 0; ch < kChannels; ++ch) {
        if (!(f.early[ch] = LineAt(rev + kEarlyLines + ch * 4, f.earlyLength))) {
            return false;
        }
        if (f.preDelayLength != 0 &&
            !(f.preDelay[ch] = LineAt(rev + kPreDelayLines + ch * 4, f.preDelayLength))) {
            return false;
        }
        for (uint32_t i = 0; i < 3; ++i) {
            if (!(f.comb[ch][i] = LineAt(rev + kCombLines + ch * 12 + i * 4, f.combLength[i]))) {
                return false;
            }
        }
        for (uint32_t i = 0; i < 2; ++i) {
            if (!(f.allpass[ch][i] = LineAt(rev + kAllpassLines + ch * 8 + i * 4, f.allpassLength[i]))) {
                return false;
            }
        }
        Line& last = f.last[ch];
        last.length = Memory::Read32(rev + kLastLength + ch * 4);
        last.pos = Memory::Read32(rev + kLastPos + ch * 4);
        if (last.pos >= last.length || !(last.data = LineAt(rev + kLastLines + ch * 4, last.length))) {
            return false;
        }
        f.lowPass[ch] = Memory::ReadFloat32(rev + kLowPassHistory + ch * 4);
    }
    f.allpassCoef = Memory::ReadFloat32(rev + kAllpassCoef);
    f.lowPassCoef = Memory::ReadFloat32(rev + kLowPassCoef);
    f.crosstalk = Memory::ReadFloat32(rev + kCrosstalk);
    f.wet = Memory::ReadFloat32(rev + kWet);
    f.outGain = Memory::ReadFloat32(rev + kOutGain);
    f.busOutGain = Memory::ReadFloat32(rev + kBusOutGain);
    return true;
}

// One delay line tap: read the slot `pos` floats in, write `value` there, return the read.
inline float Swap(uint8_t* line, uint32_t pos, float value) {
    uint8_t* const slot = line + pos * 4u;
    const float tap = ReverbStd::LoadFloat(slot);
    ReverbStd::StoreFloat(slot, value);
    return tap;
}

inline void Advance(uint32_t& pos, uint32_t length) {
    pos = pos + 1u < length ? pos + 1u : 0u;
}

void Render(uint32_t rev, Frame& f) {
    const float oneMinusLowPass = kOne - f.lowPassCoef;
    const float wet = kWetScale * f.wet;
    const float crosstalk = kCrosstalkScale * f.crosstalk;
    const float ap = f.allpassCoef;
    for (uint32_t sample = 0; sample < kSamplesPerFrame; ++sample) {
        const uint32_t at = sample * 4u;
        float out[kChannels];
        for (uint32_t ch = 0; ch < kChannels; ++ch) {
            int32_t raw = ReverbStd::LoadS32(f.main[ch] + at);
            if (f.busIn[ch]) {
                raw = static_cast<int32_t>(static_cast<uint32_t>(ReverbStd::LoadS32(f.busIn[ch] + at)) +
                                           static_cast<uint32_t>(raw));
            }
            const float in = static_cast<float>(raw);

            // early reflections: three taps, the input written at the third
            const float e0 = ReverbStd::LoadFloat(f.early[ch] + f.earlyPos[0] * 4u) * f.earlyCoef[0];
            const float e1 = ReverbStd::LoadFloat(f.early[ch] + f.earlyPos[1] * 4u) * f.earlyCoef[1];
            const float t2 = Swap(f.early[ch], f.earlyPos[2], in);
            const float e2 = f.earlyCoef[2] * t2;
            const float e01 = e0 + e1;
            const float early = e2 + e01;

            const float x = f.preDelayLength != 0 ? Swap(f.preDelay[ch], f.preDelayPos, in) : in;

            float acc = 0.0f;
            for (uint32_t i = 0; i < 3; ++i) {
                uint8_t* const slot = f.comb[ch][i] + f.combPos[i] * 4u;
                const float tap = ReverbStd::LoadFloat(slot);
                const float feedback = tap * f.combCoef[i];
                acc = acc + tap;
                ReverbStd::StoreFloat(slot, x + feedback);
            }

            uint8_t* const a1 = f.allpass[ch][0] + f.allpassPos[0] * 4u;
            const float t1 = ReverbStd::LoadFloat(a1);
            const float s1 = acc + t1 * ap;
            ReverbStd::StoreFloat(a1, s1);
            const float o1 = t1 - s1 * ap;

            uint8_t* const a2 = f.allpass[ch][1] + f.allpassPos[1] * 4u;
            const float t2b = ReverbStd::LoadFloat(a2);
            const float s2 = o1 + t2b * ap;
            ReverbStd::StoreFloat(a2, s2);
            const float s2ap = s2 * ap;
            const float o2 = t2b - s2ap;

            const float held = f.lowPassCoef * f.lowPass[ch];
            const float fresh = oneMinusLowPass * o2;
            const float y = fresh + held;
            f.lowPass[ch] = y;

            Line& last = f.last[ch];
            uint8_t* const a3 = last.data + last.pos * 4u;
            const float t3 = ReverbStd::LoadFloat(a3);
            const float s3 = y + t3 * ap;
            ReverbStd::StoreFloat(a3, s3);
            const float s3ap = s3 * ap;
            const float o3 = t3 - s3ap;
            Advance(last.pos, last.length);

            const float wetOut = o3 * wet;
            out[ch] = wetOut + early;
        }

        const float l = out[0], r = out[1], s = out[2];
        const float rs = r + s, ls = l + s, lr = l + r;
        const float rsx = rs * crosstalk, lsx = ls * crosstalk, lrx = lr * crosstalk;
        const float mixed[kChannels] = {l + rsx, r + lsx, s + lrx};
        for (uint32_t ch = 0; ch < kChannels; ++ch) {
            const float scaled = mixed[ch] * f.outGain;
            ReverbStd::StoreS32(f.main[ch] + at, ReverbStd::ConvertToIntegerWord(scaled));
            if (f.busOut[ch]) {
                const float bus = mixed[ch] * f.busOutGain;
                ReverbStd::StoreS32(f.busOut[ch] + at, ReverbStd::ConvertToIntegerWord(bus));
            }
        }

        for (uint32_t i = 0; i < 3; ++i) {
            Advance(f.earlyPos[i], f.earlyLength);
        }
        if (f.preDelayLength != 0) {
            Advance(f.preDelayPos, f.preDelayLength);
        }
        for (uint32_t i = 0; i < 3; ++i) {
            Advance(f.combPos[i], f.combLength[i]);
        }
        for (uint32_t i = 0; i < 2; ++i) {
            Advance(f.allpassPos[i], f.allpassLength[i]);
        }
    }

    for (uint32_t i = 0; i < 3; ++i) {
        Memory::Write32(rev + kEarlyPos + i * 4, f.earlyPos[i]);
        Memory::Write32(rev + kCombPos + i * 4, f.combPos[i]);
    }
    if (f.preDelayLength != 0) {
        Memory::Write32(rev + kPreDelayPos, f.preDelayPos);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        Memory::Write32(rev + kAllpassPos + i * 4, f.allpassPos[i]);
    }
    for (uint32_t ch = 0; ch < kChannels; ++ch) {
        Memory::Write32(rev + kLastPos + ch * 4, f.last[ch].pos);
        Memory::WriteFloat32(rev + kLowPassHistory + ch * 4, static_cast<double>(f.lowPass[ch]));
    }
}

} // namespace ReverbHi
} // namespace

extern "C" void AXFXReverbStdExpCallback_8012b830(CpuContext* ctx) {
    if (!ctx) {
        return;
    }

    const uint32_t buffersAddr = ctx->gpr[3];
    const uint32_t stateAddr = ctx->gpr[4];

    uint32_t flags = 0;
    try {
        flags = Memory::Read32(stateAddr + ReverbStd::kFieldFlags);
    } catch (const Memory::AccessViolation&) {
        WIINX_GUEST_HELPER(func_8012B830, ctx);
        return;
    }
    if (flags != 0) {
        // Reset request: the guest clears the "in progress" bit and skips the
        // frame entirely.
        Memory::Write32(stateAddr + ReverbStd::kFieldFlags, flags & ~2u);
        return;
    }

    ReverbStd::Frame frame;
    bool built = false;
    try {
        built = ReverbStd::BuildFrame(buffersAddr, stateAddr, frame);
    } catch (const Memory::AccessViolation&) {
        built = false;
    }
    if (!built) {
        WIINX_GUEST_HELPER(func_8012B830, ctx);
        return;
    }

    ReverbStd::Render(stateAddr, frame);
}

REGISTER_NATIVE_FUNCTION_AS(0x8012B830, AXFXReverbStdExpCallback_8012b830,
                            "AXFXReverbStdExpCallback_8012b830");

// AXFXReverbHiCallback(AXFX_BUFFERUPDATE* buffers, AXFX_REVERBHI_EXP* reverb),
// read from Bomberman Blast (0x801BD694). Its translated form cannot run in
// its place (natives replace it before translation), so a reverb this port
// cannot serve leaves the frame dry, once said.
extern "C" void AXFXReverbHiCallback_801bd694(CpuContext* ctx) {
    if (!ctx) {
        return;
    }
    const uint32_t buffersAddr = ctx->gpr[3];
    const uint32_t rev = ctx->gpr[4];
    try {
        const uint32_t flags = Memory::Read32(rev + ReverbHi::kFlags);
        if (flags != 0) {
            Memory::Write32(rev + ReverbHi::kFlags, flags & ~2u);  // a reset: skip this frame
            return;
        }
        ReverbHi::Frame frame;
        if (ReverbHi::BuildFrame(buffersAddr, rev, frame)) {
            ReverbHi::Render(rev, frame);
            return;
        }
    } catch (const Memory::AccessViolation&) {
    }
    static bool said = false;
    if (!said) {
        said = true;
        RT_LOG(RT_TAG_OS) << "AXFXReverbHiCallback: reverb state at 0x" << std::hex << rev << std::dec
                          << " is not one this port serves; frames left dry" << std::endl;
    }
}

REGISTER_NATIVE_FUNCTION_AS(0x801BD694, AXFXReverbHiCallback_801bd694, "AXFXReverbHiCallback_801bd694");
