#pragma once

// GX's state, as the SDK's setters change it, and what a draw needs from it:
// the shaders' configurations (tev.hpp, vertex.hpp), the pixel state
// (pixel.hpp) and the bytes of the shaders' two uniform blocks.
//
// The setters take the SDK's arguments in the SDK's numbers (GXSetTevOrder's
// stage, coordinate, map and channel IDs, GXLoadPosMtxImm's row IDs, ...), so a
// GX implementation over this is a line per function. Nothing here draws.

#include "wiinx/format/gx/pixel.hpp"
#include "wiinx/format/gx/tev.hpp"
#include "wiinx/format/gx/vertex.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace wiinx::gx {

struct Color8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
};

// GXLightObj, unpacked
struct Light {
    float pos[3] = {};
    float dir[3] = {};
    Color8 color;
    float cosAtt[3] = {1, 0, 0};   // a0, a1, a2
    float distAtt[3] = {1, 0, 0};  // k0, k1, k2
};

// the SDK's TEV presets (GXSetTevOp)
enum class TevMode : std::uint8_t { Modulate, Decal, Blend, Replace, PassClr };

class State {
  public:
    State();  // GXInit's defaults

    // --- TEV
    void SetNumTevStages(std::uint8_t count);
    void SetTevOrder(std::uint8_t stage, std::uint8_t coord, std::uint32_t map, std::uint8_t channel);
    void SetTevColorIn(std::uint8_t stage, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d);
    void SetTevAlphaIn(std::uint8_t stage, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d);
    void SetTevColorOp(std::uint8_t stage, std::uint8_t op, std::uint8_t bias, std::uint8_t scale, bool clamp,
                       std::uint8_t outReg);
    void SetTevAlphaOp(std::uint8_t stage, std::uint8_t op, std::uint8_t bias, std::uint8_t scale, bool clamp,
                       std::uint8_t outReg);
    void SetTevOp(std::uint8_t stage, TevMode mode);
    void SetTevKColorSel(std::uint8_t stage, std::uint8_t sel);
    void SetTevKAlphaSel(std::uint8_t stage, std::uint8_t sel);
    void SetTevSwapMode(std::uint8_t stage, std::uint8_t rasSel, std::uint8_t texSel);
    void SetTevSwapModeTable(std::uint8_t table, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);
    void SetAlphaCompare(std::uint8_t comp0, std::uint8_t ref0, std::uint8_t op, std::uint8_t comp1,
                         std::uint8_t ref1);
    // GX_TEVPREV, GX_TEVREG0..2; S10 takes the registers' signed range
    void SetTevColor(std::uint8_t reg, Color8 color);
    void SetTevColorS10(std::uint8_t reg, const std::int16_t rgba[4]);
    void SetTevKColor(std::uint8_t id, Color8 color);

    // --- channels, lights, texgens
    void SetNumChans(std::uint8_t count);
    void SetChanCtrl(std::uint8_t channel, bool enable, std::uint8_t ambSrc, std::uint8_t matSrc,
                     std::uint32_t lightMask, std::uint8_t diffFn, std::uint8_t attnFn);
    void SetChanAmbColor(std::uint8_t channel, Color8 color);
    void SetChanMatColor(std::uint8_t channel, Color8 color);
    void LoadLight(std::uint32_t lightMask, const Light& light);  // GXLoadLightObjImm: GX_LIGHT0 = 1 ...
    void SetNumTexGens(std::uint8_t count);
    void SetTexCoordGen2(std::uint8_t dst, std::uint8_t type, std::uint8_t src, std::uint32_t mtx, bool normalize,
                         std::uint32_t postMtx);

    // --- matrices (row-major 3x4 / 4x4, as the SDK passes them)
    void LoadPosMtx(const float mtx[3][4], std::uint32_t id);   // GX_PNMTX0 = 0, 3, ... 27
    void LoadNrmMtx(const float mtx[3][3], std::uint32_t id);
    // GX_TEXMTX0 = 30 ...; GX_PTTEXMTX0 = 64 ...; a GX_MTX2x4 loads two rows
    void LoadTexMtx(const float mtx[3][4], std::uint32_t id, bool twoRows = false);
    void SetCurrentMtx(std::uint32_t id);
    void SetProjection(const float mtx[4][4]);

    // --- the pixel engine
    void SetBlendMode(std::uint8_t type, std::uint8_t src, std::uint8_t dst, std::uint8_t logicOp);
    void SetZMode(bool enable, std::uint8_t func, bool update);
    void SetCullMode(std::uint8_t mode);
    void SetColorUpdate(bool update);
    void SetAlphaUpdate(bool update);

    // --- vertices (GX_VA_*, GX_NONE / DIRECT / INDEX8 / INDEX16)
    void ClearVtxDesc();
    void SetVtxDesc(std::uint8_t attr, std::uint8_t type);
    void SetDualTexture(bool enable);

    // --- what a draw takes
    TevConfig Tev() const;
    VertexConfig Vertex() const;
    PixelState Pixel() const { return mPixel; }
    // tev.hpp's TevBlock (128 bytes) and vertex.hpp's XfBlock, std140
    std::vector<std::uint8_t> TevUniforms() const;
    std::vector<std::uint8_t> XfUniforms() const;

    static constexpr std::size_t kTevBlockSize = 128;
    static constexpr std::size_t kXfBlockSize = 3184;

  private:
    TevConfig mTev;
    VertexConfig mVertex;
    PixelState mPixel;
    std::array<std::array<float, 4>, 4> mTevRegs{};  // PREV, REG0..2, as 0..1 of 255
    std::array<std::array<float, 4>, 4> mKColors{};
    std::array<Color8, 2> mChanAmb{};
    std::array<Color8, 2> mChanMat{};
    std::array<Light, 8> mLights{};
    std::array<std::array<float, 4>, 30> mPosRows{};
    std::array<std::array<float, 4>, 30> mNrmRows{};
    std::array<std::array<float, 4>, 30> mTexRows{};
    std::array<std::array<float, 4>, 60> mPostRows{};
    std::array<std::array<float, 4>, 4> mProj{};
    std::uint32_t mCurrentPosRow = 0;
    std::array<std::uint8_t, 21> mVtxDesc{};
};

}  // namespace wiinx::gx
