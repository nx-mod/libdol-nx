// GX's state, as the SDK's setters change it (wiinx/format/gx/state.hpp).
#include "wiinx/format/gx/state.hpp"

#include <cstring>

namespace wiinx::gx {
namespace state_detail {

constexpr std::uint8_t kChanZero = 6;   // GX_COLOR_ZERO
constexpr std::uint32_t kTexDisable = 0x100;  // GX_TEX_DISABLE
constexpr std::uint32_t kTexMtx0 = 30, kPtTexMtx0 = 64, kIdentityMtx = 60, kPtIdentity = 125;
// GX_VA_*
constexpr std::uint8_t kVaPnMtxIdx = 0, kVaNrm = 10, kVaNbt = 25, kVaClr0 = 11, kVaClr1 = 12, kVaTex0 = 13;

TevChannel ChannelOf(std::uint8_t id) {
    switch (id) {
    case 0: case 2: case 4: return TevChannel::Color0;
    case 1: case 3: case 5: return TevChannel::Color1;
    case 0xFF: return TevChannel::Null;
    default: return TevChannel::Zero;  // GX_COLOR_ZERO, and the bump alphas (not yet)
    }
}

void Put(std::vector<std::uint8_t>& out, std::size_t offset, const float* v, std::size_t count) {
    std::memcpy(out.data() + offset, v, count * sizeof(float));
}

void PutRows(std::vector<std::uint8_t>& out, std::size_t offset, const std::array<float, 4>* rows, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        Put(out, offset + i * 16, rows[i].data(), 4);
    }
}

void PutColor(std::vector<std::uint8_t>& out, std::size_t offset, Color8 c) {
    const float v[4] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
    Put(out, offset, v, 4);
}

}  // namespace state_detail

using namespace state_detail;

State::State() {
    // GXInit: every stage textured from its own coordinate and map, lit by
    // channel 0, replacing; one stage, one texgen, no channels
    for (std::uint8_t i = 0; i < 16; ++i) {
        SetTevOrder(i, i < 8 ? i : 0xFF, i < 8 ? i : 0xFF, 4);
        SetTevOp(i, TevMode::Replace);
    }
    SetNumTevStages(1);
    mTev.swapTable[0] = {SwapChannel::Red, SwapChannel::Green, SwapChannel::Blue, SwapChannel::Alpha};
    mTev.swapTable[1] = {SwapChannel::Red, SwapChannel::Red, SwapChannel::Red, SwapChannel::Alpha};
    mTev.swapTable[2] = {SwapChannel::Green, SwapChannel::Green, SwapChannel::Green, SwapChannel::Alpha};
    mTev.swapTable[3] = {SwapChannel::Blue, SwapChannel::Blue, SwapChannel::Blue, SwapChannel::Alpha};
    for (std::uint8_t i = 0; i < 8; ++i) {
        SetTexCoordGen2(i, static_cast<std::uint8_t>(TexGenType::Mtx2x4), static_cast<std::uint8_t>(4 + i), kIdentityMtx,
                        false, kPtIdentity);
    }
    SetNumTexGens(1);
    SetNumChans(0);
    for (std::uint8_t c = 0; c < 4; ++c) {
        SetChanCtrl(c, false, 0, 1, 0, 0, 2);
    }
    for (int r = 0; r < 3; ++r) {
        mPosRows[r][r] = 1.0f;
        mNrmRows[r][r] = 1.0f;
    }
    for (int r = 0; r < 4; ++r) {
        mProj[r][r] = 1.0f;
    }
    mPixel = PixelState{};
    mPixel.alphaWrite = true;
}

// --- TEV

void State::SetNumTevStages(std::uint8_t count) { mTev.stageCount = count < 1 ? 1 : count > 16 ? 16 : count; }

void State::SetTevOrder(std::uint8_t stage, std::uint8_t coord, std::uint32_t map, std::uint8_t channel) {
    if (stage >= 16) {
        return;
    }
    TevStage& s = mTev.stages[stage];
    s.texCoord = coord;
    s.texMap = (map & kTexDisable) || (map & 0xFF) >= 8 ? kTexNull : static_cast<std::uint8_t>(map & 0xFF);
    s.channel = ChannelOf(channel);
}

