#pragma once

// GX's transform unit (XF) as a GLSL vertex shader for deko3d's compiler
// (UAM) - docs/deko3d.md, step 2; the partner of tev.hpp's fragment shader.
//
// Position through the vertex's position matrix and the projection; the colour
// channels, lit as XF lights them (eight lights, the diffuse functions, spot
// and specular attenuation, 8-bit accumulation); the texture coordinates
// generated, through their texture matrices and, with dual texturing, the
// post-transform matrices.
//
// What the shader expects of whoever draws with it (the renderer turns a draw's
// vertices into this layout from libdol's display-list decoder):
//   in  vec3 location 0          position
//   in  vec3 location 1          normal
//   in  vec4 location 2, 3       colour 0, 1 (0..1)
//   in  vec2 location 4 + n      texture coordinate n
//   in  float location 12        the position-matrix index (GX's PNMTXIDX: a
//                                row index, three to a matrix), when indexed
//   uniform block binding 1      XfBlock below
// and writes what tev.hpp's fragment shader reads (colours at 0 and 1, texture
// coordinates at 2 + n as s, t, q).
//
//   layout (std140, binding = 1) uniform XfBlock {
//       vec4 posMtx[30];    // ten position matrices, three rows each
//       vec4 nrmMtx[30];    // their normal matrices, xyz of three rows
//       vec4 texMtx[30];    // ten texture matrices
//       vec4 postMtx[60];   // twenty post-transform matrices
//       vec4 proj[4];       // the projection, as four rows
//       vec4 chanAmb[2];    // ambient colour per channel
//       vec4 chanMat[2];    // material colour per channel
//       XfLight lights[8];  // pos, dir, color, cosAtt, distAtt (vec4 each)
//       ivec4 posMtxIndex;  // .x: the matrix row when vertices carry none
//   } xf;
//
// Clip depth comes out in 0..w (GX's -w..0, moved), for a device set to a
// 0..1 depth range. After Aurora's WGSL generator (lib/gx/shader.cpp, MIT).

#include <array>
#include <cstdint>
#include <string>

namespace wiinx::gx {

enum class ColorSrc : std::uint8_t { Reg, Vertex };
enum class DiffuseFn : std::uint8_t { None, Sign, Clamp };
enum class AttnFn : std::uint8_t { Spec = 0, Spot = 1, None = 2 };  // GX_AF_*

// one of XF's four lighting outputs: colour 0, colour 1, alpha 0, alpha 1
struct ColorChannel {
    ColorSrc material = ColorSrc::Reg;
    ColorSrc ambient = ColorSrc::Reg;
    bool lighting = false;
    std::uint8_t lightMask = 0;
    DiffuseFn diffuse = DiffuseFn::None;
    AttnFn attenuation = AttnFn::None;
};

enum class TexGenType : std::uint8_t { Mtx3x4 = 0, Mtx2x4 = 1, SRTG = 10 };  // (the bump types are not here yet)
enum class TexGenSrc : std::uint8_t {
    Position = 0,
    Normal = 1,
    Tex0 = 4,  // .. Tex7 = 11
    Color0 = 19,
    Color1 = 20,
};

inline constexpr std::uint8_t kIdentity = 0xFF;

struct TexGen {
    TexGenType type = TexGenType::Mtx2x4;
    std::uint8_t source = static_cast<std::uint8_t>(TexGenSrc::Tex0);  // TexGenSrc, Tex0 + n for coordinate n
    std::uint8_t matrix = kIdentity;    // texture matrix 0..9
    bool normalize = false;             // dual texturing only
    std::uint8_t postMatrix = kIdentity;  // post-transform matrix 0..19, dual texturing only
};

struct VertexConfig {
    bool hasNormal = false;
    bool hasColor0 = false;
    bool hasColor1 = false;
    std::uint8_t texCoords = 0;       // a bit per texture coordinate the vertices carry
    bool indexedPosMtx = false;       // vertices carry their position-matrix index
    std::uint8_t channelCount = 0;    // GXSetNumChans: 0..2
    std::array<ColorChannel, 4> channels{};  // colour 0, colour 1, alpha 0, alpha 1
    std::uint8_t texGenCount = 0;     // GXSetNumTexGens: 0..8
    std::array<TexGen, 8> texGens{};
    bool dualTexture = false;
};

// The vertex shader for `config`, GLSL 460 for UAM. Empty when the config asks
// for something this does not do (a bump or unknown texgen, a matrix out of
// range, a colour source the vertices do not carry is answered as white).
std::string XfVertexGlsl(const VertexConfig& config);

// A byte string naming `config` for a cache (see TevConfigKey).
std::string VertexConfigKey(const VertexConfig& config);

}  // namespace wiinx::gx
