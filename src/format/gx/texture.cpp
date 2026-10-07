// GX textures decoded to RGBA8 (wiinx/format/gx/texture.hpp).
//
// The decoders are Aurora's (lib/gfx/texture_convert.cpp, MIT, Copyright the
// Aurora contributors), themselves after Dolphin's, without Aurora's types.
#include "wiinx/format/gx/texture.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace wiinx::gx {
namespace texture_detail {

struct RGBA8 {
    std::uint8_t r, g, b, a;
};
static_assert(sizeof(RGBA8) == 4);

// http://www.mindcontrol.org/~hplus/graphics/expand-bits.html
template <unsigned Bits>
constexpr std::uint8_t ExpandTo8(std::uint8_t n) {
    if constexpr (Bits == 3) {
        return static_cast<std::uint8_t>((n << (8 - 3)) | (n << (8 - 6)) | (n >> (9 - 8)));
    } else {
        return static_cast<std::uint8_t>((n << (8 - Bits)) | (n >> ((Bits * 2) - 8)));
    }
}

constexpr std::uint8_t S3TCBlend(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::uint8_t>((((a << 1) + a) + ((b << 2) + b)) >> 3);
}

constexpr std::uint8_t HalfBlend(std::uint8_t a, std::uint8_t b) {
    return static_cast<std::uint8_t>((static_cast<std::uint32_t>(a) + b) >> 1);
}

// a big-endian 16-bit value, wherever it sits
std::uint16_t Be16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

std::size_t MippedTexels(std::uint32_t w, std::uint32_t h, std::uint32_t mips) {
    std::size_t total = static_cast<std::size_t>(w) * h;
    for (std::uint32_t i = mips; i > 1; --i) {
        w = std::max(w >> 1, 1u);
        h = std::max(h >> 1, 1u);
        total += static_cast<std::size_t>(w) * h;
    }
    return total;
}

struct Tiling {
    std::uint32_t blockWidth, blockHeight, blockBytes;
};

bool TilingOf(TexFormat format, Tiling& tiling) {
    switch (format) {
    case TexFormat::I4:
    case TexFormat::C4:
    case TexFormat::CMPR: tiling = {8, 8, 32}; return true;
    case TexFormat::I8:
    case TexFormat::IA4:
    case TexFormat::C8:
    case TexFormat::Z8: tiling = {8, 4, 32}; return true;
    case TexFormat::IA8:
    case TexFormat::RGB565:
    case TexFormat::RGB5A3:
    case TexFormat::C14X2:
    case TexFormat::Z16: tiling = {4, 4, 32}; return true;
    case TexFormat::RGBA8:
    case TexFormat::Z24X8: tiling = {4, 4, 64}; return true;
    }
    return false;
}

// One texel decoder per format: a tile row of `BlockWidth` texels takes
// BlockWidth / Frac source units.
struct DecodeI4 {
    using Target = RGBA8;
    static constexpr std::uint32_t Frac = 2, BlockWidth = 8, BlockHeight = 8, SourceBytes = 1;
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) {
        const std::uint8_t i = ExpandTo8<4>(in[x / 2] >> (x & 1 ? 0 : 4) & 0xf);
        t[x] = {i, i, i, i};
    }
#if defined(__ARM_NEON)
    // eight texels from four bytes: high nibble then low, each replicated to 8
    // bits, written as four equal channels
    static bool Row(Target* t, const std::uint8_t* in, std::uint32_t n) {
        if (n != 8) {
            return false;
        }
        std::uint32_t word;
        std::memcpy(&word, in, 4);
        const uint8x8_t packed = vreinterpret_u8_u32(vdup_n_u32(word));
        const uint8x8x2_t zipped = vzip_u8(vshr_n_u8(packed, 4), vand_u8(packed, vdup_n_u8(0x0f)));
        const uint8x8_t i = vorr_u8(vshl_n_u8(zipped.val[0], 4), zipped.val[0]);
        vst4_u8(reinterpret_cast<std::uint8_t*>(t), uint8x8x4_t{{i, i, i, i}});
        return true;
    }
#endif
};

struct DecodeI8 {
    using Target = RGBA8;
    static constexpr std::uint32_t Frac = 1, BlockWidth = 8, BlockHeight = 4, SourceBytes = 1;
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) { t[x] = {in[x], in[x], in[x], in[x]}; }
#if defined(__ARM_NEON)
    // a THP video frame's planes are I8: one byte to four in three instructions
    static bool Row(Target* t, const std::uint8_t* in, std::uint32_t n) {
        if (n != 8) {
            return false;
        }
        const uint8x8_t i = vld1_u8(in);
        vst4_u8(reinterpret_cast<std::uint8_t*>(t), uint8x8x4_t{{i, i, i, i}});
        return true;
    }
