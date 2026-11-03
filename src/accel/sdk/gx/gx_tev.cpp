// GX's material-setup setters, natively: the TEV combiner and order registers,
// the konstant colours, the channel control and the indirect-texture setup -
// what nw4r::lyt::Material::SetupGX writes.
//
// Every layout material sets up its TEV stages each time it is drawn
// (nw4r::lyt::Material::SetupGX calls each of these up to eight times), so a
// menu of a few hundred panes calls them thousands of times a frame. Each one
// is a register in gxData updated in place and sent as one BP write.
//
// Read off rvl.gx@2007-12-11 as Bomberman Blast ships it (the build Mario Kart
// Wii links). gxData: tevc[16] at 0x180, teva[16] at 0x1C0, tref[8] at 0x150
// (two stages each, the odd one in the upper half), each stage's texture map
// at 0x5A4, the stages using a texture coordinate as bits at 0x5E8. The colour
// channel table is the game's: {0, 1, 0, 1, 0, 1, 7, 5, 6}.
#include "gx_internal.hpp"
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"

#include <initializer_list>

namespace wiinx::sdk::gx {
namespace {

using detail::insert;

constexpr u32 kTevColor = 0x180;    // u32[16]: tevc, a stage's colour combiner
constexpr u32 kTevAlpha = 0x1C0;    // u32[16]: teva, its alpha combiner
constexpr u32 kTevOrder = 0x150;    // u32[8]: tref, two stages each
constexpr u32 kTevTexMap = 0x5A4;   // u32[16]: the texture map each stage was given
constexpr u32 kTevTexCoordEnabled = 0x5E8;  // u32: the stages that use a texture coordinate
constexpr u8 kChannelTable[9] = {0, 1, 0, 1, 0, 1, 7, 5, 6};
constexpr u32 kColorControl = 0x0B8;  // u32[4]: colorControl, one per colour/alpha channel (XF)
constexpr u32 kRas1Iref = 0x170;      // the indirect texture reference register (BP 0x27)
constexpr u32 kRas1Ss0 = 0x178;       // indirect stages 0,1 scale (BP 0x25)
constexpr u32 kRas1Ss1 = 0x17C;       // indirect stages 2,3 scale (BP 0x26)

void send(GuestAddr gd, GuestAddr reg) {
    {
        detail::Fifo fifo;
        fifo.bp(load<u32>(reg));
    }
    store<u16>(gd + detail::GxData2007::kBpSentNot, 0);
}

}  // namespace

// GXSetTevColorIn(stage, a, b, c, d)
void set_tev_color_in(u32 stage, u32 a, u32 b, u32 c, u32 d) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const GuestAddr reg = gd + kTevColor + stage * 4;
    u32 value = load<u32>(reg);
    value = insert(value, a, 12, 0x0000F000u);
    value = insert(value, b, 8, 0x00000F00u);
    value = insert(value, c, 4, 0x000000F0u);
    value = insert(value, d, 0, 0x0000000Fu);
    store<u32>(reg, value);
    send(gd, reg);
}

// GXSetTevAlphaIn(stage, a, b, c, d)
void set_tev_alpha_in(u32 stage, u32 a, u32 b, u32 c, u32 d) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const GuestAddr reg = gd + kTevAlpha + stage * 4;
    u32 value = load<u32>(reg);
    value = insert(value, a, 13, 0x0000E000u);
    value = insert(value, b, 10, 0x00001C00u);
    value = insert(value, c, 7, 0x00000380u);
    value = insert(value, d, 4, 0x00000070u);
    store<u32>(reg, value);
    send(gd, reg);
}

