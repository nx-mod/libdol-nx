// GX vertex records converted: records packed here by hand, converted, and
// each value that says something about a format checked.
#include "wiinx/format/gx/vertex_stream.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace wiinx::gx;

namespace {
int gFailures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

bool Near(float a, float b) { return a > b - 1e-4f && a < b + 1e-4f; }

void PutF32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}
}  // namespace

int main() {
    std::printf("float position, RGBA8 colour, float ST\n");
    {
        VertexFormat f;
        f.present[VaPos] = f.present[VaClr0] = f.present[VaTex0] = true;
        f.format[VaPos] = {1, static_cast<std::uint8_t>(CompType::F32), 0};
        f.format[VaClr0] = {1, static_cast<std::uint8_t>(ColorType::RGBA8), 0};
        f.format[VaTex0] = {1, static_cast<std::uint8_t>(CompType::F32), 0};
        Check("a record is 12 + 4 + 8 bytes", PackedVertexSize(f) == 24);
        std::vector<std::uint8_t> src;
        for (int vtx = 0; vtx < 2; ++vtx) {
            PutF32(src, 1.5f + vtx), PutF32(src, -2.0f), PutF32(src, 0.25f);
            src.insert(src.end(), {255, 128, 0, 64});
            PutF32(src, 0.5f), PutF32(src, 0.75f);
        }
        std::vector<float> out(2 * kVertexFloats);
        Check("both records convert", ConvertVertices(f, src.data(), src.size(), 2, out.data()) == 2);
        Check("the position, big-endian", Near(out[0], 1.5f) && Near(out[1], -2.0f) && Near(out[2], 0.25f));
        Check("the second record's x", Near(out[kVertexFloats], 2.5f));
        Check("colour 0 as 0..1", Near(out[6], 1.0f) && Near(out[7], 128 / 255.0f) && Near(out[9], 64 / 255.0f));
        Check("texture coordinate 0", Near(out[14], 0.5f) && Near(out[15], 0.75f));
        Check("colour 1, absent, reads white", Near(out[10], 1.0f) && Near(out[13], 1.0f));
    }

    std::printf("fixed point: s16 XY with 8 fractional bits, u8 S, s8 normal, a matrix index\n");
    {
        VertexFormat f;
        f.present[VaPnMtxIdx] = f.present[VaPos] = f.present[VaNrm] = f.present[VaTex0 + 2] = true;
        f.format[VaPos] = {0, static_cast<std::uint8_t>(CompType::S16), 8};
        f.format[VaNrm] = {0, static_cast<std::uint8_t>(CompType::S8), 0};
        f.format[VaTex0 + 2] = {0, static_cast<std::uint8_t>(CompType::U8), 4};
        Check("a record is 1 + 4 + 3 + 1 bytes", PackedVertexSize(f) == 9);
        std::vector<std::uint8_t> src = {6};
        PutU16(src, 0x0180);  // 1.5
        PutU16(src, 0xFF00);  // -1.0
        src.insert(src.end(), {0x40, 0xC0, 0x20});  // 1.0, -1.0, 0.5
        src.push_back(0x28);  // 2.5 with 4 fractional bits
        std::vector<float> out(kVertexFloats);
        Check("converts", ConvertVertices(f, src.data(), src.size(), 1, out.data()) == 1);
        Check("an XY position, z 0", Near(out[0], 1.5f) && Near(out[1], -1.0f) && Near(out[2], 0.0f));
        Check("an s8 normal at 1/64 whatever the table says", Near(out[3], 1.0f) && Near(out[4], -1.0f) && Near(out[5], 0.5f));
        Check("an S-only coordinate 2, t 0", Near(out[14 + 4], 2.5f) && Near(out[14 + 5], 0.0f));
        Check("the matrix index", Near(out[30], 6.0f));
    }

    std::printf("colour formats\n");
    {
        const struct {
            ColorType type;
            std::vector<std::uint8_t> bytes;
            float r, g, b, a;
            const char* what;
        } cases[] = {
            {ColorType::RGB565, {0xF8, 0x1F}, 1.0f, 0.0f, 1.0f, 1.0f, "RGB565: red and blue, opaque"},
            {ColorType::RGB8, {10, 20, 30}, 10 / 255.0f, 20 / 255.0f, 30 / 255.0f, 1.0f, "RGB8"},
            {ColorType::RGBX8, {10, 20, 30, 0}, 10 / 255.0f, 20 / 255.0f, 30 / 255.0f, 1.0f, "RGBX8 ignores its fourth byte"},
            {ColorType::RGBA4, {0xF0, 0x0F}, 1.0f, 0.0f, 0.0f, 1.0f, "RGBA4"},
            {ColorType::RGBA6, {0xFC, 0x00, 0x3F}, 1.0f, 0.0f, 0.0f, 1.0f, "RGBA6"},
            {ColorType::RGBA8, {1, 2, 3, 4}, 1 / 255.0f, 2 / 255.0f, 3 / 255.0f, 4 / 255.0f, "RGBA8"},
        };
        for (const auto& c : cases) {
            VertexFormat f;
            f.present[VaClr1] = true;
            f.format[VaClr1] = {1, static_cast<std::uint8_t>(c.type), 0};
            std::vector<float> out(kVertexFloats);
            const bool converted = ConvertVertices(f, c.bytes.data(), c.bytes.size(), 1, out.data()) == 1;
            Check(c.what, converted && Near(out[10], c.r) && Near(out[11], c.g) && Near(out[12], c.b) && Near(out[13], c.a));
        }
    }

    std::printf("NBT and refusals\n");
    {
        VertexFormat f;
        f.present[VaNrm] = true;
        f.format[VaNrm] = {1, static_cast<std::uint8_t>(CompType::S16), 0};
        Check("an s16 NBT normal is nine components", PackedVertexSize(f) == 18);
        std::vector<std::uint8_t> src;
        PutU16(src, 0x4000);
        for (int i = 0; i < 8; ++i) {
            PutU16(src, 0x1234);
        }
        std::vector<float> out(2 * kVertexFloats);
        Check("the normal is the first three, at 1/16384", ConvertVertices(f, src.data(), src.size(), 1, out.data()) == 1 &&
                                                            Near(out[3], 1.0f));
        Check("a short buffer converts what it holds", ConvertVertices(f, src.data(), src.size(), 2, out.data()) == 1);
        VertexFormat u;
        u.present[VaNrm] = true;
        u.format[VaNrm] = {0, static_cast<std::uint8_t>(CompType::U8), 0};
        const std::uint8_t u8n[3] = {128, 64, 0};
        Check("a u8 normal at 1/128", ConvertVertices(u, u8n, 3, 1, out.data()) == 1 && Near(out[3], 1.0f) &&
                                         Near(out[4], 0.5f));
        u.format[VaNrm].type = static_cast<std::uint8_t>(CompType::U16);
        const std::uint8_t u16n[6] = {0x80, 0x00, 0x40, 0x00, 0, 0};
        Check("a u16 normal at 1/32768", ConvertVertices(u, u16n, 6, 1, out.data()) == 1 && Near(out[3], 1.0f) &&
                                            Near(out[4], 0.5f));
        f.format[VaNrm].type = 7;
        Check("an unknown component type is no format", PackedVertexSize(f) == 0);
    }

    std::printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
