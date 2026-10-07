#pragma once

// GX vertices as the vertex shader reads them (vertex.hpp).
//
// A game's vertices are packed big-endian records, each attribute in the
// format its vertex attribute table entry gives: positions, normals and
// texture coordinates as u8/s8/u16/s16 fixed point or f32, colours in one of
// six packed formats, matrix indices as bytes. libdol's GX code expands indexed
// attributes into the record before a draw, so every attribute here is direct.
//
// Out comes one fixed record of kVertexFloats floats a vertex:
//   [0..2]   position (z 0 for an XY position)
//   [3..5]   normal
//   [6..9]   colour 0, 0..1      [10..13] colour 1
//   [14..29] texture coordinates 0..7, s and t (t 0 for an S-only one)
//   [30]     the position-matrix index, as GX's row number
//   [31]     (unused)
// at the byte offsets kOffset* below, which the renderer gives deko3d as the
// vertex attribute state.

#include <array>
#include <cstddef>
#include <cstdint>

namespace wiinx::gx {

// GX_VA_*
enum VertexAttr : std::uint8_t {
    VaPnMtxIdx = 0,
    VaTex0MtxIdx = 1,  // .. 8
    VaPos = 9,
    VaNrm = 10,
    VaClr0 = 11,
    VaClr1 = 12,
    VaTex0 = 13,  // .. 20
    VaCount = 21,
};

// GX_U8 .. GX_F32, and the colour formats GX_RGB565 .. GX_RGBA8
enum class CompType : std::uint8_t { U8, S8, U16, S16, F32 };
enum class ColorType : std::uint8_t { RGB565, RGB8, RGBX8, RGBA4, RGBA6, RGBA8 };

// one attribute's table entry: GX_POS_XY/XYZ, GX_NRM_XYZ/NBT/NBT3,
// GX_CLR_RGB/RGBA, GX_TEX_S/ST as `count`; the component or colour type; the
// fixed-point shift (not used by normals, whose scale is fixed)
struct AttrFormat {
    std::uint8_t count = 1;
    std::uint8_t type = 0;
    std::uint8_t frac = 0;
};

struct VertexFormat {
    std::array<bool, VaCount> present{};  // which attributes the records carry
    std::array<AttrFormat, VaCount> format{};
};

inline constexpr std::size_t kVertexFloats = 32;
inline constexpr std::size_t kOffsetPosition = 0;
inline constexpr std::size_t kOffsetNormal = 3 * 4;
inline constexpr std::size_t kOffsetColor0 = 6 * 4;
inline constexpr std::size_t kOffsetColor1 = 10 * 4;
inline constexpr std::size_t kOffsetTex0 = 14 * 4;  // + 8 bytes a coordinate
inline constexpr std::size_t kOffsetPosMtxIndex = 30 * 4;

// The bytes one packed record of `format` takes; 0 when a format is invalid.
std::size_t PackedVertexSize(const VertexFormat& format);

// `count` records from `src` (`size` bytes) into `out` (count * kVertexFloats
// floats). Returns how many it converted: fewer than `count` when `size` runs
// out or the format is invalid.
std::size_t ConvertVertices(const VertexFormat& format, const std::uint8_t* src, std::size_t size, std::size_t count,
                            float* out);

}  // namespace wiinx::gx