// GXSetTevOrder(stage, coord, map, color)
void set_tev_order(u32 stage, u32 coord, u32 map, u32 color) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const u32 bare = map & ~0x100u;          // without GX_TEX_DISABLE
    const u32 mapField = bare >= 8 ? 0 : bare;
    store<u32>(gd + kTevTexMap + stage * 4, map);
    u32 enabled = load<u32>(gd + kTevTexCoordEnabled);
    if (static_cast<s32>(coord) >= 8) {
        enabled &= ~(1u << stage);
        coord = 0;
    } else {
        enabled |= 1u << stage;
    }
    store<u32>(gd + kTevTexCoordEnabled, enabled);

    const GuestAddr reg = gd + kTevOrder + (static_cast<u32>(static_cast<s32>(stage) / 2)) * 4;
    // (a channel past the table is the game's bug: the original reads on into
    // whatever data follows it, which no native can know; 0 here)
    const u32 channel = static_cast<s32>(color) == 255 ? 7u : (color < 9 ? kChannelTable[color] : 0u);
    const u32 textured = (static_cast<s32>(map) != 255 && !(map & 0x100)) ? 1u : 0u;
    u32 value = load<u32>(reg);
    if (stage & 1) {
        value = insert(value, mapField, 12, 0x00007000u);
        value = insert(value, coord, 15, 0x00038000u);
        value = insert(value, channel, 19, 0x00380000u);
        value = insert(value, textured, 18, 0x00040000u);
    } else {
        value = insert(value, mapField, 0, 0x00000007u);
        value = insert(value, coord, 3, 0x00000038u);
        value = insert(value, channel, 7, 0x00000380u);
        value = insert(value, textured, 6, 0x00000040u);
    }
    store<u32>(reg, value);
    send(gd, reg);
    store<u32>(gd + detail::GxData2007::kDirtyState, load<u32>(gd + detail::GxData2007::kDirtyState) | 1u);
}

namespace {
constexpr u32 kKonstSel = 0x200;  // u32[8]: ksel - konstant selections and the swap tables
constexpr u32 kGenMode = 0x254;
constexpr u32 kBlendMode = 0x220;  // cmode0

// GXSetTevColorOp / GXSetTevAlphaOp: op, bias, scale, clamp, output register;
// a comparison (op past 1) takes the scale bits for its kind and bias 3
void set_tev_op(u32 base, u32 stage, u32 op, u32 bias, u32 scale, u32 clamp, u32 out) {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const GuestAddr reg = gd + base + stage * 4;
    u32 value = load<u32>(reg);
    value = insert(value, op, 18, 0x00040000u);
    if (static_cast<s32>(op) <= 1) {
        value = insert(value, scale, 20, 0x00300000u);
        value = insert(value, bias, 16, 0x00030000u);
    } else {
        value = insert(value, op, 19, 0x00300000u);
        value |= 0x00030000u;
    }
    value = insert(value, clamp, 19, 0x00080000u);
    value = insert(value, out, 22, 0x00C00000u);
    store<u32>(reg, value);
    send(gd, reg);
}

// GXSetTevKColorSel / GXSetTevKAlphaSel: a 5-bit field of the stage pair's ksel
void set_konst_sel(u32 stage, u32 sel, bool alpha) {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const GuestAddr reg = gd + kKonstSel + ((stage << 1) & ~3u);
    u32 value = load<u32>(reg);
    if (stage & 1) {
        value = alpha ? insert(value, sel, 19, 0x00F80000u) : insert(value, sel, 14, 0x0007C000u);
    } else {
        value = alpha ? insert(value, sel, 9, 0x00003E00u) : insert(value, sel, 4, 0x000001F0u);
    }
    store<u32>(reg, value);
    send(gd, reg);
}

void bp_words(GuestAddr gd, std::initializer_list<u32> words) {
    {
        detail::Fifo fifo;
        for (u32 w : words) {
            fifo.bp(w);
        }
    }
    store<u16>(gd + detail::GxData2007::kBpSentNot, 0);
}
}  // namespace

