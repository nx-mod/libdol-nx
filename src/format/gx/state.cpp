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
