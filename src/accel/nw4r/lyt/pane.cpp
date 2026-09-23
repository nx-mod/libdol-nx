// nw4r::lyt::Pane::CalculateMtx
//
// Every menu screen recomputes each layout pane's matrix every frame: scale,
// three axis rotations and a translation, chained onto the parent's. Run as
// translated PowerPC it was 8% of an animated menu's frame in Mario Kart Wii,
// plus 2% in the PSMTXRotRad it calls three times per pane.
//
// Logic follows the ogws decompilation (doldecomp/ogws, CC0: lyt_pane.cpp).
// The 2007 and 2008 builds run the same logic; the 2008 build added four
// bytes after the matrices, so alpha, global alpha and the flags sit four
// bytes later. Offsets: 2007 from ogws's lyt_pane.h, 2008 read off Mario Kart
// Wii's code.
//
// Layout is UI-only - nothing replays it - so ordinary float math replaces
// the paired-single sequence and MSL's sinf/cosf without any determinism
// concern. Gameplay math must not be ported this way.
#include "wiinx/accel/nw4r.hpp"
#include "wiinx/core/guest.hpp"

#include <cmath>

namespace wiinx::nw4r::lyt {

// Set by the binding (see nw4r.hpp).
GuestAddr g_calculateMtxAddress = 0;

namespace {

// Members both builds share (ogws lyt_pane.h / lyt_drawInfo.h).
struct PaneCommon {
    static constexpr Field<u32, 0x0C> parent{};
    static constexpr Field<u32, 0x14> childListNode{};  // mChildList's own node: first link at +0
    static constexpr u32 kChildLinkOffset = 0x04;       // a pane's node within its parent's list
    static constexpr Field<f32, 0x2C> translate{};      // VEC3
    static constexpr Field<f32, 0x38> rotate{};         // VEC3, degrees
    static constexpr Field<f32, 0x44> scale{};          // VEC2
    static constexpr Field<Mtx34, 0x54> mtx{};
    static constexpr Field<Mtx34, 0x84> glbMtx{};
    static constexpr u32 kVtableCalculateMtx = 0x10;
    static constexpr u8 kVisible = 0x01;
    static constexpr u8 kInfluencedAlpha = 0x02;
    static constexpr u8 kLocationAdjust = 0x04;
};

struct PaneLayout2007 : PaneCommon {
    static constexpr Field<u8, 0xB4> alpha{};
    static constexpr Field<u8, 0xB5> glbAlpha{};
    static constexpr Field<u8, 0xB7> flag{};
};

struct PaneLayout2008 : PaneCommon {
    static constexpr Field<u8, 0xB8> alpha{};
    static constexpr Field<u8, 0xB9> glbAlpha{};
    static constexpr Field<u8, 0xBB> flag{};
};

// DrawInfo is the same in both builds.
struct DrawInfo {
    static constexpr Field<Mtx34, 0x04> viewMtx{};
    static constexpr Field<f32, 0x44> locationAdjustScale{};  // VEC2
    static constexpr Field<f32, 0x4C> globalAlpha{};
    static constexpr Field<u8, 0x50> flags{};
    static constexpr u8 kMultipleViewMtx = 0x80;
    static constexpr u8 kInfluencedAlpha = 0x40;
    static constexpr u8 kLocationAdjust = 0x20;
    static constexpr u8 kInvisiblePaneCalculateMtx = 0x10;
};

// PSMTXConcat: a * b for 3x4 affine matrices.
Mtx34 concat(const Mtx34& a, const Mtx34& b) noexcept {
    Mtx34 out;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            out.m[row][col] = a.m[row][0] * b.m[0][col] + a.m[row][1] * b.m[1][col] +
                              a.m[row][2] * b.m[2][col] + (col == 3 ? a.m[row][3] : 0.0f);
        }
    }
    return out;
}

// PSMTXRotRad about 'x', 'y' or 'z'.
Mtx34 rotation(char axis, f32 radians) noexcept {
    const f32 s = std::sin(radians);
    const f32 c = std::cos(radians);
    Mtx34 r{};
    switch (axis) {
    case 'x': r.m[0][0] = 1; r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; break;
    case 'y': r.m[0][0] = c; r.m[0][2] = s; r.m[1][1] = 1; r.m[2][0] = -s; r.m[2][2] = c; break;
    default:  r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c; r.m[2][2] = 1; break;
    }
    return r;
}

constexpr f32 kDegToRad = 3.14159265358979323846f / 180.0f;