#endif
};

struct DecodeIA4 {
    using Target = RGBA8;
    static constexpr std::uint32_t Frac = 1, BlockWidth = 8, BlockHeight = 4, SourceBytes = 1;
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) {
        const std::uint8_t i = ExpandTo8<4>(in[x] & 0xf);
        t[x] = {i, i, i, ExpandTo8<4>(in[x] >> 4)};
    }
};

struct DecodeIA8 {
    using Target = RGBA8;
    static constexpr std::uint32_t Frac = 1, BlockWidth = 4, BlockHeight = 4, SourceBytes = 2;
    // alpha first, then intensity
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) {
        const std::uint8_t i = in[x * 2 + 1];
        t[x] = {i, i, i, in[x * 2]};
    }
};

struct DecodeC4 {
    using Target = std::uint16_t;
    static constexpr std::uint32_t Frac = 2, BlockWidth = 8, BlockHeight = 8, SourceBytes = 1;
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) {
        t[x] = in[x / 2] >> (x & 1 ? 0 : 4) & 0xf;
    }
};

struct DecodeC8 {
    using Target = std::uint16_t;
    static constexpr std::uint32_t Frac = 1, BlockWidth = 8, BlockHeight = 4, SourceBytes = 1;
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) { t[x] = in[x]; }
};

struct DecodeC14X2 {
    using Target = std::uint16_t;
    static constexpr std::uint32_t Frac = 1, BlockWidth = 4, BlockHeight = 4, SourceBytes = 2;
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) { t[x] = Be16(in + x * 2) & 0x3fff; }
};

struct DecodeRGB565 {
    using Target = RGBA8;
    static constexpr std::uint32_t Frac = 1, BlockWidth = 4, BlockHeight = 4, SourceBytes = 2;
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) {
        const std::uint16_t v = Be16(in + x * 2);
        t[x] = {ExpandTo8<5>(v >> 11 & 0x1f), ExpandTo8<6>(v >> 5 & 0x3f), ExpandTo8<5>(v & 0x1f), 0xff};
    }
};

struct DecodeRGB5A3 {
    using Target = RGBA8;
    static constexpr std::uint32_t Frac = 1, BlockWidth = 4, BlockHeight = 4, SourceBytes = 2;
    // top bit set: RGB555, opaque; clear: ARGB3444
    static void Texel(Target* t, const std::uint8_t* in, std::uint32_t x) {
        const std::uint16_t v = Be16(in + x * 2);
        if (v & 0x8000) {
            t[x] = {ExpandTo8<5>(v >> 10 & 0x1f), ExpandTo8<5>(v >> 5 & 0x1f), ExpandTo8<5>(v & 0x1f), 0xff};
        } else {
            t[x] = {ExpandTo8<4>(v >> 8 & 0xf), ExpandTo8<4>(v >> 4 & 0xf), ExpandTo8<4>(v & 0xf),
                    ExpandTo8<3>(v >> 12 & 0x7)};
        }
    }
};

template <typename D>
std::vector<typename D::Target> DecodeTiled(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                                            const std::uint8_t* in) {
    std::vector<typename D::Target> out(MippedTexels(width, height, mips));
    auto* level = out.data();
    std::uint32_t w = width, h = height;
    // one tile row's source bytes
    constexpr std::uint32_t rowBytes = D::BlockWidth / D::Frac * D::SourceBytes;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint32_t tilesWide = (w + D::BlockWidth - 1) / D::BlockWidth;
        const std::uint32_t tilesHigh = (h + D::BlockHeight - 1) / D::BlockHeight;
        for (std::uint32_t ty = 0; ty < tilesHigh; ++ty) {
            const std::uint32_t baseY = ty * D::BlockHeight;
            const std::uint32_t rows = std::min(h - baseY, D::BlockHeight);
            for (std::uint32_t tx = 0; tx < tilesWide; ++tx) {
                const std::uint32_t baseX = tx * D::BlockWidth;
                const std::uint32_t n = std::min(w - baseX, D::BlockWidth);
                for (std::uint32_t y = 0; y < rows; ++y) {
                    auto* target = level + static_cast<std::size_t>(baseY + y) * w + baseX;
                    bool done = false;
                    if constexpr (requires { D::Row(target, in, n); }) {
                        done = D::Row(target, in, n);
                    }
                    for (std::uint32_t x = 0; !done && x < n; ++x) {
                        D::Texel(target, in, x);
                    }
                    in += rowBytes;
                }
                // (a tile cut off at the bottom still takes its whole 32 bytes)
                in += rowBytes * (D::BlockHeight - rows);
            }
        }
        level += static_cast<std::size_t>(w) * h;
        w = std::max(w >> 1, 1u);
        h = std::max(h >> 1, 1u);
    }
    return out;
}