void set_tev_color_op(u32 stage, u32 op, u32 bias, u32 scale, u32 clamp, u32 out) noexcept {
    set_tev_op(kTevColor, stage, op, bias, scale, clamp, out);
}
void set_tev_alpha_op(u32 stage, u32 op, u32 bias, u32 scale, u32 clamp, u32 out) noexcept {
    set_tev_op(kTevAlpha, stage, op, bias, scale, clamp, out);
}
void set_tev_kcolor_sel(u32 stage, u32 sel) noexcept { set_konst_sel(stage, sel, false); }
void set_tev_kalpha_sel(u32 stage, u32 sel) noexcept { set_konst_sel(stage, sel, true); }

// GXSetTevSwapMode(stage, ras, tex): the low four bits of teva
void set_tev_swap_mode(u32 stage, u32 ras, u32 tex) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const GuestAddr reg = gd + kTevAlpha + stage * 4;
    u32 value = load<u32>(reg);
    value = insert(value, ras, 0, 0x3u);
    value = insert(value, tex, 2, 0xCu);
    store<u32>(reg, value);
    send(gd, reg);
}

// GXSetTevSwapModeTable(table, r, g, b, a): two ksel words
void set_tev_swap_mode_table(u32 table, u32 r, u32 g, u32 b, u32 a) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const GuestAddr first = gd + kKonstSel + table * 8, second = gd + kKonstSel + (table * 2 + 1) * 4;
    store<u32>(first, insert(insert(load<u32>(first), r, 0, 0x3u), g, 2, 0xCu));
    store<u32>(second, insert(insert(load<u32>(second), b, 0, 0x3u), a, 2, 0xCu));
    bp_words(gd, {load<u32>(first), load<u32>(second)});
}

// GXSetTevKColor(id, GXColor*): registers 0xE0 + 2id (red, alpha) and
// 0xE1 + 2id (blue, green), each marked a konstant; nothing kept
void set_tev_kcolor(u32 id, u32 rgba) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const u32 ra = ((id * 2 + 0xE0) << 24) | (rgba >> 24) | ((rgba & 0xFF) << 12) | 0x00800000u;
    const u32 bg = ((id * 2 + 0xE1) << 24) | ((rgba >> 8) & 0xFF) | (((rgba >> 16) & 0xFF) << 12) | 0x00800000u;
    bp_words(gd, {ra, bg});
}

// GXSetTevColorS10(id, GXColorS10*): 11 bits each; the second word three
// times, as the SDK sends it
void set_tev_color_s10(u32 id, u32 rg, u32 ba) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    const u32 r = rg >> 16, g = rg & 0xFFFF, b = ba >> 16, a = ba & 0xFFFF;
    const u32 ra = ((id * 2 + 0xE0) << 24) | (r & 0x7FF) | ((a & 0x7FF) << 12);
    const u32 bgw = ((id * 2 + 0xE1) << 24) | (b & 0x7FF) | ((g & 0x7FF) << 12);
    bp_words(gd, {ra, bgw, bgw, bgw});
}

// GXSetNumTevStages(n): genMode, sent later (dirty state bit 2)
void set_num_tev_stages(u32 count) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    store<u32>(gd + kGenMode, insert(load<u32>(gd + kGenMode), count - 1, 10, 0x00003C00u));
    store<u32>(gd + detail::GxData2007::kDirtyState, load<u32>(gd + detail::GxData2007::kDirtyState) | 4u);
}

// GXSetNumTexGens / GXSetNumChans / GXSetNumIndStages: genMode fields, sent
// later (the dirty-state bits each sets, as the original's)
void set_gen_mode_count(u32 count, u32 shift, u32 mask, u32 dirty) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    store<u32>(gd + kGenMode, insert(load<u32>(gd + kGenMode), count, shift, mask));
    store<u32>(gd + detail::GxData2007::kDirtyState, load<u32>(gd + detail::GxData2007::kDirtyState) | dirty);
}
void set_num_tex_gens(u32 count) noexcept { set_gen_mode_count(count, 0, 0x0000000Fu, 0x02000004u); }
void set_num_chans(u32 count) noexcept { set_gen_mode_count(count, 4, 0x00000070u, 0x01000004u); }
void set_num_ind_stages(u32 count) noexcept { set_gen_mode_count(count, 16, 0x00070000u, 0x00000006u); }

