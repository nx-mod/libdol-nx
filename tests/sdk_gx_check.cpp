// The GX texture-object natives against GXTexObjs worked out by hand from Mario
// Kart Wii's own instructions (doldecomp/mkw lib/rvl/gx/gxTexture.c): each
// expected word below was computed from the original's rlwimi masks and
// constants, not from the native, so a slip in either shows here.
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace wiinx;

namespace {
std::vector<u8> g_memory(0x1000, 0);
int g_failures = 0;
std::vector<u8> g_fifo;  // what the natives sent down the command stream
constexpr GuestAddr kGxData = 0x400;

void check(const char* what, u32 got, u32 want) {
    if (got == want) {
        std::printf("  [PASS] %s: 0x%08X\n", what, got);
    } else {
        std::printf("  [FAIL] %s: 0x%08X, want 0x%08X\n", what, got, want);
        ++g_failures;
    }
}

void fill(GuestAddr obj, u8 value) {
    for (u32 i = 0; i < 0x20; ++i) store<u8>(obj + i, value);
}
}  // namespace

int main() {
    Host h;
    h.memory = g_memory.data();
    h.gx_write = [](const u8* data, u32 size) { g_fifo.insert(g_fifo.end(), data, data + size); };
    h.gx_data = []() -> GuestAddr { return kGxData; };
    set_host(h);
    constexpr GuestAddr kObj = 0x100, kObj2 = 0x200, kObj3 = 0x300;

    std::printf("GXInitTexObj: CMPR 64x32, repeat/clamp, mipmapped\n");
    fill(kObj, 0xAB);  // (it clears the whole object first)
    sdk::gx::init_tex_obj(kObj, 0x80123460, 64, 32, 14, 1, 0, 1);
    check("mode0 (wrap 1/0, mag linear, min LIN_MIP_LIN)", load<u32>(kObj + 0x00), 0x000000D1);
    check("mode1 (max LOD 6 x16)", load<u32>(kObj + 0x04), 0x00006000);
    check("image0 (63, 31, format 14)", load<u32>(kObj + 0x08), 0x00E07C3F);
    check("image3 (address >> 5, 24 bits)", load<u32>(kObj + 0x0C), 0x000091A3);
    check("format", load<u32>(kObj + 0x14), 14);
    check("tiles (8 x 4 of 8x8)", load<u16>(kObj + 0x1C), 32);
    check("load class (CMPR)", load<u8>(kObj + 0x1E), 0);
    check("flags (mipmapped, not paletted)", load<u8>(kObj + 0x1F), 3);

    std::printf("GXInitTexObj: RGBA8 3x5, mirror/mirror, no mips\n");
    sdk::gx::init_tex_obj(kObj2, 0x00001000, 3, 5, 6, 2, 2, 0);
    check("mode0 (wrap 2/2, mag linear, min LINEAR)", load<u32>(kObj2 + 0x00), 0x0000009A);
    check("mode1", load<u32>(kObj2 + 0x04), 0);
    check("image0 (2, 4, format 6)", load<u32>(kObj2 + 0x08), 0x00601002);
    check("tiles (1 x 2 of 4x4)", load<u16>(kObj2 + 0x1C), 2);
    check("load class (RGBA8)", load<u8>(kObj2 + 0x1E), 3);
    check("flags", load<u8>(kObj2 + 0x1F), 2);

    std::printf("GXInitTexObjCI: C8 16x16, mipmapped, palette 5\n");
    sdk::gx::init_tex_obj_ci(kObj3, 0x80200000, 16, 16, 9, 0, 0, 1, 5);
    check("mode0 (paletted mips: hw min 5)", load<u32>(kObj3 + 0x00), 0x000000B0);
    check("mode1 (max LOD 4 x16)", load<u32>(kObj3 + 0x04), 0x00004000);
    check("image0 (15, 15, format 9)", load<u32>(kObj3 + 0x08), 0x00903C0F);
    check("tlut", load<u32>(kObj3 + 0x18), 5);
    check("tiles (2 x 4 of 8x4)", load<u16>(kObj3 + 0x1C), 8);
    check("flags (mipmapped, paletted)", load<u8>(kObj3 + 0x1F), 1);

    std::printf("GXInitTexObjLOD: on the RGBA8 object\n");
    sdk::gx::init_tex_obj_lod(kObj2, 5, 0, 0.5, 12.0, -1.5, 1, 0, 2);
    check("mode0 (bias -48, mag near, min hw 6, edge, aniso 2, clamp)", load<u32>(kObj2 + 0x00), 0x0031A1CA);
    check("mode1 (min 8, max 10 x16)", load<u32>(kObj2 + 0x04), 0x0000A008);

    std::printf("GXInitTexObjLOD: bias past 4, NaN min LOD, linear mag\n");
    sdk::gx::init_tex_obj_lod(kObj, 1, 1, std::nan(""), 2.0, 5.0, 0, 1, 0);
    // bias 3.99f x 32 = 127.68 -> 127: 0xFE00; mag bit 4; min LINEAR hw 4;
    // edge 1 -> bit 8 clear; wrap 1 kept from before
    check("mode0", load<u32>(kObj + 0x00), 0x0000FE91);
    check("mode1 (min 0, max 32)", load<u32>(kObj + 0x04), 0x00002000);

    std::printf("GXLoadTexObjPreLoaded: the RGBA8 object into map 5\n");
    store<u32>(0x500, 0x11111111);  // the region's two TMEM words
    store<u32>(0x504, 0x22222222);
    store<u32>(kGxData + 0x5FC, 0x80);
    store<u16>(kGxData + 0x002, 1);
    sdk::gx::load_tex_obj_preloaded(nullptr, kObj2, 0x500, 5);
    // six BP writes, map 5's registers (0xA1, A5, A9, AD, B1, B5), in the original's order
    const u8 want[] = {0x61, 0xA1, 0x31, 0xA1, 0xCA, 0x61, 0xA5, 0x00, 0xA0, 0x08, 0x61, 0xA9, 0x60, 0x10, 0x02,
                       0x61, 0xAD, 0x11, 0x11, 0x11, 0x61, 0xB1, 0x22, 0x22, 0x22, 0x61, 0xB5, 0x00, 0x00, 0x80};
    check("command bytes", g_fifo.size() == sizeof(want) && std::equal(g_fifo.begin(), g_fifo.end(), want), 1);
    check("object mode0 keeps its register", load<u32>(kObj2 + 0x00), 0xA131A1CA);
    check("region keeps its register", load<u32>(0x500), 0xAD111111);
    check("gxData image0[5]", load<u32>(kGxData + 0x564 + 20), 0xA9601002);
    check("gxData mode0[5]", load<u32>(kGxData + 0x584 + 20), 0xA131A1CA);
    check("dirty state gains bit 0", load<u32>(kGxData + 0x5FC), 0x81);
    check("bpSentNot cleared", load<u16>(kGxData + 0x002), 0);

    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
