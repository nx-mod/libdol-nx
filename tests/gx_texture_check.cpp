// GX texture decoding: blocks built here by hand, decoded, and each texel that
// says something about the format checked.
#include "wiinx/format/gx/texture.hpp"

#include <cstdio>
#include <cstdint>
#include <vector>

using wiinx::gx::TexFormat;
using wiinx::gx::TlutFormat;

namespace {
int gFailures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

struct Texel {
    std::uint8_t r, g, b, a;
};

Texel At(const std::vector<std::uint8_t>& rgba, std::size_t index) {
    return {rgba[index * 4], rgba[index * 4 + 1], rgba[index * 4 + 2], rgba[index * 4 + 3]};
}

bool Is(const std::vector<std::uint8_t>& rgba, std::size_t index, Texel want) {
    if (rgba.size() < (index + 1) * 4) {
        return false;
    }
    const Texel t = At(rgba, index);
    if (t.r != want.r || t.g != want.g || t.b != want.b || t.a != want.a) {
        std::printf("      texel %zu is %02x %02x %02x %02x, want %02x %02x %02x %02x\n", index, t.r, t.g, t.b, t.a,
                    want.r, want.g, want.b, want.a);
        return false;
    }
    return true;
}

std::vector<std::uint8_t> Decode(TexFormat format, std::uint32_t w, std::uint32_t h, std::uint32_t mips,
                                 const std::vector<std::uint8_t>& data) {
    return wiinx::gx::DecodeTexture(format, w, h, mips, data.data(), data.size());
}
}  // namespace