// GXSetBlendMode(type, src, dst, op): cmode0; subtract and logic are bits of
// their own
void set_blend_mode(u32 type, u32 src, u32 dst, u32 op) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    u32 value = load<u32>(gd + kBlendMode);
    value = insert(value, type == 3 ? 1u : 0u, 11, 0x00000800u);
    value = insert(value, type, 0, 0x00000001u);
    value = insert(value, type == 2 ? 1u : 0u, 1, 0x00000002u);
    value = insert(value, op, 12, 0x0000F000u);
    value = insert(value, src, 8, 0x00000700u);
    value = insert(value, dst, 5, 0x000000E0u);
    store<u32>(gd + kBlendMode, value);
    bp_words(gd, {value});
}

// GXSetAlphaCompare(comp0, ref0, op, comp1, ref1): register 0xF3, not kept
void set_alpha_compare(u32 comp0, u32 ref0, u32 op, u32 comp1, u32 ref1) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    u32 value = 0xF3000000u;
    value = insert(value, ref0, 0, 0x000000FFu);
    value = insert(value, ref1, 8, 0x0000FF00u);
    value = insert(value, comp0, 16, 0x00070000u);
    value = insert(value, comp1, 19, 0x00380000u);
    value = insert(value, op, 22, 0x00C00000u);
    bp_words(gd, {value});
}

// GXSetTevDirect(stage): GXSetTevIndirect with every field zero, so the IND_CMD
// register (0x10 + stage) with nothing set - what a material with no indirect
// texturing writes for each stage. The full GXSetTevIndirect takes stack
// arguments and only an indirect material needs it; it stays translated.
void set_tev_direct(u32 stage) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    {
        detail::Fifo fifo;
        fifo.bp((0x10u + stage) << 24);
    }
    store<u16>(gd + detail::GxData2007::kBpSentNot, 0);
}

// GXSetChanCtrl(chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn): an XF
// channel register (ogws GXLight.c). Deferred - the dirty-state flush sends it.
void set_chan_ctrl(u32 chan, u32 enable, u32 amb_src, u32 mat_src, u32 light_mask, u32 diff_fn,
                   u32 attn_fn) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0) {
        return;
    }
    // GX_AF_SPEC = 0, GX_AF_NONE = 2 (ogws GXEnum.h)
    u32 reg = 0;
    reg = insert(reg, mat_src, 0, 0x1u);
    reg = insert(reg, enable, 1, 0x2u);
    reg = insert(reg, light_mask, 2, 0x3Cu);                    // lights 0-3
    reg = insert(reg, amb_src, 6, 0x40u);
    reg = insert(reg, attn_fn == 0 ? 0u : diff_fn, 7, 0x180u);  // diffuse, GX_DF_NONE when specular
    reg = insert(reg, attn_fn != 2 ? 1u : 0u, 9, 0x200u);       // attenuation enabled
    reg = insert(reg, attn_fn != 0 ? 1u : 0u, 10, 0x400u);      // spotlight, not specular
    reg = insert(reg, light_mask >> 4, 11, 0x7800u);            // lights 4-7
    const u32 idx = chan % 4;
    store<u32>(gd + kColorControl + idx * 4, reg);
    u32 dirty = load<u32>(gd + detail::GxData2007::kDirtyState) | (0x1000u << idx);
    if (chan == 4) {  // GX_COLOR0A0: the matching alpha channel too
        store<u32>(gd + kColorControl + 2 * 4, reg);
        dirty |= 0x1000u | 0x4000u;
    } else if (chan == 5) {  // GX_COLOR1A1
        store<u32>(gd + kColorControl + 3 * 4, reg);
        dirty |= 0x2000u | 0x8000u;
    }
    store<u32>(gd + detail::GxData2007::kDirtyState, dirty);
}