// RGBA8: each 4x4 tile is 32 bytes of AR pairs, then 32 of GB pairs
std::vector<RGBA8> DecodeRGBA8(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                               const std::uint8_t* in) {
    std::vector<RGBA8> out(MippedTexels(width, height, mips));
    auto* level = out.data();
    std::uint32_t w = width, h = height;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        for (std::uint32_t baseY = 0; baseY < h; baseY += 4) {
            for (std::uint32_t baseX = 0; baseX < w; baseX += 4) {
                for (std::uint32_t half = 0; half < 2; ++half) {
                    for (std::uint32_t y = 0; y < 4; ++y, in += 8) {
                        for (std::uint32_t x = 0; x < 4; ++x) {
                            if (baseX + x >= w || baseY + y >= h) {
                                continue;
                            }
                            RGBA8& t = level[static_cast<std::size_t>(baseY + y) * w + baseX + x];
                            if (half == 0) {
                                t.a = in[x * 2];
                                t.r = in[x * 2 + 1];
                            } else {
                                t.g = in[x * 2];
                                t.b = in[x * 2 + 1];
                            }
                        }
                    }
                }
            }
        }
        level += static_cast<std::size_t>(w) * h;
        w = std::max(w >> 1, 1u);
        h = std::max(h >> 1, 1u);
    }
    return out;
}

// CMPR: 8x8 tiles of four DXT1 blocks (top left, top right, bottom left,
// bottom right), colours big-endian; the three-colour mode's fourth entry is
// the midpoint with alpha 0, not black
std::vector<RGBA8> DecodeCMPR(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                              const std::uint8_t* in) {
    std::vector<RGBA8> out(MippedTexels(width, height, mips));
    auto* level = out.data();
    std::uint32_t w = width, h = height;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        for (std::uint32_t yy = 0; yy < h; yy += 8) {
            for (std::uint32_t xx = 0; xx < w; xx += 8) {
                for (std::uint32_t yb = 0; yb < 8; yb += 4) {
                    for (std::uint32_t xb = 0; xb < 8; xb += 4) {
                        const std::uint16_t c1 = Be16(in);
                        const std::uint16_t c2 = Be16(in + 2);
                        in += 4;
                        std::array<RGBA8, 4> colors{};
                        colors[0] = {ExpandTo8<5>(c1 >> 11 & 0x1f), ExpandTo8<6>(c1 >> 5 & 0x3f),
                                     ExpandTo8<5>(c1 & 0x1f), 0xff};
                        colors[1] = {ExpandTo8<5>(c2 >> 11 & 0x1f), ExpandTo8<6>(c2 >> 5 & 0x3f),
                                     ExpandTo8<5>(c2 & 0x1f), 0xff};
                        if (c1 > c2) {
                            colors[2] = {S3TCBlend(colors[1].r, colors[0].r), S3TCBlend(colors[1].g, colors[0].g),
                                         S3TCBlend(colors[1].b, colors[0].b), 0xff};
                            colors[3] = {S3TCBlend(colors[0].r, colors[1].r), S3TCBlend(colors[0].g, colors[1].g),
                                         S3TCBlend(colors[0].b, colors[1].b), 0xff};
                        } else {
                            colors[2] = {HalfBlend(colors[0].r, colors[1].r), HalfBlend(colors[0].g, colors[1].g),
                                         HalfBlend(colors[0].b, colors[1].b), 0xff};
                            colors[3] = colors[2];
                            colors[3].a = 0;
                        }
                        for (std::uint32_t y = 0; y < 4; ++y) {
                            std::uint8_t bits = in[y];
                            for (std::uint32_t x = 0; x < 4; ++x, bits <<= 2) {
                                if (xx + xb + x < w && yy + yb + y < h) {
                                    level[static_cast<std::size_t>(yy + yb + y) * w + xx + xb + x] = colors[bits >> 6 & 3];
                                }
                            }
                        }
                        in += 4;
                    }
                }
            }
        }
        level += static_cast<std::size_t>(w) * h;
        w = std::max(w >> 1, 1u);
        h = std::max(h >> 1, 1u);
    }
    return out;
}

std::vector<std::uint8_t> Bytes(const std::vector<RGBA8>& texels) {
    std::vector<std::uint8_t> bytes(texels.size() * 4);
    if (!texels.empty()) {
        std::memcpy(bytes.data(), texels.data(), bytes.size());
    }
    return bytes;
}

}  // namespace texture_detail

using namespace texture_detail;

bool IsKnown(TexFormat format) {
    Tiling tiling;
    return TilingOf(format, tiling);
}

