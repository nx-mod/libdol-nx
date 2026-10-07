// GX's command stream: display lists built here byte by byte, run against a
// small guest memory, and what reached the state and the draw callback checked.
#include "wiinx/format/gx/command.hpp"

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

struct List {
    std::vector<std::uint8_t> bytes;
    void U8(std::uint32_t v) { bytes.push_back(static_cast<std::uint8_t>(v)); }
    void U16(std::uint32_t v) { U8(v >> 8), U8(v); }
    void U32(std::uint32_t v) { U16(v >> 16), U16(v); }
    void F32(float f) {
        std::uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        U32(bits);
    }
    void CP(std::uint8_t reg, std::uint32_t value) { U8(0x08), U8(reg), U32(value); }
    void BP(std::uint32_t value) { U8(0x61), U32(value); }
};

// guest memory: one buffer at 0x80001000 (0x00001000 physically)
std::vector<std::uint8_t> gMemory(0x1000);
constexpr std::uint32_t kBase = 0x1000;

const std::uint8_t* Memory(std::uint32_t address, std::uint32_t size) {
    address &= 0x3FFFFFFF;
    if (address < kBase || address - kBase + size > gMemory.size()) {
        return nullptr;
    }
    return gMemory.data() + (address - kBase);
}

float FloatAt(const std::vector<std::uint8_t>& block, std::size_t offset) {
    float v;
    std::memcpy(&v, block.data() + offset, sizeof(v));
    return v;
}
}  // namespace