// GXSetIndTexOrder(stage, coord, map): three bits of map then of coord per
// stage in the IREF register (ogws GXBump.c)
void set_ind_tex_order(u32 stage, u32 coord, u32 map) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0 || stage > 3) {
        return;
    }
    if (static_cast<s32>(map) == 255) {
        map = 0;  // GX_TEXMAP_NULL -> GX_TEXMAP0
    }
    if (static_cast<s32>(coord) == 255) {
        coord = 0;  // GX_TEXCOORD_NULL -> GX_TEXCOORD0
    }
    const GuestAddr reg = gd + kRas1Iref;
    const u32 mapShift = stage * 6;
    u32 value = load<u32>(reg);
    value = insert(value, map, mapShift, 0x7u << mapShift);
    value = insert(value, coord, mapShift + 3, 0x7u << (mapShift + 3));
    store<u32>(reg, value);  // the register id (0x27) stays in the top byte
    send(gd, reg);
    store<u32>(gd + detail::GxData2007::kDirtyState, load<u32>(gd + detail::GxData2007::kDirtyState) | 3u);
}

// GXSetIndTexCoordScale(stage, scaleS, scaleT): four bits each, in SS0 (stages
// 0,1) or SS1 (stages 2,3), the register id set in the top byte
void set_ind_tex_coord_scale(u32 stage, u32 scaleS, u32 scaleT) noexcept {
    const GuestAddr gd = detail::data();
    if (gd == 0 || stage > 3) {
        return;
    }
    const GuestAddr reg = gd + (stage < 2 ? kRas1Ss0 : kRas1Ss1);
    const u32 opcode = stage < 2 ? 0x25u : 0x26u;
    const u32 shift = (stage & 1) ? 8u : 0u;
    u32 value = load<u32>(reg);
    value = insert(value, scaleS, shift, 0xFu << shift);
    value = insert(value, scaleT, shift + 4, 0xFu << (shift + 4));
    value = insert(value, opcode, 24, 0xFF000000u);
    store<u32>(reg, value);
    send(gd, reg);
}

namespace {
void GXSetTevColorIn(Cpu* cpu) {
    const Host& h = host();
    set_tev_color_in(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5), h.gpr(cpu, 6), h.gpr(cpu, 7));
}
void GXSetTevAlphaIn(Cpu* cpu) {
    const Host& h = host();
    set_tev_alpha_in(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5), h.gpr(cpu, 6), h.gpr(cpu, 7));
}
void GXSetTevOrder(Cpu* cpu) {
    const Host& h = host();
    set_tev_order(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5), h.gpr(cpu, 6));
}
u32 r(Cpu* cpu, int i) { return host().gpr(cpu, i); }
void GXSetTevColorOp(Cpu* c) { set_tev_color_op(r(c, 3), r(c, 4), r(c, 5), r(c, 6), r(c, 7), r(c, 8)); }
void GXSetTevAlphaOp(Cpu* c) { set_tev_alpha_op(r(c, 3), r(c, 4), r(c, 5), r(c, 6), r(c, 7), r(c, 8)); }
void GXSetTevKColorSel(Cpu* c) { set_tev_kcolor_sel(r(c, 3), r(c, 4)); }
void GXSetTevKAlphaSel(Cpu* c) { set_tev_kalpha_sel(r(c, 3), r(c, 4)); }
void GXSetTevSwapMode(Cpu* c) { set_tev_swap_mode(r(c, 3), r(c, 4), r(c, 5)); }
void GXSetTevSwapModeTable(Cpu* c) { set_tev_swap_mode_table(r(c, 3), r(c, 4), r(c, 5), r(c, 6), r(c, 7)); }
void GXSetTevKColor(Cpu* c) { set_tev_kcolor(r(c, 3), load<u32>(r(c, 4))); }
void GXSetTevColorS10(Cpu* c) { set_tev_color_s10(r(c, 3), load<u32>(r(c, 4)), load<u32>(r(c, 4) + 4)); }
void GXSetNumTevStages(Cpu* c) { set_num_tev_stages(r(c, 3) & 0xFF); }
void GXSetNumTexGens(Cpu* c) { set_num_tex_gens(r(c, 3) & 0xFF); }
void GXSetNumChans(Cpu* c) { set_num_chans(r(c, 3) & 0xFF); }
void GXSetNumIndStages(Cpu* c) { set_num_ind_stages(r(c, 3) & 0xFF); }
void GXSetChanCtrl(Cpu* c) { set_chan_ctrl(r(c, 3), r(c, 4), r(c, 5), r(c, 6), r(c, 7), r(c, 8), r(c, 9)); }
void GXSetTevDirect(Cpu* c) { set_tev_direct(r(c, 3)); }
void GXSetIndTexOrder(Cpu* c) { set_ind_tex_order(r(c, 3), r(c, 4), r(c, 5)); }
void GXSetIndTexCoordScale(Cpu* c) { set_ind_tex_coord_scale(r(c, 3), r(c, 4), r(c, 5)); }
void GXSetBlendMode(Cpu* c) { set_blend_mode(r(c, 3), r(c, 4), r(c, 5), r(c, 6)); }
void GXSetAlphaCompare(Cpu* c) { set_alpha_compare(r(c, 3), r(c, 4) & 0xFF, r(c, 5), r(c, 6), r(c, 7) & 0xFF); }
}  // namespace