bool IsPaletted(TexFormat format) {
    return format == TexFormat::C4 || format == TexFormat::C8 || format == TexFormat::C14X2;
}

std::size_t TextureDataSize(TexFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t mips) {
    Tiling tiling;
    if (width == 0 || height == 0 || mips == 0 || !TilingOf(format, tiling)) {
        return 0;
    }
    std::size_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::size_t wide = (static_cast<std::size_t>(width) + tiling.blockWidth - 1) / tiling.blockWidth;
        const std::size_t high = (static_cast<std::size_t>(height) + tiling.blockHeight - 1) / tiling.blockHeight;
        if (wide * high > (std::numeric_limits<std::size_t>::max() - total) / tiling.blockBytes) {
            return 0;
        }
        total += wide * high * tiling.blockBytes;
        width = std::max(width >> 1, 1u);
        height = std::max(height >> 1, 1u);
    }
    return total;
}

std::vector<std::uint8_t> DecodeTexture(TexFormat format, std::uint32_t width, std::uint32_t height,
                                        std::uint32_t mips, const std::uint8_t* data, std::size_t size) {
    const std::size_t needed = TextureDataSize(format, width, height, mips);
    if (needed == 0 || data == nullptr || size < needed || IsPaletted(format)) {
        return {};
    }
    switch (format) {
    case TexFormat::I4: return Bytes(DecodeTiled<DecodeI4>(width, height, mips, data));
    case TexFormat::I8:
    case TexFormat::Z8: return Bytes(DecodeTiled<DecodeI8>(width, height, mips, data));
    case TexFormat::IA4: return Bytes(DecodeTiled<DecodeIA4>(width, height, mips, data));
    case TexFormat::IA8:
    case TexFormat::Z16: return Bytes(DecodeTiled<DecodeIA8>(width, height, mips, data));
    case TexFormat::RGB565: return Bytes(DecodeTiled<DecodeRGB565>(width, height, mips, data));
    case TexFormat::RGB5A3: return Bytes(DecodeTiled<DecodeRGB5A3>(width, height, mips, data));
    case TexFormat::RGBA8:
    case TexFormat::Z24X8: return Bytes(DecodeRGBA8(width, height, mips, data));
    case TexFormat::CMPR: return Bytes(DecodeCMPR(width, height, mips, data));
    default: return {};
    }
}

std::vector<std::uint16_t> DecodeTextureIndices(TexFormat format, std::uint32_t width, std::uint32_t height,
                                                std::uint32_t mips, const std::uint8_t* data, std::size_t size) {
    const std::size_t needed = TextureDataSize(format, width, height, mips);
    if (needed == 0 || data == nullptr || size < needed) {
        return {};
    }
    switch (format) {
    case TexFormat::C4: return DecodeTiled<DecodeC4>(width, height, mips, data);
    case TexFormat::C8: return DecodeTiled<DecodeC8>(width, height, mips, data);
    case TexFormat::C14X2: return DecodeTiled<DecodeC14X2>(width, height, mips, data);
    default: return {};
    }
}

std::vector<std::uint8_t> DecodeTlut(TlutFormat format, std::uint32_t entries, const std::uint8_t* data,
                                     std::size_t size) {
    if (data == nullptr || size < static_cast<std::size_t>(entries) * 2) {
        return {};
    }
    std::vector<RGBA8> out(entries);
    for (std::uint32_t i = 0; i < entries; ++i) {
        switch (format) {
        case TlutFormat::IA8: DecodeIA8::Texel(out.data(), data, i); break;
        case TlutFormat::RGB565: DecodeRGB565::Texel(out.data(), data, i); break;
        case TlutFormat::RGB5A3: DecodeRGB5A3::Texel(out.data(), data, i); break;
        default: return {};
        }
    }
    return Bytes(out);
}

std::vector<std::uint8_t> DecodeTexturePalette(TexFormat format, std::uint32_t width, std::uint32_t height,
                                               std::uint32_t mips, const std::uint8_t* data, std::size_t size,
                                               TlutFormat tlutFormat, std::uint32_t entries,
                                               const std::uint8_t* tlut, std::size_t tlutSize) {
    const std::vector<std::uint16_t> indices = DecodeTextureIndices(format, width, height, mips, data, size);
    const std::vector<std::uint8_t> palette = DecodeTlut(tlutFormat, entries, tlut, tlutSize);
    if (indices.empty() || palette.empty()) {
        return {};
    }
    std::vector<std::uint8_t> out(indices.size() * 4, 0);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] < entries) {
            std::memcpy(out.data() + i * 4, palette.data() + static_cast<std::size_t>(indices[i]) * 4, 4);
        }
    }
    return out;
}

}  // namespace wiinx::gx