int main() {
    State state;
    std::vector<Draw> draws;
    std::vector<std::vector<float>> converted;
    CommandProcessor cp(state, Memory, [&](const Draw& d) {
        draws.push_back(d);
        std::vector<float> out(static_cast<std::size_t>(d.count) * kVertexFloats);
        ConvertVertices(d.format, d.records, d.bytes, d.count, out.data());
        converted.push_back(out);
    });

    std::printf("direct: a triangle of float positions and RGBA8 colours\n");
    {
        List l;
        l.CP(0x50, 1u << 9 | 1u << 13);           // POS direct, CLR0 direct
        l.CP(0x60, 0);
        l.CP(0x70, 1u | 4u << 1 | 1u << 13 | 5u << 14);  // table 0: POS XYZ F32, CLR0 RGBA RGBA8
        l.BP(0x41u << 24 | 1u | 4u << 8 | 5u << 5 | 1u << 3);  // a blend mode on the way
        l.U8(0x00);  // NOP
        l.U8(0x90);  // GX_TRIANGLES, table 0
        l.U16(3);
        for (int v = 0; v < 3; ++v) {
            l.F32(static_cast<float>(v)), l.F32(1.0f), l.F32(-1.0f);
            l.U8(255), l.U8(0), l.U8(v * 100), l.U8(255);
        }
        const std::size_t ran = cp.Run(l.bytes.data(), l.bytes.size());
        Check("the whole list ran", ran == l.bytes.size());
        Check("one triangle draw", draws.size() == 1 && draws[0].primitive == 0x90 && draws[0].count == 3);
        Check("its records are the stream's, 16 bytes each", draws[0].bytes == 48);
        Check("vertex 2 converts", Near(converted[0][2 * kVertexFloats], 2.0f) &&
                                       Near(converted[0][2 * kVertexFloats + 8], 200 / 255.0f));
        Check("the BP write reached the state", state.Pixel().blendMode == BlendMode::Blend);
        Check("the descriptor reached the shaders' configuration", state.Vertex().hasColor0 && !state.Vertex().hasNormal);
    }

    std::printf("indexed: positions from an array by u8 index, texture coordinates by u16\n");
    {
        draws.clear(), converted.clear();
        // positions: s16 XYZ, 6 fractional bits, stride 8 at 0x1100
        const std::uint32_t posArray = 0x80001100;
        for (int i = 0; i < 4; ++i) {
            const std::int16_t xyz[3] = {static_cast<std::int16_t>(64 * i), 32, -64};
            for (int c = 0; c < 3; ++c) {
                gMemory[0x100 + i * 8 + c * 2] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(xyz[c]) >> 8);
                gMemory[0x100 + i * 8 + c * 2 + 1] = static_cast<std::uint8_t>(xyz[c]);
            }
        }
        // texture coordinate 0: f32 ST, stride 8 at 0x1200
        for (int i = 0; i < 300; i += 299) {
            const float st[2] = {0.25f * (i == 299 ? 3 : 1), 0.5f};
            for (int c = 0; c < 2; ++c) {
                std::uint32_t bits;
                std::memcpy(&bits, &st[c], 4);
                for (int b = 0; b < 4; ++b) {
                    gMemory[0x200 + (i % 64) * 8 + c * 4 + b] = static_cast<std::uint8_t>(bits >> (24 - 8 * b));
                }
            }
        }
        List l;
        l.CP(0x50, 2u << 9);                       // POS INDEX8
        l.CP(0x60, 3u);                            // TEX0 INDEX16
        l.CP(0x71, 1u | 3u << 1 | 6u << 4 | 1u << 21 | 4u << 22);  // table 1: POS XYZ S16 frac 6, TEX0 ST F32
        l.CP(0xA0, posArray), l.CP(0xB0, 8);
        l.CP(0xA4, 0x80001200), l.CP(0xB4, 8);
        l.U8(0x80 | 1);  // GX_QUADS, table 1
        l.U16(2);
        l.U8(3), l.U16(0);
        l.U8(1), l.U16(299 % 64);
        const std::size_t ran = cp.Run(l.bytes.data(), l.bytes.size());
        Check("the indexed list ran", ran == l.bytes.size());
        Check("records expanded to direct: 6 + 8 bytes each", draws.size() == 1 && draws[0].bytes == 28);
        Check("vertex 0 is array entry 3", Near(converted[0][0], 3.0f) && Near(converted[0][1], 0.5f) &&
                                               Near(converted[0][2], -1.0f));
        Check("vertex 1's coordinate came from its own index", Near(converted[0][kVertexFloats + 14], 0.75f));
    }

    std::printf("XF loads, direct and from an array; a called list\n");
    {
        List called;
        called.BP(0x40u << 24 | 0u);  // z test off
        std::memcpy(gMemory.data() + 0x400, called.bytes.data(), called.bytes.size());

        // a 3x4 matrix in memory for LOAD_INDX_A, array 12 at 0x1500, stride 48
        for (int i = 0; i < 12; ++i) {
            const float v = 10.0f + i;
            std::uint32_t bits;
            std::memcpy(&bits, &v, 4);
            for (int b = 0; b < 4; ++b) {
                gMemory[0x500 + 48 + i * 4 + b] = static_cast<std::uint8_t>(bits >> (24 - 8 * b));
            }
        }
        List l;
        l.U8(0x10), l.U32(11u << 16 | 0);  // XF: position matrix 0
        for (int i = 0; i < 12; ++i) {
            l.F32(static_cast<float>(i) + 0.5f);
        }
        l.CP(0xAC, 0x80001500), l.CP(0xBC, 48);
        l.U8(0x20), l.U32(1u << 16 | 11u << 12 | 12u);  // array 12 entry 1, 12 words, to XF 12 (position matrix 1)
        l.U8(0x40), l.U32(0x80001400), l.U32(static_cast<std::uint32_t>(called.bytes.size()));
        const std::size_t ran = cp.Run(l.bytes.data(), l.bytes.size());
        const std::vector<std::uint8_t> xf = state.XfUniforms();
        Check("the list ran", ran == l.bytes.size());
        Check("XF position matrix 0, row 2, column 3", Near(FloatAt(xf, 2 * 16 + 12), 11.5f));
        Check("LOAD_INDX_A put entry 1 into position matrix 1", Near(FloatAt(xf, 3 * 16), 10.0f) &&
                                                                  Near(FloatAt(xf, 5 * 16 + 12), 21.0f));
        Check("the called list's BP write", !state.Pixel().depthTest);
    }

    std::printf("refusals\n");
    {
        const std::uint8_t garbage[] = {0x00, 0x77, 0x00};
        Check("stops at a byte that is not a command", cp.Run(garbage, sizeof(garbage)) == 1);
        const std::uint8_t cut[] = {0x90, 0x00, 0x05, 0x00};
        Check("stops at a draw cut short", cp.Run(cut, sizeof(cut)) == 0);
    }

    std::printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
    return gFailures ? 1 : 0;
}