int main() {
    std::printf("sizes\n");
    Check("I4 8x8 is one 32-byte tile", wiinx::gx::TextureDataSize(TexFormat::I4, 8, 8, 1) == 32);
    Check("RGBA8 4x4 is one 64-byte tile", wiinx::gx::TextureDataSize(TexFormat::RGBA8, 4, 4, 1) == 64);
    Check("CMPR 1x1 still takes a whole tile", wiinx::gx::TextureDataSize(TexFormat::CMPR, 1, 1, 1) == 32);
    Check("I8 8x8 with its 4x4 mip is 96 bytes", wiinx::gx::TextureDataSize(TexFormat::I8, 8, 8, 2) == 96);
    Check("an unknown format has no size", wiinx::gx::TextureDataSize(static_cast<TexFormat>(7), 8, 8, 1) == 0);

    std::printf("intensity\n");
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0x1F;
        const auto out = Decode(TexFormat::I4, 8, 8, 1, data);
        Check("I4: the high nibble is the first texel, replicated", Is(out, 0, {0x11, 0x11, 0x11, 0x11}));
        Check("I4: the low nibble the second", Is(out, 1, {0xFF, 0xFF, 0xFF, 0xFF}));
    }
    {
        std::vector<std::uint8_t> data(64);
        for (int i = 0; i < 64; ++i) {
            data[i] = static_cast<std::uint8_t>(i);
        }
        const auto out = Decode(TexFormat::I8, 16, 4, 1, data);
        Check("I8: texel (8,0) is the second tile's first byte", Is(out, 8, {32, 32, 32, 32}));
        Check("I8: texel (0,1) is the first tile's second row", Is(out, 16, {8, 8, 8, 8}));
        const auto small = Decode(TexFormat::I8, 2, 2, 1, std::vector<std::uint8_t>(data.begin(), data.begin() + 32));
        Check("I8: a 2x2 texture reads its tile's second row at 8", Is(small, 3, {9, 9, 9, 9}));
    }
    {
        std::vector<std::uint8_t> data(96, 0);
        data[64 + 8] = 0x77;  // the mip's tile, second row
        const auto out = Decode(TexFormat::I8, 8, 8, 2, data);
        Check("I8: the 4x4 mip follows the 8x8 level", out.size() == (64 + 16) * 4 && Is(out, 64 + 4, {0x77, 0x77, 0x77, 0x77}));
    }
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0x3C;
        Check("IA4: intensity low nibble, alpha high", Is(Decode(TexFormat::IA4, 8, 4, 1, data), 0, {0xCC, 0xCC, 0xCC, 0x33}));
        data[0] = 0x80;
        data[1] = 0x40;
        Check("IA8: alpha first, then intensity", Is(Decode(TexFormat::IA8, 4, 4, 1, data), 0, {0x40, 0x40, 0x40, 0x80}));
    }

    std::printf("colour\n");
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0xF8;  // red, big-endian
        data[2] = 0x07;
        data[3] = 0xE0;  // green
        const auto out = Decode(TexFormat::RGB565, 4, 4, 1, data);
        Check("RGB565: red", Is(out, 0, {0xFF, 0, 0, 0xFF}));
        Check("RGB565: green", Is(out, 1, {0, 0xFF, 0, 0xFF}));
    }
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0xFF;
        data[1] = 0xFF;  // top bit set: RGB555, opaque
        data[2] = 0x3A;
        data[3] = 0x50;  // clear: alpha 3, then 4-bit R G B
        const auto out = Decode(TexFormat::RGB5A3, 4, 4, 1, data);
        Check("RGB5A3: RGB555 is opaque", Is(out, 0, {0xFF, 0xFF, 0xFF, 0xFF}));
        Check("RGB5A3: ARGB3444, alpha replicated from three bits", Is(out, 1, {0xAA, 0x55, 0x00, 109}));
    }
    {
        std::vector<std::uint8_t> data(64, 0);
        data[0] = 0x11;   // A
        data[1] = 0x22;   // R
        data[32] = 0x33;  // G
        data[33] = 0x44;  // B
        Check("RGBA8: AR from the first half, GB from the second",
              Is(Decode(TexFormat::RGBA8, 4, 4, 1, data), 0, {0x22, 0x33, 0x44, 0x11}));
    }

    std::printf("CMPR\n");
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 0xF8;  // colour 1: red
        data[3] = 0x1F;  // colour 2: blue, so four-colour mode
        data[4] = 0x1B;  // first row: indices 0 1 2 3
        const auto out = Decode(TexFormat::CMPR, 8, 8, 1, data);
        Check("CMPR: index 0 is colour 1", Is(out, 0, {0xFF, 0, 0, 0xFF}));
        Check("CMPR: index 1 is colour 2", Is(out, 1, {0, 0, 0xFF, 0xFF}));
        Check("CMPR: index 2 is 5/8 colour 1", Is(out, 2, {159, 0, 95, 0xFF}));
        Check("CMPR: index 3 is 3/8 colour 1", Is(out, 3, {95, 0, 159, 0xFF}));
        data[0] = 0x00;
        data[1] = 0x1F;  // colour 1: blue
        data[2] = 0xF8;
        data[3] = 0x00;  // colour 2: red, so three-colour mode
        const auto three = Decode(TexFormat::CMPR, 8, 8, 1, data);
        Check("CMPR: three-colour mode's index 3 is the midpoint, transparent", Is(three, 3, {127, 0, 127, 0}));
        data[8 + 0] = 0xF8;  // the second 4x4 block: (4,0) onward
        data[8 + 2] = 0xF8;
        Check("CMPR: the second block is the tile's top right", Is(Decode(TexFormat::CMPR, 8, 8, 1, data), 4, {0xFF, 0, 0, 0xFF}));
    }

    std::printf("palettes\n");
    {
        std::vector<std::uint8_t> data(32, 0);
        data[0] = 1;
        data[1] = 5;  // past the palette's two entries
        const std::uint8_t tlut[4] = {0xF8, 0x00, 0x00, 0x1F};  // RGB565: red, blue
        const auto out = wiinx::gx::DecodeTexturePalette(TexFormat::C8, 8, 4, 1, data.data(), data.size(),
                                                         TlutFormat::RGB565, 2, tlut, sizeof(tlut));
        Check("C8: an index reads its palette entry", Is(out, 0, {0, 0, 0xFF, 0xFF}));
        Check("C8: an index past the palette is transparent black", Is(out, 1, {0, 0, 0, 0}));
        Check("C8: decodes through DecodeTexturePalette, not DecodeTexture",
              Decode(TexFormat::C8, 8, 4, 1, data).empty());
        data[0] = 0x12;
        const auto indices = wiinx::gx::DecodeTextureIndices(TexFormat::C4, 8, 8, 1, data.data(), data.size());
        Check("C4: two indices to a byte, high first", indices.size() == 64 && indices[0] == 1 && indices[1] == 2);
        data[0] = 0xC0;
        data[1] = 0x05;
        const auto wide = wiinx::gx::DecodeTextureIndices(TexFormat::C14X2, 4, 4, 1, data.data(), data.size());
        Check("C14X2: big-endian, top two bits dropped", !wide.empty() && wide[0] == 0x0005);
    }

    std::printf("refusals\n");
    Check("short data decodes to nothing", Decode(TexFormat::RGBA8, 4, 4, 1, std::vector<std::uint8_t>(63)).empty());
    Check("a zero size decodes to nothing", Decode(TexFormat::I8, 0, 4, 1, std::vector<std::uint8_t>(32)).empty());

    std::printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
