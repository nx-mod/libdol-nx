#pragma once

// GX textures: the formats Flipper and Hollywood sample, decoded to RGBA8.
//
// A texture is stored in tiles - 8x8, 8x4 or 4x4 texels depending on the
// format, each tile 32 bytes (64 for RGBA8, whose two halves hold AR and GB),
// tiles left to right, top to bottom - and big-endian. Mip levels follow one
// another, each half the size of the last (never below 1). The CI formats are
// indices into a palette (a TLUT); CMPR is S3TC/DXT1 with big-endian colours
// and 2x2 blocks of its 4x4 blocks per tile.
//
// What the renderer uploads. Decoders after Aurora's lib/gfx/texture_convert.cpp
// (MIT), which follows Dolphin's.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wiinx::gx {

// the hardware's own format numbers
enum class TexFormat : std::uint32_t {
    I4 = 0x0,
    I8 = 0x1,
    IA4 = 0x2,
    IA8 = 0x3,
    RGB565 = 0x4,
    RGB5A3 = 0x5,
    RGBA8 = 0x6,
    C4 = 0x8,
    C8 = 0x9,
    C14X2 = 0xA,
    CMPR = 0xE,
    // depth copies read back as textures: decoded as I8, IA8 and RGBA8 are
    Z8 = 0x11,
    Z16 = 0x13,
    Z24X8 = 0x16,
};

enum class TlutFormat : std::uint32_t {
    IA8 = 0,
    RGB565 = 1,
    RGB5A3 = 2,
};

// A known format?
bool IsKnown(TexFormat format);
// C4, C8 and C14X2: indices into a palette
bool IsPaletted(TexFormat format);

// The bytes a `mips`-level texture takes, or 0 when the format is unknown or a
// size is 0.
std::size_t TextureDataSize(TexFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t mips);

// RGBA8 texels, each level after the last. Empty when the format is unknown or
// paletted, or `size` is short of TextureDataSize.
std::vector<std::uint8_t> DecodeTexture(TexFormat format, std::uint32_t width, std::uint32_t height,
                                        std::uint32_t mips, const std::uint8_t* data, std::size_t size);

// A paletted texture's indices, one per texel, each level after the last.
// Empty when the format is not paletted or `size` is short.
std::vector<std::uint16_t> DecodeTextureIndices(TexFormat format, std::uint32_t width, std::uint32_t height,
                                                std::uint32_t mips, const std::uint8_t* data, std::size_t size);

// A palette as RGBA8: `entries` big-endian 16-bit entries. Empty when `size`
// is short.
std::vector<std::uint8_t> DecodeTlut(TlutFormat format, std::uint32_t entries, const std::uint8_t* data,
                                     std::size_t size);

// A paletted texture through its palette, as RGBA8. An index past the
// palette's end reads transparent black.
std::vector<std::uint8_t> DecodeTexturePalette(TexFormat format, std::uint32_t width, std::uint32_t height,
                                               std::uint32_t mips, const std::uint8_t* data, std::size_t size,
                                               TlutFormat tlutFormat, std::uint32_t entries,
                                               const std::uint8_t* tlut, std::size_t tlutSize);

}  // namespace wiinx::gx