template <class L>
void calculate_mtx(Cpu* cpu) {
    const Host& h = host();
    const Guest<L> self{h.gpr(cpu, 3)};
    const Guest<DrawInfo> info{h.gpr(cpu, 4)};

    // These reads go straight to guest memory, where the original's went
    // through the runtime's checked accessors: a pointer that is not really a
    // pane was reported there, and would fault here. Check the two the game
    // hands us once, and every child before it is walked into.
    const auto readable = [&h](GuestAddr address, u32 size) {
        return h.valid == nullptr || h.valid(address, size);
    };
    if (!readable(self.addr, L::flag.offset + 1) ||
        !readable(info.addr, DrawInfo::flags.offset + 1)) {
        return;
    }

    const u8 flag = self[L::flag];
    const u8 infoFlags = info[DrawInfo::flags];
    if (!(flag & L::kVisible) && !(infoFlags & DrawInfo::kInvisiblePaneCalculateMtx)) {
        return;
    }

    f32 scaleX = self.at(L::scale, 0);
    f32 scaleY = self.at(L::scale, 1);
    if ((infoFlags & DrawInfo::kLocationAdjust) && (flag & L::kLocationAdjust)) {
        scaleX *= info.at(DrawInfo::locationAdjustScale, 0);
        scaleY *= info.at(DrawInfo::locationAdjustScale, 1);
    }
    Mtx34 scale{};
    scale.m[0][0] = scaleX;
    scale.m[1][1] = scaleY;
    scale.m[2][2] = 1.0f;

    Mtx34 mtx = concat(rotation('x', self.at(L::rotate, 0) * kDegToRad), scale);
    mtx = concat(rotation('y', self.at(L::rotate, 1) * kDegToRad), mtx);
    mtx = concat(rotation('z', self.at(L::rotate, 2) * kDegToRad), mtx);

    // PSMTXTransApply: translation added on the left.
    for (int row = 0; row < 3; ++row) {
        mtx.m[row][3] += self.at(L::translate, static_cast<u32>(row));
    }
    self.set(L::mtx, mtx);

    const Guest<L> parent{self[L::parent]};
    if (parent) {
        self.set(L::glbMtx, concat(parent[L::glbMtx], mtx));
    } else if (infoFlags & DrawInfo::kMultipleViewMtx) {
        self.set(L::glbMtx, mtx);
    } else {
        self.set(L::glbMtx, concat(info[DrawInfo::viewMtx], mtx));
    }

    const u8 alpha = self[L::alpha];
    const f32 globalAlpha = info[DrawInfo::globalAlpha];
    const bool influenced = (infoFlags & DrawInfo::kInfluencedAlpha) != 0;
    self.set(L::glbAlpha, (influenced && parent) ? static_cast<u8>(alpha * globalAlpha) : alpha);

    // Children inherit this pane's alpha while they are computed.
    const bool modifyInfo = (flag & L::kInfluencedAlpha) && alpha != 255;
    if (modifyInfo) {
        info.set(DrawInfo::globalAlpha, (globalAlpha * static_cast<f32>(alpha)) * (1.0f / 255.0f));
        info.set(DrawInfo::flags, static_cast<u8>(info[DrawInfo::flags] | DrawInfo::kInfluencedAlpha));
    }

    // CalculateMtxChild: each child through its own vtable, which for an
    // ordinary pane routes straight back here.
    const GuestAddr sentinel = self.addr + L::childListNode.offset;
    for (GuestAddr link = load<u32>(sentinel); link != sentinel && link != 0; link = load<u32>(link)) {
        const GuestAddr child = link - L::kChildLinkOffset;
        if (!readable(child, L::flag.offset + 1)) {
            break;
        }
        const GuestAddr method = load<u32>(load<u32>(child) + L::kVtableCalculateMtx);
        h.set_gpr(cpu, 3, child);
        h.set_gpr(cpu, 4, info.addr);
        if (method == g_calculateMtxAddress && g_calculateMtxAddress != 0) {
            // An ordinary pane: its vtable entry is this native. Going out
            // through the guest dispatcher to arrive back here costs a lookup
            // and a call frame for every pane in the tree, and a menu is
            // hundreds of panes deep - the code this replaced called its
            // children directly.
            calculate_mtx<L>(cpu);
        } else {
            h.call(cpu, method);
        }
    }

    if (modifyInfo) {
        info.set(DrawInfo::globalAlpha, globalAlpha);
        const u8 now = info[DrawInfo::flags];
        info.set(DrawInfo::flags, influenced ? static_cast<u8>(now | DrawInfo::kInfluencedAlpha)
                                             : static_cast<u8>(now & ~DrawInfo::kInfluencedAlpha));
    }
}

}  // namespace

void set_pane_calculate_mtx_address(GuestAddr address) noexcept {
    g_calculateMtxAddress = address;
}

// The module table picks these up.
extern const Native kPaneNatives[] = {
    WIINX_NATIVE("nw4r::lyt::Pane::CalculateMtx", kLyt_2007_06, calculate_mtx<PaneLayout2007>),
    WIINX_NATIVE("nw4r::lyt::Pane::CalculateMtx", kLyt_2008_03, calculate_mtx<PaneLayout2008>),
};
extern const std::size_t kPaneNativeCount = std::size(kPaneNatives);

}  // namespace wiinx::nw4r::lyt