void State::SetTevColorIn(std::uint8_t stage, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    if (stage < 16) {
        TevCombiner& k = mTev.stages[stage].color;
        k.a = a, k.b = b, k.c = c, k.d = d;
    }
}

void State::SetTevAlphaIn(std::uint8_t stage, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    if (stage < 16) {
        TevCombiner& k = mTev.stages[stage].alpha;
        k.a = a, k.b = b, k.c = c, k.d = d;
    }
}

namespace {
void SetOp(TevCombiner& k, std::uint8_t op, std::uint8_t bias, std::uint8_t scale, bool clamp, std::uint8_t outReg) {
    k.op = static_cast<TevOp>(op);
    k.bias = static_cast<TevBias>(bias);
    k.scale = static_cast<TevScale>(scale);
    k.clamp = clamp;
    k.out = static_cast<TevReg>(outReg & 3);
}
}  // namespace

void State::SetTevColorOp(std::uint8_t stage, std::uint8_t op, std::uint8_t bias, std::uint8_t scale, bool clamp,
                          std::uint8_t outReg) {
    if (stage < 16) {
        SetOp(mTev.stages[stage].color, op, bias, scale, clamp, outReg);
    }
}

void State::SetTevAlphaOp(std::uint8_t stage, std::uint8_t op, std::uint8_t bias, std::uint8_t scale, bool clamp,
                          std::uint8_t outReg) {
    if (stage < 16) {
        SetOp(mTev.stages[stage].alpha, op, bias, scale, clamp, outReg);
    }
}

// The SDK's presets: the rasterised colour on stage 0, the previous stage's
// result after it.
void State::SetTevOp(std::uint8_t stage, TevMode mode) {
    using C = TevColorArg;
    using A = TevAlphaArg;
    const auto c = [](C v) { return static_cast<std::uint8_t>(v); };
    const auto a = [](A v) { return static_cast<std::uint8_t>(v); };
    const std::uint8_t carg = stage == 0 ? c(C::RasC) : c(C::CPrev);
    const std::uint8_t aarg = stage == 0 ? a(A::RasA) : a(A::APrev);
    switch (mode) {
    case TevMode::Modulate:
        SetTevColorIn(stage, c(C::Zero), c(C::TexC), carg, c(C::Zero));
        SetTevAlphaIn(stage, a(A::Zero), a(A::TexA), aarg, a(A::Zero));
        break;
    case TevMode::Decal:
        SetTevColorIn(stage, carg, c(C::TexC), c(C::TexA), c(C::Zero));
        SetTevAlphaIn(stage, a(A::Zero), a(A::Zero), a(A::Zero), aarg);
        break;
    case TevMode::Blend:
        SetTevColorIn(stage, carg, c(C::One), c(C::TexC), c(C::Zero));
        SetTevAlphaIn(stage, a(A::Zero), a(A::TexA), aarg, a(A::Zero));
        break;
    case TevMode::Replace:
        SetTevColorIn(stage, c(C::Zero), c(C::Zero), c(C::Zero), c(C::TexC));
        SetTevAlphaIn(stage, a(A::Zero), a(A::Zero), a(A::Zero), a(A::TexA));
        break;
    case TevMode::PassClr:
        SetTevColorIn(stage, c(C::Zero), c(C::Zero), c(C::Zero), carg);
        SetTevAlphaIn(stage, a(A::Zero), a(A::Zero), a(A::Zero), aarg);
        break;
    }
    SetTevColorOp(stage, 0, 0, 0, true, 0);
    SetTevAlphaOp(stage, 0, 0, 0, true, 0);
}

void State::SetTevKColorSel(std::uint8_t stage, std::uint8_t sel) {
    if (stage < 16) {
        mTev.stages[stage].kcolorSel = sel;
    }
}

void State::SetTevKAlphaSel(std::uint8_t stage, std::uint8_t sel) {
    if (stage < 16) {
        mTev.stages[stage].kalphaSel = sel;
    }
}

