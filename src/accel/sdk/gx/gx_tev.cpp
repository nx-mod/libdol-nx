// GX's TEV setters, natively: GXSetTevColorIn, GXSetTevAlphaIn, GXSetTevOrder.
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

namespace wiinx::sdk::gx {
namespace {

using detail::insert;

constexpr u32 kTevColor = 0x180;    // u32[16]: tevc, a stage's colour combiner
constexpr u32 kTevAlpha = 0x1C0;    // u32[16]: teva, its alpha combiner
constexpr u32 kTevOrder = 0x150;    // u32[8]: tref, two stages each
constexpr u32 kTevTexMap = 0x5A4;   // u32[16]: the texture map each stage was given
constexpr u32 kTevTexCoordEnabled = 0x5E8;  // u32: the stages that use a texture coordinate
constexpr u8 kChannelTable[9] = {0, 1, 0, 1, 0, 1, 7, 5, 6};

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
}  // namespace

extern const Native kGxTevNatives[] = {
    WIINX_NATIVE("GXSetTevColorIn", kGx_2007_12, GXSetTevColorIn),
    WIINX_NATIVE("GXSetTevAlphaIn", kGx_2007_12, GXSetTevAlphaIn),
    WIINX_NATIVE("GXSetTevOrder", kGx_2007_12, GXSetTevOrder),
};
extern const std::size_t kGxTevNativeCount = std::size(kGxTevNatives);

}  // namespace wiinx::sdk::gx
