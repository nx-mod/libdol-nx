// nw4r::ut::CharWriter::PrintGlyph: one character of text, every frame.
//
// A glyph is a textured quad: its cell in the font's sheet as texture
// coordinates, its size scaled, the writer's four vertex colours. Translated,
// each one was a function call and twenty gather-pipe stores through the
// checked path - and a menu prints hundreds (Bomberman Blast's Characters menu
// spends 3-4% of its frame here). Here the quad goes to the FIFO as one write.
//
// Logic follows the ogws decompilation (doldecomp/ogws, CC0: ut_CharWriter.cpp).
// Offsets read off Bomberman Blast's (nw4r of 2007-11) and New Super Mario Bros.
// Wii's (2009) code, which agree field for field: the writer's vertex colours
// at 8/12/16/20, its scale at 36/40, its filter at 56/60; the glyph's texture
// at 0, format at 8, sheet size at 12/14, cell at 16/18, width at 5, height at
// 7. The texture last loaded is a static of the game's (LoadingTexture: slot,
// texture, filter), found by wiinx-find-globals.
//
// What the original calls in the game - GXInitTexObj, GXInitTexObjLOD and
// GXLoadTexObj when the texture changes, GXBegin always - this calls in the
// game too, by name: they are whatever this game binds them to (its own code,
// or natives written for its GX build), and GXBegin's dirty-state flush stays
// the SDK's.
#pragma GCC optimize("fp-contract=off")

#include "wiinx/accel/nw4r.hpp"
#include "wiinx/core/guest.hpp"

#include <cstring>

