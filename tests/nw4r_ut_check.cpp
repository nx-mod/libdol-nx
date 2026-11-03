// nw4r::ut::CharWriter::PrintGlyph on a fake writer and glyph: the quad's
// position, colours and texture coordinates in the FIFO, the game's own GX
// functions called where the original calls them, and the texture cache.
#include "wiinx/accel/nw4r.hpp"
#include "wiinx/core/guest.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace wiinx;

namespace {
std::vector<u8> g_memory(0x10000, 0);
std::vector<u8> g_fifo;
u32 g_gpr[32];
f64 g_fpr[32];

struct Call {
    GuestAddr target, resume;
    u32 r1, r3, r4, r5;
};
std::vector<Call> g_calls;

constexpr GuestAddr kInitTexObj = 0x9000, kInitTexObjLod = 0x9100, kLoadTexObj = 0x9200, kBegin = 0x9300;
constexpr GuestAddr kPrintGlyph = 0x9400, kLoading = 0x3000, kWriter = 0x1000, kGlyph = 0x2000;
constexpr DirectCall kDirect[] = {{kInitTexObj, 0xA000}, {kInitTexObjLod, 0xA004}, {kLoadTexObj, 0xA008},
                                  {kBegin, 0xA00C}};

void call_from(Cpu*, GuestAddr target, GuestAddr resume) {
    g_calls.push_back({target, resume, g_gpr[1], g_gpr[3], g_gpr[4], g_gpr[5]});
    if (target == kBegin) {  // the game's GXBegin: its header into the FIFO
        g_fifo.push_back(static_cast<u8>(g_gpr[3] | g_gpr[4]));
        g_fifo.push_back(0);
        g_fifo.push_back(static_cast<u8>(g_gpr[5]));
    }
}

u32 bits(f32 v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    return b;
}
u32 be32(size_t at) {
    return u32(g_fifo[at]) << 24 | u32(g_fifo[at + 1]) << 16 | u32(g_fifo[at + 2]) << 8 | g_fifo[at + 3];
}
u16 be16(size_t at) { return static_cast<u16>(g_fifo[at] << 8 | g_fifo[at + 1]); }
}  // namespace

int main() {
    Host h;
    h.memory = g_memory.data();
    h.gpr = [](Cpu*, int i) { return g_gpr[i]; };
    h.set_gpr = [](Cpu*, int i, u32 v) { g_gpr[i] = v; };
    h.fpr = [](Cpu*, int i) { return g_fpr[i]; };
    h.set_fpr = [](Cpu*, int i, f64 v) { g_fpr[i] = v; };
    h.call_from = call_from;
    h.gx_write = [](const u8* data, u32 size) { g_fifo.insert(g_fifo.end(), data, data + size); };
    h.global = [](const char* name) -> GuestAddr {
        return std::strcmp(name, "ut.CharWriter_loadingTexture") == 0 ? kLoading : 0;
    };
    set_host(h);
    add_natives(nw4r::natives());
    set_bound("nw4r::ut::CharWriter::PrintGlyph", kPrintGlyph, {}, kDirect);
    set_bound("GXInitTexObj", kInitTexObj, {});
    set_bound("GXInitTexObjLOD", kInitTexObjLod, {});
    set_bound("GXLoadTexObj", kLoadTexObj, {});
    set_bound("GXBegin", kBegin, {});
    const Native* native = find_native("nw4r::ut::CharWriter::PrintGlyph", "nw4r.ut@any");
    assert(native != nullptr);

    // a writer at scale 2 x 1.5, four colours, linear filters (1, 1)
    store<u32>(kWriter + 0x08, 0x11111111);
    store<u32>(kWriter + 0x0C, 0x22222222);
    store<u32>(kWriter + 0x10, 0x33333333);
    store<u32>(kWriter + 0x14, 0x44444444);
    store<f32>(kWriter + 0x24, 2.0f);
    store<f32>(kWriter + 0x28, 1.5f);
    store<u32>(kWriter + 0x38, 1);
    store<u32>(kWriter + 0x3C, 1);
    // a glyph 10 wide, 12 high, at cell 30,24 of a 256x128 I4 sheet
    store<u32>(kGlyph + 0x00, 0x80400000);
    store<u8>(kGlyph + 0x05, 10);
    store<u8>(kGlyph + 0x07, 12);
    store<u32>(kGlyph + 0x08, 0);
    store<u16>(kGlyph + 0x0C, 256);
    store<u16>(kGlyph + 0x0E, 128);
    store<u16>(kGlyph + 0x10, 30);
    store<u16>(kGlyph + 0x12, 24);
    store<u32>(kLoading + 4, 0xFFFFFFFF);  // nothing loaded yet

    const auto print = [&] {
        g_gpr[1] = 0x8000;
        g_gpr[3] = kWriter;
        g_gpr[4] = kGlyph;
        g_fpr[1] = 100.0;
        g_fpr[2] = 50.0;
        g_fpr[3] = 0.0;
        native->entry(nullptr);
    };
    print();

    // the texture loaded through the game's own GX functions, from the
    // original's call sites, with the object below the stack pointer
    assert(g_calls.size() == 4);
    assert(g_calls[0].target == kInitTexObj && g_calls[0].resume == 0xA000);
    assert(g_calls[0].r1 < 0x8000 && g_calls[0].r3 > g_calls[0].r1 && g_calls[0].r4 == 0x80400000 &&
           g_calls[0].r5 == 256);
    assert(g_calls[1].target == kInitTexObjLod && g_calls[1].resume == 0xA004 && g_calls[1].r4 == 1);
    assert(g_calls[2].target == kLoadTexObj && g_calls[2].resume == 0xA008 && g_calls[2].r4 == 0);
    assert(g_calls[3].target == kBegin && g_calls[3].resume == 0xA00C && g_calls[3].r3 == 0x80 &&
           g_calls[3].r5 == 4);
    assert(g_gpr[1] == 0x8000);  // the stack pointer given back
    assert(load<u32>(kLoading + 4) == 0x80400000 && load<u32>(kLoading + 8) == 1);

    // the quad: GXBegin's header, then four vertices of 20 bytes
    assert(g_fifo.size() == 3 + 80);
    const f32 x2 = 100.0f + 10.0f * 2.0f, y2 = 50.0f + 12.0f * 1.5f;
    const u16 s0 = (30u << 15) / 256, t0 = (24u << 15) / 128, s1 = (40u << 15) / 256, t1 = (36u << 15) / 128;
    const struct {
        f32 x, y;
        u32 color;
        u16 s, t;
    } want[] = {{100, 50, 0x11111111, s0, t0}, {x2, 50, 0x22222222, s1, t0},
                {x2, y2, 0x44444444, s1, t1},  {100, y2, 0x33333333, s0, t1}};
    for (int v = 0; v < 4; ++v) {
        const size_t at = 3 + v * 20;
        assert(be32(at) == bits(want[v].x) && be32(at + 4) == bits(want[v].y) && be32(at + 8) == bits(0.0f));
        assert(be32(at + 12) == want[v].color);
        assert(be16(at + 16) == want[v].s && be16(at + 18) == want[v].t);
    }

    // the same texture again: no reload, only GXBegin
    g_calls.clear();
    g_fifo.clear();
    print();
    assert(g_calls.size() == 1 && g_calls[0].target == kBegin);
    assert(g_fifo.size() == 3 + 80);
    std::printf("nw4r.ut PrintGlyph ok\n");
}