extern const Native kGxTevNatives[] = {
    WIINX_NATIVE("GXSetTevColorOp", kGx_2007_12, GXSetTevColorOp),
    WIINX_NATIVE("GXSetTevAlphaOp", kGx_2007_12, GXSetTevAlphaOp),
    WIINX_NATIVE("GXSetTevKColorSel", kGx_2007_12, GXSetTevKColorSel),
    WIINX_NATIVE("GXSetTevKAlphaSel", kGx_2007_12, GXSetTevKAlphaSel),
    WIINX_NATIVE("GXSetTevSwapMode", kGx_2007_12, GXSetTevSwapMode),
    WIINX_NATIVE("GXSetTevSwapModeTable", kGx_2007_12, GXSetTevSwapModeTable),
    WIINX_NATIVE("GXSetTevKColor", kGx_2007_12, GXSetTevKColor),
    WIINX_NATIVE("GXSetTevColorS10", kGx_2007_12, GXSetTevColorS10),
    WIINX_NATIVE("GXSetNumTevStages", kGx_2007_12, GXSetNumTevStages),
    WIINX_NATIVE("GXSetNumTexGens", kGx_2007_12, GXSetNumTexGens),
    WIINX_NATIVE("GXSetNumChans", kGx_2007_12, GXSetNumChans),
    WIINX_NATIVE("GXSetNumIndStages", kGx_2007_12, GXSetNumIndStages),
    WIINX_NATIVE("GXSetChanCtrl", kGx_2007_12, GXSetChanCtrl),
    WIINX_NATIVE("GXSetTevDirect", kGx_2007_12, GXSetTevDirect),
    WIINX_NATIVE("GXSetIndTexOrder", kGx_2007_12, GXSetIndTexOrder),
    WIINX_NATIVE("GXSetIndTexCoordScale", kGx_2007_12, GXSetIndTexCoordScale),
    WIINX_NATIVE("GXSetBlendMode", kGx_2007_12, GXSetBlendMode),
    WIINX_NATIVE("GXSetAlphaCompare", kGx_2007_12, GXSetAlphaCompare),
    WIINX_NATIVE("GXSetTevColorIn", kGx_2007_12, GXSetTevColorIn),
    WIINX_NATIVE("GXSetTevAlphaIn", kGx_2007_12, GXSetTevAlphaIn),
    WIINX_NATIVE("GXSetTevOrder", kGx_2007_12, GXSetTevOrder),
};
extern const std::size_t kGxTevNativeCount = std::size(kGxTevNatives);

}  // namespace wiinx::sdk::gx
