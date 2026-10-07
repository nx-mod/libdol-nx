#pragma once

// GX's pixel engine state: blending, the logic op, the depth test, culling and
// which channels a draw writes - GXSetBlendMode, GXSetZMode, GXSetCullMode,
// GXSetColorUpdate, GXSetAlphaUpdate - in the SDK's own numbers. The Switch
// renderer turns it into deko3d's state objects (src/platform/gpu,
// docs/deko3d.md).

#include <cstdint>

namespace wiinx::gx {

enum class BlendMode : std::uint8_t { None, Blend, Logic, Subtract };

// GX_BL_*: 2 and 3 are the destination's colour as a source factor and the
// source's colour as a destination factor
enum class BlendFactor : std::uint8_t {
    Zero,
    One,
    SrcClr,     // DSTCLR as the source factor
    InvSrcClr,  // INVDSTCLR as the source factor
    SrcAlpha,
    InvSrcAlpha,
    DstAlpha,
    InvDstAlpha,
};

// GX_LO_*
enum class LogicOp : std::uint8_t {
    Clear, And, RevAnd, Copy, InvAnd, NoOp, Xor, Or, Nor, Equiv, Inv, RevOr, InvCopy, InvOr, Nand, Set,
};

// GX_CULL_*: GX's front faces are the clockwise ones
enum class CullMode : std::uint8_t { None, Front, Back, All };

struct PixelState {
    BlendMode blendMode = BlendMode::None;
    BlendFactor srcFactor = BlendFactor::One;
    BlendFactor dstFactor = BlendFactor::Zero;
    LogicOp logicOp = LogicOp::Copy;
    bool depthTest = true;
    std::uint8_t depthCompare = 3;  // GX_NEVER..GX_ALWAYS (tev.hpp's Compare): GX_LEQUAL
    bool depthWrite = true;
    CullMode cull = CullMode::Back;
    bool colorWrite = true;
    bool alphaWrite = false;
};

}  // namespace wiinx::gx
