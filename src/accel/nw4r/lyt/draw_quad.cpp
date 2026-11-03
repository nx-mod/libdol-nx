// nw4r::lyt::detail::DrawQuad: the quad every picture, window and bounding
// pane draws.
//
// Four vertices: position (x, y), the pane's colour if it has one, and a
// texture coordinate per texture. Translated, each was one gather-pipe store
// per value through the checked path; here the quad is one FIFO write, after
// the game's own GXBegin (its dirty-state flush is the SDK's, called where the
// original calls it).
//
// Logic follows the ogws decompilation (doldecomp/ogws, CC0: lyt_common.cpp).
// Its 2007 build adds the height to y; the code of 2007-11 (Bomberman Blast),
// 2008-03 (Mario Kart Wii) and 2009-04 (New Super Mario Bros. Wii) subtracts
// it. Corners in the original's order: left top, right top, right bottom,
// left bottom - colours 0, 1, 3, 2 and each texture's coordinates at +0, +8,
// +24, +16 of its 32 bytes, read off Bomberman Blast's code.
#pragma GCC optimize("fp-contract=off")

#include "wiinx/accel/nw4r.hpp"
#include "wiinx/core/guest.hpp"

#include <cstring>

namespace wiinx::nw4r::lyt {
namespace {

constexpr const char* kDrawQuad =
    "nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, "
    "const nw4r::math::VEC2[4]*, const nw4r::ut::Color*)";
constexpr const char* kDrawQuadAlpha =
    "nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, "
    "const nw4r::math::VEC2[4]*, const nw4r::ut::Color*, unsigned char)";

struct Corner {
    u32 color;     // index into the colours
    u32 texCoord;  // offset into each texture's four coordinates
    bool right, bottom;
};
constexpr Corner kCorners[4] = {{0, 0, false, false}, {1, 8, true, false}, {3, 24, true, true}, {2, 16, false, true}};

void put_u32(u8*& at, u32 v) {
    at[0] = static_cast<u8>(v >> 24);
    at[1] = static_cast<u8>(v >> 16);
    at[2] = static_cast<u8>(v >> 8);
    at[3] = static_cast<u8>(v);
    at += 4;
}
void put_f32(u8*& at, f32 v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    put_u32(at, b);
}

// `colors` as host values (four RGBA words) or null for a quad without
template <bool kSubtractHeight>
void draw_quad(Cpu* cpu, GuestAddr base, GuestAddr size, u32 count, GuestAddr coords, const u32* colors) {
    const Host& h = host();
    static const Bound* const self = bound(kDrawQuad);
    static const Bound* const begin = bound("GXBegin");
    h.set_gpr(cpu, 3, 0x80);  // GX_QUADS
    h.set_gpr(cpu, 4, 0);     // GX_VTXFMT0
    h.set_gpr(cpu, 5, 4);
    call_bound(cpu, begin, self);

    const f32 x = load<f32>(base), y = load<f32>(base + 4);
    const f32 width = load<f32>(size), height = load<f32>(size + 4);
    const f32 right = x + width;
    const f32 bottom = kSubtractHeight ? y - height : y + height;
    // (GX has eight texture coordinates: a count past that is the game's bug,
    // drawn as far as eight)
    const u32 textures = count < 8 ? count : 8;
    u8 quad[4 * (8 + 4 + 8 * 8)];
    u8* at = quad;
    for (const Corner& c : kCorners) {
        put_f32(at, c.right ? right : x);
        put_f32(at, c.bottom ? bottom : y);
        if (colors != nullptr) {
            put_u32(at, colors[c.color]);
        }
        for (u32 i = 0; i < textures; ++i) {
            put_f32(at, load<f32>(coords + i * 32 + c.texCoord));
            put_f32(at, load<f32>(coords + i * 32 + c.texCoord + 4));
        }
    }
    if (h.gx_write) {
        h.gx_write(quad, static_cast<u32>(at - quad));
    }
}

template <bool kSubtractHeight>
void entry_draw_quad(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr colorsAt = h.gpr(cpu, 7);
    u32 colors[4];
    for (u32 i = 0; i < 4 && colorsAt != 0; ++i) {
        colors[i] = load<u32>(colorsAt + i * 4);
    }
    draw_quad<kSubtractHeight>(cpu, h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5) & 0xFF, h.gpr(cpu, 6),
                               colorsAt != 0 ? colors : nullptr);
}

// the colours' alpha scaled by the pane's, as MultipleAlpha does: a * alpha /
// 255, truncated, and left alone at 255
template <bool kSubtractHeight>
void entry_draw_quad_alpha(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr colorsAt = h.gpr(cpu, 7);
    const u32 alpha = h.gpr(cpu, 8) & 0xFF;
    u32 colors[4];
    for (u32 i = 0; i < 4 && colorsAt != 0; ++i) {
        u32 c = load<u32>(colorsAt + i * 4);
        if (alpha != 255) {
            c = (c & 0xFFFFFF00u) | (((c & 0xFF) * alpha / 255) & 0xFF);
        }
        colors[i] = c;
    }
    draw_quad<kSubtractHeight>(cpu, h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5) & 0xFF, h.gpr(cpu, 6),
                               colorsAt != 0 ? colors : nullptr);
}

}  // namespace

extern const Native kDrawQuadNatives[] = {
    WIINX_NATIVE("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, const nw4r::math::VEC2[4]*, const nw4r::ut::Color*)", kLyt_2007_06, entry_draw_quad<false>),
    WIINX_NATIVE("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, const nw4r::math::VEC2[4]*, const nw4r::ut::Color*)", kLyt_2008_03, entry_draw_quad<true>),
    WIINX_NATIVE("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, const nw4r::math::VEC2[4]*, const nw4r::ut::Color*)", kLyt_2009_04, entry_draw_quad<true>),
    WIINX_NATIVE("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, const nw4r::math::VEC2[4]*, const nw4r::ut::Color*, unsigned char)", kLyt_2007_06, entry_draw_quad_alpha<false>),
    WIINX_NATIVE("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, const nw4r::math::VEC2[4]*, const nw4r::ut::Color*, unsigned char)", kLyt_2008_03, entry_draw_quad_alpha<true>),
    WIINX_NATIVE("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, const nw4r::math::VEC2[4]*, const nw4r::ut::Color*, unsigned char)", kLyt_2009_04, entry_draw_quad_alpha<true>),
};
extern const std::size_t kDrawQuadNativeCount = std::size(kDrawQuadNatives);

}  // namespace wiinx::nw4r::lyt