void State::SetTevSwapMode(std::uint8_t stage, std::uint8_t rasSel, std::uint8_t texSel) {
    if (stage < 16) {
        mTev.stages[stage].rasSwap = rasSel & 3;
        mTev.stages[stage].texSwap = texSel & 3;
    }
}

void State::SetTevSwapModeTable(std::uint8_t table, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    mTev.swapTable[table & 3] = {static_cast<SwapChannel>(r & 3), static_cast<SwapChannel>(g & 3),
                                 static_cast<SwapChannel>(b & 3), static_cast<SwapChannel>(a & 3)};
}

void State::SetAlphaCompare(std::uint8_t comp0, std::uint8_t ref0, std::uint8_t op, std::uint8_t comp1,
                            std::uint8_t ref1) {
    mTev.alphaComp0 = static_cast<Compare>(comp0 & 7);
    mTev.alphaRef0 = ref0;
    mTev.alphaOp = static_cast<AlphaOp>(op & 3);
    mTev.alphaComp1 = static_cast<Compare>(comp1 & 7);
    mTev.alphaRef1 = ref1;
}

void State::SetTevColor(std::uint8_t reg, Color8 color) {
    mTevRegs[reg & 3] = {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
}

void State::SetTevColorS10(std::uint8_t reg, const std::int16_t rgba[4]) {
    for (int i = 0; i < 4; ++i) {
        mTevRegs[reg & 3][i] = rgba[i] / 255.0f;
    }
}

void State::SetTevKColor(std::uint8_t id, Color8 color) {
    mKColors[id & 3] = {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
}

// --- channels, lights, texgens

void State::SetNumChans(std::uint8_t count) { mVertex.channelCount = count > 2 ? 2 : count; }

void State::SetChanCtrl(std::uint8_t channel, bool enable, std::uint8_t ambSrc, std::uint8_t matSrc,
                        std::uint32_t lightMask, std::uint8_t diffFn, std::uint8_t attnFn) {
    ColorChannel cc;
    cc.lighting = enable;
    cc.ambient = static_cast<ColorSrc>(ambSrc & 1);
    cc.material = static_cast<ColorSrc>(matSrc & 1);
    cc.lightMask = static_cast<std::uint8_t>(lightMask);
    cc.diffuse = static_cast<DiffuseFn>(diffFn > 2 ? 0 : diffFn);
    cc.attenuation = static_cast<AttnFn>(attnFn > 2 ? 2 : attnFn);
    // GX_COLOR0A0 and GX_COLOR1A1 set the colour and the alpha
    if (channel == 4 || channel == 5) {
        mVertex.channels[channel - 4] = cc;
        mVertex.channels[channel - 2] = cc;
    } else if (channel < 4) {
        mVertex.channels[channel] = cc;
    }
}

void State::SetChanAmbColor(std::uint8_t channel, Color8 color) {
    // (a colour channel sets RGB, an alpha channel A, a pair all four)
    Color8& c = mChanAmb[channel & 1];
    if (channel == 0 || channel == 1 || channel >= 4) c.r = color.r, c.g = color.g, c.b = color.b;
    if (channel >= 2) c.a = color.a;
}

void State::SetChanMatColor(std::uint8_t channel, Color8 color) {
    Color8& c = mChanMat[channel & 1];
    if (channel == 0 || channel == 1 || channel >= 4) c.r = color.r, c.g = color.g, c.b = color.b;
    if (channel >= 2) c.a = color.a;
}

void State::LoadLight(std::uint32_t lightMask, const Light& light) {
    for (int i = 0; i < 8; ++i) {
        if (lightMask & (1u << i)) {
            mLights[i] = light;
            return;
        }
    }
}

void State::SetNumTexGens(std::uint8_t count) { mVertex.texGenCount = count > 8 ? 8 : count; }

void State::SetTexCoordGen2(std::uint8_t dst, std::uint8_t type, std::uint8_t src, std::uint32_t mtx, bool normalize,
                            std::uint32_t postMtx) {
    if (dst >= 8) {
        return;
    }
    TexGen& tg = mVertex.texGens[dst];
    tg.type = static_cast<TexGenType>(type);
    tg.source = src;
    tg.matrix = mtx >= kTexMtx0 && mtx < kTexMtx0 + 30 ? static_cast<std::uint8_t>((mtx - kTexMtx0) / 3) : kIdentity;
    tg.normalize = normalize;
    tg.postMatrix = postMtx >= kPtTexMtx0 && postMtx < kPtTexMtx0 + 60 ? static_cast<std::uint8_t>((postMtx - kPtTexMtx0) / 3)
                                                                        : kIdentity;
}

// --- matrices

void State::LoadPosMtx(const float mtx[3][4], std::uint32_t id) {
    if (id + 3 > mPosRows.size()) {
        return;
    }
    for (int r = 0; r < 3; ++r) {
        mPosRows[id + r] = {mtx[r][0], mtx[r][1], mtx[r][2], mtx[r][3]};
    }
}

void State::LoadNrmMtx(const float mtx[3][3], std::uint32_t id) {
    if (id + 3 > mNrmRows.size()) {
        return;
    }
    for (int r = 0; r < 3; ++r) {
        mNrmRows[id + r] = {mtx[r][0], mtx[r][1], mtx[r][2], 0.0f};
    }
}

void State::LoadTexMtx(const float mtx[3][4], std::uint32_t id, bool twoRows) {
    const int rows = twoRows ? 2 : 3;
    std::array<float, 4>* target = nullptr;
    if (id >= kTexMtx0 && id + 3 <= kTexMtx0 + 30) {
        target = &mTexRows[id - kTexMtx0];
    } else if (id >= kPtTexMtx0 && id + 3 <= kPtTexMtx0 + 60) {
        target = &mPostRows[id - kPtTexMtx0];
    } else {
        return;
    }
    for (int r = 0; r < rows; ++r) {
        target[r] = {mtx[r][0], mtx[r][1], mtx[r][2], mtx[r][3]};
    }
}

void State::SetCurrentMtx(std::uint32_t id) {
    if (id + 3 <= mPosRows.size()) {
        mCurrentPosRow = id;
    }
}

void State::SetProjection(const float mtx[4][4]) {
    for (int r = 0; r < 4; ++r) {
        mProj[r] = {mtx[r][0], mtx[r][1], mtx[r][2], mtx[r][3]};
    }
}

// --- the pixel engine

void State::SetBlendMode(std::uint8_t type, std::uint8_t src, std::uint8_t dst, std::uint8_t logicOp) {
    mPixel.blendMode = static_cast<BlendMode>(type & 3);
    mPixel.srcFactor = static_cast<BlendFactor>(src & 7);
    mPixel.dstFactor = static_cast<BlendFactor>(dst & 7);
    mPixel.logicOp = static_cast<LogicOp>(logicOp & 15);
}

void State::SetZMode(bool enable, std::uint8_t func, bool update) {
    mPixel.depthTest = enable;
    mPixel.depthCompare = func & 7;
    mPixel.depthWrite = update;
}

void State::SetCullMode(std::uint8_t mode) { mPixel.cull = static_cast<CullMode>(mode & 3); }
void State::SetColorUpdate(bool update) { mPixel.colorWrite = update; }
void State::SetAlphaUpdate(bool update) { mPixel.alphaWrite = update; }

// --- vertices

void State::ClearVtxDesc() { mVtxDesc.fill(0); }

void State::SetVtxDesc(std::uint8_t attr, std::uint8_t type) {
    if (attr == kVaNbt) {
        attr = kVaNrm;  // (normal, binormal, tangent: the normal is what lights)
    }
    if (attr < mVtxDesc.size()) {
        mVtxDesc[attr] = type;
    }
}

void State::SetDualTexture(bool enable) { mVertex.dualTexture = enable; }

// --- the hardware's registers
//
// The layouts are Aurora's command processor's (lib/gx/command_processor.cpp,
// MIT), after Dolphin's BPMemory and XFMemory.

namespace {
std::uint32_t Bits(std::uint32_t v, unsigned size, unsigned shift) { return (v >> shift) & ((1u << size) - 1u); }

std::int16_t Signed11(std::uint32_t v) {
    return static_cast<std::int16_t>((v & 0x400) ? static_cast<std::int32_t>(v | ~0x7FFu) : static_cast<std::int32_t>(v));
}

// a TEV stage's rasterised channel, from the hardware's number to the SDK's
std::uint8_t ChannelFromHardware(std::uint32_t hw) {
    static constexpr std::uint8_t kSdk[8] = {4, 5, 4, 5, 4, 7, 8, 6};  // COLOR0A0, COLOR1A1, ..., BUMP, BUMPN, ZERO
    return hw < 8 ? kSdk[hw] : 0xFF;
}

Color8 ColorFromXF(std::uint32_t v) {
    return {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8),
            static_cast<std::uint8_t>(v)};
}

float AsFloat(std::uint32_t v) {
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

// an XF source row (0x40.. bits 7-11) as the SDK's GX_TG_* source
std::uint8_t TexGenSourceFromRow(std::uint32_t row) {
    static constexpr std::uint8_t kSdk[13] = {0, 1, 19, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    return row < 13 ? kSdk[row] : 4;
}

// an XF matrix-index field (a matrix memory row) as a texture matrix slot
std::uint8_t TexMtxSlotFromRow(std::uint32_t row) {
    return row >= kTexMtx0 && row < kTexMtx0 + 30 ? static_cast<std::uint8_t>((row - kTexMtx0) / 3) : kIdentity;
}
}  // namespace

void State::ApplyBP(std::uint32_t value) {
    const std::uint32_t reg = value >> 24;
    if (reg == 0xFE) {
        mBpMask = value & 0x00FFFFFF;  // the next write's mask
        return;
    }
    const std::uint32_t merged = (mBpRegs[reg] & ~mBpMask) | (value & mBpMask & 0x00FFFFFF);
    mBpMask = 0x00FFFFFF;
    mBpRegs[reg] = merged;
    const std::uint32_t v = merged;

    // TEV combiners: colour at the even registers 0xC0..0xDE, alpha at the odd
    if (reg >= 0xC0 && reg <= 0xDF) {
        TevStage& s = mTev.stages[(reg - 0xC0) / 2];
        const bool alpha = (reg & 1) != 0;
        TevCombiner& k = alpha ? s.alpha : s.color;
        if (alpha) {
            s.rasSwap = static_cast<std::uint8_t>(Bits(v, 2, 0));
            s.texSwap = static_cast<std::uint8_t>(Bits(v, 2, 2));
            k.d = static_cast<std::uint8_t>(Bits(v, 3, 4));
            k.c = static_cast<std::uint8_t>(Bits(v, 3, 7));
            k.b = static_cast<std::uint8_t>(Bits(v, 3, 10));
            k.a = static_cast<std::uint8_t>(Bits(v, 3, 13));
        } else {
            k.d = static_cast<std::uint8_t>(Bits(v, 4, 0));
            k.c = static_cast<std::uint8_t>(Bits(v, 4, 4));
            k.b = static_cast<std::uint8_t>(Bits(v, 4, 8));
            k.a = static_cast<std::uint8_t>(Bits(v, 4, 12));
        }
        k.clamp = Bits(v, 1, 19) != 0;
        k.out = static_cast<TevReg>(Bits(v, 2, 22));
        if (Bits(v, 2, 16) == 3) {
            // a bias of 3 is a compare: the op from bit 18 and the scale bits
            k.op = static_cast<TevOp>(8 + (Bits(v, 1, 18) | Bits(v, 2, 20) << 1));
            k.bias = TevBias::Zero;
            k.scale = TevScale::One;
        } else {
            k.op = static_cast<TevOp>(Bits(v, 1, 18));
            k.bias = static_cast<TevBias>(Bits(v, 2, 16));
            k.scale = static_cast<TevScale>(Bits(v, 2, 20));
        }
        return;
    }
    // TEV orders, two stages a register
    if (reg >= 0x28 && reg <= 0x2F) {
        for (unsigned half = 0; half < 2; ++half) {
            const unsigned shift = half * 12;
            const std::uint8_t stage = static_cast<std::uint8_t>((reg - 0x28) * 2 + half);
            const bool enabled = Bits(v, 1, shift + 6) != 0;
            SetTevOrder(stage, static_cast<std::uint8_t>(Bits(v, 3, shift + 3)),
                        enabled ? Bits(v, 3, shift) : 0xFF, ChannelFromHardware(Bits(v, 3, shift + 7)));
        }
        return;
    }
    // konst selections, two stages a register, and the swap tables
    if (reg >= 0xF6 && reg <= 0xFD) {
        const unsigned index = reg - 0xF6;
        TevSwap& swap = mTev.swapTable[index / 2];
        if (index & 1) {
            swap.b = static_cast<SwapChannel>(Bits(v, 2, 0));
            swap.a = static_cast<SwapChannel>(Bits(v, 2, 2));
        } else {
            swap.r = static_cast<SwapChannel>(Bits(v, 2, 0));
            swap.g = static_cast<SwapChannel>(Bits(v, 2, 2));
        }
        mTev.stages[index * 2].kcolorSel = static_cast<std::uint8_t>(Bits(v, 5, 4));
        mTev.stages[index * 2].kalphaSel = static_cast<std::uint8_t>(Bits(v, 5, 9));
        mTev.stages[index * 2 + 1].kcolorSel = static_cast<std::uint8_t>(Bits(v, 5, 14));
        mTev.stages[index * 2 + 1].kalphaSel = static_cast<std::uint8_t>(Bits(v, 5, 19));
        return;
    }
    // TEV and konst colour registers: even RA, odd BG; bit 23 picks konst
    if (reg >= 0xE0 && reg <= 0xE7) {
        const unsigned index = (reg - 0xE0) / 2;
        const bool ra = (reg & 1) == 0;
        if (Bits(v, 1, 23)) {
            auto& k = mKColors[index];
            k[ra ? 0 : 2] = Bits(v, 8, 0) / 255.0f;
            k[ra ? 3 : 1] = Bits(v, 8, 12) / 255.0f;
        } else {
            auto& r = mTevRegs[index];
            r[ra ? 0 : 2] = Signed11(Bits(v, 11, 0)) / 255.0f;
            r[ra ? 3 : 1] = Signed11(Bits(v, 11, 12)) / 255.0f;
        }
        return;
    }
    switch (reg) {
    case 0x00: {  // gen mode
        SetNumTexGens(static_cast<std::uint8_t>(Bits(v, 4, 0)));
        SetNumChans(static_cast<std::uint8_t>(Bits(v, 3, 4)));
        SetNumTevStages(static_cast<std::uint8_t>(Bits(v, 4, 10) + 1));
        // (the hardware's front and back are the SDK's back and front)
        const std::uint32_t cull = Bits(v, 2, 14);
        SetCullMode(static_cast<std::uint8_t>(cull == 1 ? 2 : cull == 2 ? 1 : cull));
        break;
    }
    case 0x40:  // z mode
        SetZMode(Bits(v, 1, 0) != 0, static_cast<std::uint8_t>(Bits(v, 3, 1)), Bits(v, 1, 4) != 0);
        break;
    case 0x41: {  // blend mode
        const std::uint8_t type = Bits(v, 1, 11) ? 3 : Bits(v, 1, 0) ? 1 : Bits(v, 1, 1) ? 2 : 0;
        SetBlendMode(type, static_cast<std::uint8_t>(Bits(v, 3, 8)), static_cast<std::uint8_t>(Bits(v, 3, 5)),
                     static_cast<std::uint8_t>(Bits(v, 4, 12)));
        SetColorUpdate(Bits(v, 1, 3) != 0);
        SetAlphaUpdate(Bits(v, 1, 4) != 0);
        break;
    }
    case 0xF3:  // alpha compare
        SetAlphaCompare(static_cast<std::uint8_t>(Bits(v, 3, 16)), static_cast<std::uint8_t>(Bits(v, 8, 0)),
                        static_cast<std::uint8_t>(Bits(v, 2, 22)), static_cast<std::uint8_t>(Bits(v, 3, 19)),
                        static_cast<std::uint8_t>(Bits(v, 8, 8)));
        break;
    default:
        break;  // (copies, textures, fog, indirect: not decoded here yet)
    }
}

void State::ApplyXF(std::uint32_t address, const std::uint32_t* words, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t a = address + i;
        const float f = AsFloat(words[i]);
        if (a < 0x100) {
            // matrix memory: rows of four, position matrices then texture ones
            const std::uint32_t row = a / 4, col = a % 4;
            if (row < 30) {
                mPosRows[row][col] = f;
            } else if (row < 60) {
                mTexRows[row - 30][col] = f;
            }
        } else if (a >= 0x400 && a < 0x460) {
            // normal matrices: rows of three
            const std::uint32_t row = (a - 0x400) / 3, col = (a - 0x400) % 3;
            if (row < mNrmRows.size()) {
                mNrmRows[row][col] = f;
            }
        } else if (a >= 0x500 && a < 0x600) {
            const std::uint32_t row = (a - 0x500) / 4, col = (a - 0x500) % 4;
            if (row < mPostRows.size()) {
                mPostRows[row][col] = f;
            }
        } else if (a >= 0x600 && a < 0x680) {
            Light& l = mLights[(a - 0x600) / 16];
            switch ((a - 0x600) % 16) {
            case 3: l.color = ColorFromXF(words[i]); break;
            case 4: case 5: case 6: l.cosAtt[(a - 0x600) % 16 - 4] = f; break;
            case 7: case 8: case 9: l.distAtt[(a - 0x600) % 16 - 7] = f; break;
            case 10: case 11: case 12: l.pos[(a - 0x600) % 16 - 10] = f; break;
            case 13: case 14: case 15: l.dir[(a - 0x600) % 16 - 13] = f; break;
            default: break;
            }
        } else if (a >= 0x1000) {
            ApplyXFRegister(a - 0x1000, words[i]);
        }
    }
}

void State::ApplyXFRegister(std::uint32_t reg, std::uint32_t v) {
    switch (reg) {
    case 0x09: SetNumChans(static_cast<std::uint8_t>(v)); return;
    case 0x0A: case 0x0B: SetChanAmbColor(static_cast<std::uint8_t>(4 + reg - 0x0A), ColorFromXF(v)); return;
    case 0x0C: case 0x0D: SetChanMatColor(static_cast<std::uint8_t>(4 + reg - 0x0C), ColorFromXF(v)); return;
    case 0x0E: case 0x0F: case 0x10: case 0x11: {
        const std::uint32_t attn = Bits(v, 2, 9);
        SetChanCtrl(static_cast<std::uint8_t>(reg - 0x0E), Bits(v, 1, 1) != 0, static_cast<std::uint8_t>(Bits(v, 1, 6)),
                    static_cast<std::uint8_t>(Bits(v, 1, 0)), Bits(v, 4, 2) | Bits(v, 4, 11) << 4,
                    static_cast<std::uint8_t>(Bits(v, 2, 7)), attn == 1 ? 0 : attn == 3 ? 1 : 2);
        return;
    }
    case 0x12: SetDualTexture(v != 0); return;
    case 0x18:
        SetCurrentMtx(Bits(v, 6, 0));
        for (unsigned t = 0; t < 4; ++t) {
            mVertex.texGens[t].matrix = TexMtxSlotFromRow(Bits(v, 6, 6 + t * 6));
        }
        return;
    case 0x19:
        for (unsigned t = 0; t < 4; ++t) {
            mVertex.texGens[4 + t].matrix = TexMtxSlotFromRow(Bits(v, 6, t * 6));
        }
        return;
    case 0x3F: SetNumTexGens(static_cast<std::uint8_t>(v)); return;
    default: break;
    }
    if (reg >= 0x1A && reg <= 0x1F) {
        mViewport[reg - 0x1A] = AsFloat(v);
    } else if (reg >= 0x20 && reg <= 0x25) {
        mProjParams[reg - 0x20] = AsFloat(v);
        RebuildProjection();
    } else if (reg == 0x26) {
        mProjOrtho = v != 0;
        RebuildProjection();
    } else if (reg >= 0x40 && reg < 0x48) {
        TexGen& tg = mVertex.texGens[reg - 0x40];
        const std::uint32_t type = Bits(v, 3, 4);
        tg.type = type == 0 ? (Bits(v, 1, 1) ? TexGenType::Mtx3x4 : TexGenType::Mtx2x4)
                  : type == 1 ? static_cast<TexGenType>(2 + Bits(v, 3, 15))  // bump (the shaders refuse it yet)
                              : TexGenType::SRTG;
        tg.source = TexGenSourceFromRow(Bits(v, 5, 7));
    } else if (reg >= 0x50 && reg < 0x58) {
        TexGen& tg = mVertex.texGens[reg - 0x50];
        const std::uint32_t row = Bits(v, 6, 0);
        tg.postMatrix = row == 61 ? kIdentity : static_cast<std::uint8_t>(row / 3);  // (GX_PTIDENTITY is row 61)
        tg.normalize = Bits(v, 1, 8) != 0;
    }
}

// the projection's six parameters as the 4x4 GXSetProjection would have
// loaded
void State::RebuildProjection() {
    const auto& p = mProjParams;
    std::array<std::array<float, 4>, 4> m{};
    m[0][0] = p[0];
    m[1][1] = p[2];
    m[2][2] = p[4];
    m[2][3] = p[5];
    if (mProjOrtho) {
        m[0][3] = p[1];
        m[1][3] = p[3];
        m[3][3] = 1.0f;
    } else {
        m[0][2] = p[1];
        m[1][2] = p[3];
        m[3][2] = -1.0f;
    }
    mProj = m;
}

// --- what a draw takes

TevConfig State::Tev() const {
    TevConfig config = mTev;
    config.texCoordCount = mVertex.texGenCount;
    return config;
}

VertexConfig State::Vertex() const {
    VertexConfig config = mVertex;
    config.hasNormal = mVtxDesc[kVaNrm] != 0;
    config.hasColor0 = mVtxDesc[kVaClr0] != 0;
    config.hasColor1 = mVtxDesc[kVaClr1] != 0;
    config.indexedPosMtx = mVtxDesc[kVaPnMtxIdx] != 0;
    config.texCoords = 0;
    for (int t = 0; t < 8; ++t) {
        if (mVtxDesc[kVaTex0 + t] != 0) {
            config.texCoords |= static_cast<std::uint8_t>(1u << t);
        }
    }
    return config;
}

std::vector<std::uint8_t> State::TevUniforms() const {
    std::vector<std::uint8_t> out(kTevBlockSize, 0);
    PutRows(out, 0, mTevRegs.data(), 4);
    PutRows(out, 64, mKColors.data(), 4);
    return out;
}

std::vector<std::uint8_t> State::XfUniforms() const {
    std::vector<std::uint8_t> out(kXfBlockSize, 0);
    PutRows(out, 0, mPosRows.data(), 30);
    PutRows(out, 480, mNrmRows.data(), 30);
    PutRows(out, 960, mTexRows.data(), 30);
    PutRows(out, 1440, mPostRows.data(), 60);
    PutRows(out, 2400, mProj.data(), 4);
    for (int c = 0; c < 2; ++c) {
        PutColor(out, 2464 + c * 16, mChanAmb[c]);
        PutColor(out, 2496 + c * 16, mChanMat[c]);
    }
    for (int i = 0; i < 8; ++i) {
        const Light& l = mLights[i];
        const std::size_t base = 2528 + static_cast<std::size_t>(i) * 80;
        const float pos[4] = {l.pos[0], l.pos[1], l.pos[2], 0.0f};
        const float dir[4] = {l.dir[0], l.dir[1], l.dir[2], 0.0f};
        const float cosAtt[4] = {l.cosAtt[0], l.cosAtt[1], l.cosAtt[2], 0.0f};
        const float distAtt[4] = {l.distAtt[0], l.distAtt[1], l.distAtt[2], 0.0f};
        Put(out, base, pos, 4);
        Put(out, base + 16, dir, 4);
        PutColor(out, base + 32, l.color);
        Put(out, base + 48, cosAtt, 4);
        Put(out, base + 64, distAtt, 4);
    }
    const std::int32_t index[4] = {static_cast<std::int32_t>(mCurrentPosRow), 0, 0, 0};
    std::memcpy(out.data() + 3168, index, sizeof(index));
    return out;
}

}  // namespace wiinx::gx