namespace wiinx::nw4r::ut {
namespace {

struct Writer {
    static constexpr u32 kColorLeftUp = 0x08, kColorRightUp = 0x0C, kColorLeftDown = 0x10, kColorRightDown = 0x14;
    static constexpr u32 kScaleX = 0x24, kScaleY = 0x28;
    static constexpr u32 kFilterSmall = 0x38, kFilterLarge = 0x3C;
};
struct Glyph {
    static constexpr u32 kTexture = 0x00, kGlyphWidth = 0x05, kHeight = 0x07, kFormat = 0x08;
    static constexpr u32 kTexWidth = 0x0C, kTexHeight = 0x0E, kCellX = 0x10, kCellY = 0x12;
};

constexpr const char* kPrintGlyph = "nw4r::ut::CharWriter::PrintGlyph";

struct Called {
    const Bound* self = bound(kPrintGlyph);
    const Bound* init_tex_obj = bound("GXInitTexObj");
    const Bound* init_tex_obj_lod = bound("GXInitTexObjLOD");
    const Bound* load_tex_obj = bound("GXLoadTexObj");
    const Bound* begin = bound("GXBegin");
};
const Called& called() {
    static const Called c;
    return c;
}

// divwu, as the original divides: a zero divisor (a glyph sheet of no size)
// gives 0 here, where the host would trap
u16 tex_coord(u32 cell, u32 size) { return size == 0 ? 0 : static_cast<u16>((cell << 15) / size); }

void put_f32(u8*& at, f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, 4);
    at[0] = static_cast<u8>(bits >> 24);
    at[1] = static_cast<u8>(bits >> 16);
    at[2] = static_cast<u8>(bits >> 8);
    at[3] = static_cast<u8>(bits);
    at += 4;
}
void put_u32(u8*& at, u32 value) {
    at[0] = static_cast<u8>(value >> 24);
    at[1] = static_cast<u8>(value >> 16);
    at[2] = static_cast<u8>(value >> 8);
    at[3] = static_cast<u8>(value);
    at += 4;
}
void put_u16(u8*& at, u16 value) {
    at[0] = static_cast<u8>(value >> 8);
    at[1] = static_cast<u8>(value);
    at += 2;
}

// CharWriter::LoadTexture(glyph, GX_TEXMAP0)
void load_texture(Cpu* cpu, GuestAddr writer, GuestAddr glyph) {
    const Host& h = host();
    const Called& c = called();
    const GuestAddr loading = h.global ? h.global("ut.CharWriter_loadingTexture") : 0;
    const u32 texture = load<u32>(glyph + Glyph::kTexture);
    const u32 small = load<u32>(writer + Writer::kFilterSmall);
    const u32 large = load<u32>(writer + Writer::kFilterLarge);
    if (loading != 0 && load<u32>(loading) == 0 && load<u32>(loading + 4) == texture &&
        load<u32>(loading + 8) == small && load<u32>(loading + 12) == large) {
        return;  // already loaded, as the original's cache says
    }
    // the GXTexObj on the guest stack, in a frame of its own: GXLoadTexObj may
    // call the game's region callback, which uses the stack below r1
    const GuestAddr sp = h.gpr(cpu, 1);
    const GuestAddr frame = (sp - 0x40) & ~0xFu;
    store<u32>(frame, sp);  // back chain
    const GuestAddr obj = frame + 0x10;
    h.set_gpr(cpu, 1, frame);
    h.set_gpr(cpu, 3, obj);
    h.set_gpr(cpu, 4, texture);
    h.set_gpr(cpu, 5, load<u16>(glyph + Glyph::kTexWidth));
    h.set_gpr(cpu, 6, load<u16>(glyph + Glyph::kTexHeight));
    h.set_gpr(cpu, 7, load<u32>(glyph + Glyph::kFormat));
    h.set_gpr(cpu, 8, 0);  // GX_CLAMP
    h.set_gpr(cpu, 9, 0);  // GX_CLAMP
    h.set_gpr(cpu, 10, 0); // no mipmaps
    call_bound(cpu, c.init_tex_obj, c.self);
    h.set_gpr(cpu, 3, obj);
    h.set_gpr(cpu, 4, small);
    h.set_gpr(cpu, 5, large);
    if (h.set_fpr) {
        h.set_fpr(cpu, 1, 0.0);
        h.set_fpr(cpu, 2, 0.0);
        h.set_fpr(cpu, 3, 0.0);
    }
    h.set_gpr(cpu, 6, 0);  // no bias clamp
    h.set_gpr(cpu, 7, 0);  // no edge LOD
    h.set_gpr(cpu, 8, 0);  // GX_ANISO_1
    call_bound(cpu, c.init_tex_obj_lod, c.self);
    h.set_gpr(cpu, 3, obj);
    h.set_gpr(cpu, 4, 0);  // GX_TEXMAP0
    call_bound(cpu, c.load_tex_obj, c.self);
    h.set_gpr(cpu, 1, sp);
    if (loading != 0) {
        store<u32>(loading, 0);
        store<u32>(loading + 4, texture);
        store<u32>(loading + 8, small);
        store<u32>(loading + 12, large);
    }
}

void print_glyph(Cpu* cpu) {
    const Host& h = host();
    const Called& c = called();
    const GuestAddr writer = h.gpr(cpu, 3);
    const GuestAddr glyph = h.gpr(cpu, 4);
    const f32 x = static_cast<f32>(h.fpr(cpu, 1));
    const f32 y = static_cast<f32>(h.fpr(cpu, 2));
    const f32 z = static_cast<f32>(h.fpr(cpu, 3));

    const u32 glyphWidth = load<u8>(glyph + Glyph::kGlyphWidth);
    const u32 height = load<u8>(glyph + Glyph::kHeight);
    const u32 cellX = load<u16>(glyph + Glyph::kCellX);
    const u32 cellY = load<u16>(glyph + Glyph::kCellY);
    const u32 texWidth = load<u16>(glyph + Glyph::kTexWidth);
    const u32 texHeight = load<u16>(glyph + Glyph::kTexHeight);

    // the original's single-precision operations, in its order
    const f32 x2 = x + static_cast<f32>(glyphWidth) * load<f32>(writer + Writer::kScaleX);
    const f32 y2 = y + static_cast<f32>(height) * load<f32>(writer + Writer::kScaleY);
    const u16 texLeft = tex_coord(cellX, texWidth);
    const u16 texTop = tex_coord(cellY, texHeight);
    const u16 texRight = tex_coord(cellX + glyphWidth, texWidth);
    const u16 texBottom = tex_coord(cellY + height, texHeight);

    load_texture(cpu, writer, glyph);

    // GXBegin(GX_QUADS, GX_VTXFMT0, 4), the game's own
    h.set_gpr(cpu, 3, 0x80);
    h.set_gpr(cpu, 4, 0);
    h.set_gpr(cpu, 5, 4);
    call_bound(cpu, c.begin, c.self);

    // four vertices: position (f32 x3), colour (RGBA8), texture coordinate (u16 x2)
    u8 quad[4 * 20];
    u8* at = quad;
    const auto vertex = [&](f32 vx, f32 vy, u32 colorOffset, u16 s, u16 t) {
        put_f32(at, vx);
        put_f32(at, vy);
        put_f32(at, z);
        put_u32(at, load<u32>(writer + colorOffset));
        put_u16(at, s);
        put_u16(at, t);
    };
    vertex(x, y, Writer::kColorLeftUp, texLeft, texTop);
    vertex(x2, y, Writer::kColorRightUp, texRight, texTop);
    vertex(x2, y2, Writer::kColorRightDown, texRight, texBottom);
    vertex(x, y2, Writer::kColorLeftDown, texLeft, texBottom);
    if (h.gx_write) {
        h.gx_write(quad, sizeof(quad));
    }
}

}  // namespace

extern const Native kCharWriterNatives[] = {
    WIINX_NATIVE("nw4r::ut::CharWriter::PrintGlyph", kUt_Any, print_glyph),
};
extern const std::size_t kCharWriterNativeCount = std::size(kCharWriterNatives);

}  // namespace wiinx::nw4r::ut
