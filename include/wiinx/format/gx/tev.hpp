#pragma once

// The TEV, GX's fixed-function pixel pipeline, as GLSL for deko3d's compiler
// (UAM) - docs/deko3d.md, step 2.
//
// Up to sixteen stages, each a colour and an alpha combiner computing
//   d + ((1 - c) * a + c * b) + bias, scaled, clamped
// (or a compare), from the previous stage, three registers, a texture, a
// rasterised colour, constants - in the hardware's 8-bit arithmetic, which is
// what keeps the result exact rather than close. Then the alpha test.
//
// What the shader expects of whoever draws with it:
//   in  vec4 location 0, 1      the rasterised colour channels 0 and 1
//   in  vec3 location 2 + n     texture coordinate n (s, t, q)
//   uniform block binding 0     vec4 tevreg[4] (PREV, REG0..2 as GXSetTevColor
//                               sets them), vec4 kcolor[4]
//   sampler2D binding 0..7      texture maps 0..7
//   out vec4 location 0         the pixel
//
// After Aurora's WGSL generator (lib/gx/shader.cpp, MIT), which is after
// Dolphin's. Indirect textures, fog and the z texture are not here yet.

#include <array>
#include <cstdint>
#include <string>

namespace wiinx::gx {

// the SDK's own numbers for each, as a game passes them
enum class TevColorArg : std::uint8_t {
    CPrev, APrev, C0, A0, C1, A1, C2, A2, TexC, TexA, RasC, RasA, One, Half, Konst, Zero,
};
enum class TevAlphaArg : std::uint8_t { APrev, A0, A1, A2, TexA, RasA, Konst, Zero };
enum class TevOp : std::uint8_t {
    Add = 0,
    Sub = 1,
    CompR8Gt = 8,
    CompR8Eq = 9,
    CompGR16Gt = 10,
    CompGR16Eq = 11,
    CompBGR24Gt = 12,
    CompBGR24Eq = 13,
    CompRGB8Gt = 14,  // the alpha combiner's A8 compare
    CompRGB8Eq = 15,
};
enum class TevBias : std::uint8_t { Zero, AddHalf, SubHalf };
enum class TevScale : std::uint8_t { One, Two, Four, Half };
enum class TevReg : std::uint8_t { Prev, Reg0, Reg1, Reg2 };
// a stage's rasterised colour: channel 0 or 1, or none
enum class TevChannel : std::uint8_t { Color0 = 0, Color1 = 1, Zero = 6, Null = 0xFF };
enum class Compare : std::uint8_t { Never, Less, Equal, LEqual, Greater, NEqual, GEqual, Always };
enum class AlphaOp : std::uint8_t { And, Or, Xor, Xnor };
enum class SwapChannel : std::uint8_t { Red, Green, Blue, Alpha };

inline constexpr std::uint8_t kTexNull = 0xFF;

struct TevCombiner {
    std::uint8_t a, b, c, d;  // TevColorArg or TevAlphaArg
    TevOp op = TevOp::Add;
    TevBias bias = TevBias::Zero;
    TevScale scale = TevScale::One;
    bool clamp = true;
    TevReg out = TevReg::Prev;
};

struct TevStage {
    TevCombiner color{static_cast<std::uint8_t>(TevColorArg::Zero), static_cast<std::uint8_t>(TevColorArg::Zero),
                      static_cast<std::uint8_t>(TevColorArg::Zero), static_cast<std::uint8_t>(TevColorArg::Zero)};
    TevCombiner alpha{static_cast<std::uint8_t>(TevAlphaArg::Zero), static_cast<std::uint8_t>(TevAlphaArg::Zero),
                      static_cast<std::uint8_t>(TevAlphaArg::Zero), static_cast<std::uint8_t>(TevAlphaArg::Zero)};
    std::uint8_t kcolorSel = 0;     // GX_TEV_KCSEL_*: 0..7 the eighths, 0xC.. the registers
    std::uint8_t kalphaSel = 0;     // GX_TEV_KASEL_*
    std::uint8_t texCoord = kTexNull;
    std::uint8_t texMap = kTexNull;
    TevChannel channel = TevChannel::Null;
    std::uint8_t rasSwap = 0;       // into swapTable
    std::uint8_t texSwap = 0;
};

struct TevSwap {
    SwapChannel r = SwapChannel::Red, g = SwapChannel::Green, b = SwapChannel::Blue, a = SwapChannel::Alpha;
};

struct TevConfig {
    std::uint8_t stageCount = 1;
    std::array<TevStage, 16> stages{};
    std::array<TevSwap, 4> swapTable{};
    std::uint8_t texCoordCount = 0;  // how many coordinates the vertex shader writes
    Compare alphaComp0 = Compare::Always;
    std::uint8_t alphaRef0 = 0;
    AlphaOp alphaOp = AlphaOp::And;
    Compare alphaComp1 = Compare::Always;
    std::uint8_t alphaRef1 = 0;
};

// The fragment shader for `config`, GLSL 460 for UAM. Empty when the config
// names something out of range (a stage count past 16, an unknown argument).
std::string TevFragmentGlsl(const TevConfig& config);

}  // namespace wiinx::gx
