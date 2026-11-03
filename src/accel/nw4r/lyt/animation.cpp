// nw4r::lyt animation: every animated pane and material, every frame.
//
// A layout animates by walking its pane tree: each pane runs the animations
// bound to it (position, rotation, scale, size, visibility, vertex colours),
// then its material's (colours, texture SRT, texture swaps, indirect SRT),
// then its children. Translated, the walk is a guest call per pane, per
// animation and per curve, and a menu is hundreds of panes deep; here the walk
// stays native from the layout's root to the last key, and leaves for the
// game only where the game may have overridden something (a pane class of its
// own, a vertex colour setter).
//
// Logic follows the ogws decompilation (doldecomp/ogws, CC0: lyt_animation.cpp,
// lyt_pane.cpp, lyt_material.cpp, lyt_texMap.cpp). Offsets and slots: 2007
// from ogws's headers; 2008 read off Mario Kart Wii's code, and New Super Mario
// Bros. Wii's (2009-04) found identical in every function here.
//
// Exact: the curves are the original's single-precision operations in the
// original's order (no fused multiply-add: the 2009 code has none either), and
// the colour conversions clamp and truncate as Gekko's quantised stores do. A
// key that lands between two frames lands on the same float.
#pragma GCC optimize("fp-contract=off")

#include "wiinx/accel/nw4r.hpp"
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace wiinx::nw4r::lyt {
namespace {

// ---------- what both layouts share (ogws lyt_pane.h, lyt_material.h,
// lyt_animation.h, lyt_types.h)

struct Common {
    // Pane
    static constexpr u32 kPaneChildren = 0x14;    // mChildList's node; a child's own node is at +4
    static constexpr u32 kPaneChildLink = 0x04;
    static constexpr u32 kPaneAnimations = 0x20;  // mAnimList's node
    static constexpr u32 kPaneMaterial = 0x28;
    static constexpr u32 kPaneSrt = 0x2C;         // translate, rotate, scale, size: ten floats
    static constexpr u32 kPaneAnimate = 0x1C;     // vtable slots
    static constexpr u32 kPaneAnimateSelf = 0x20;
    static constexpr u32 kPaneSetColorElement = 0x30;
    static constexpr u8 kVisible = 0x01;
    static constexpr u32 kSkipInvisible = 0x01;   // ANIMOPTION_SKIP_INVISIBLE

    // AnimationLink
    static constexpr u32 kLinkTransform = 0x08;
    static constexpr u32 kLinkIndex = 0x0C;       // u16
    static constexpr u32 kLinkDisabled = 0x0E;    // bool

    // AnimTransform(Basic)
    static constexpr u32 kTransformRes = 0x0C;
    static constexpr u32 kTransformFrame = 0x10;
    static constexpr u32 kTransformFiles = 0x14;  // the texture palettes a pattern swaps in

    // Material
    static constexpr u32 kMaterialAnimations = 0x08;  // mAnimList's node
    static constexpr u32 kMaterialTevColors = 0x10;   // GXColorS10[3]
    static constexpr u32 kMaterialTevKColors = 0x28;  // GXColor[4]
    static constexpr u32 kMaterialCap = 0x38;         // BitGXNums: what mpGXMem holds room for
    static constexpr u32 kMaterialNum = 0x3C;         // ... and what it holds
    static constexpr u32 kMaterialGxMem = 0x40;
    static constexpr u32 kMaterialAnimate = 0x1C;     // vtable slot

    // the material's GX block: each array after the ones before it, sized by
    // the capacities (ogws lyt_material.cpp, CalcOffset*)
    static constexpr u32 kTexMapSize = 0x1C;
    static constexpr u32 kTexSrtSize = 0x14;
};

// BitGXNums, as the compiler packs it from the top bit down
struct Nums {
    u32 bits;
    u32 tex_map() const { return bits >> 28; }
    u32 tex_srt() const { return (bits >> 24) & 0xF; }
    u32 tex_coord_gen() const { return (bits >> 20) & 0xF; }
    u32 ind_srt() const { return (bits >> 18) & 0x3; }
    u32 ind_stage() const { return (bits >> 15) & 0x7; }
    u32 tev_swap() const { return (bits >> 14) & 0x1; }
    u32 chan_ctrl() const { return (bits >> 8) & 0x1; }
    u32 mat_col() const { return (bits >> 7) & 0x1; }
    u32 alp_comp() const { return (bits >> 6) & 0x1; }
    u32 blend_mode() const { return (bits >> 5) & 0x1; }
};

struct Layout2007 : Common {
    static constexpr u32 kPaneFlag = 0xB7;
    static constexpr u32 kAnimatePane = 0x18;      // AnimTransform vtable
    static constexpr u32 kAnimateMaterial = 0x1C;
};

// 2008-03 (Mario Kart Wii) and 2009-04 (New Super Mario Bros. Wii): four bytes
// after the pane's matrices, and AnimTransform one virtual longer
struct Layout2008 : Common {
    static constexpr u32 kPaneFlag = 0xBB;
    static constexpr u32 kAnimatePane = 0x1C;
    static constexpr u32 kAnimateMaterial = 0x20;
};

// ---------- names, as each game's bindings register them (the table below
// spells them out: wiinx-sign-natives reads its literals)

constexpr const char* kPaneAnimate = "nw4r::lyt::Pane::Animate";
constexpr const char* kPaneAnimateSelf = "nw4r::lyt::Pane::AnimateSelf";
constexpr const char* kMaterialAnimate = "nw4r::lyt::Material::Animate";
constexpr const char* kAnimatePane = "nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Pane*)";
constexpr const char* kAnimateMaterial =
    "nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Material*)";

// The bindings, looked up once: whether a vtable entry is one of these
// natives, and where the original's calls return.
struct Bindings {
    const Bound* pane_animate = bound(kPaneAnimate);
    const Bound* pane_animate_self = bound(kPaneAnimateSelf);
    const Bound* material_animate = bound(kMaterialAnimate);
    const Bound* animate_pane = bound(kAnimatePane);
    const Bound* animate_material = bound(kAnimateMaterial);
};
const Bindings& bindings() {
    static const Bindings b;
    return b;
}

bool is(const Bound* binding, GuestAddr method) { return binding != nullptr && binding->address == method; }

GuestAddr method(GuestAddr object, u32 slot) { return load<u32>(load<u32>(object) + slot); }

// ---------- the curves (lyt_animation.cpp)

constexpr f32 kTolerance = 0.001f;

bool same(f32 a, f32 b) {
    const f32 c = a - b;
    return -kTolerance < c && c < kTolerance;
}

// StepKey: frame (f32), value (u16); eight bytes each
u16 step_value(f32 frame, GuestAddr keys, u32 count) {
    const auto key_frame = [keys](int i) { return load<f32>(keys + static_cast<u32>(i) * 8); };
    const auto key_value = [keys](int i) { return load<u16>(keys + static_cast<u32>(i) * 8 + 4); };
    if (count == 1 || frame <= key_frame(0)) {
        return key_value(0);
    }
    if (frame >= key_frame(static_cast<int>(count) - 1)) {
        return key_value(static_cast<int>(count) - 1);
    }
    int left = 0;
    int right = static_cast<int>(count) - 1;
    while (left != right - 1 && left != right) {
        const int center = (left + right) / 2;
        if (frame < key_frame(center)) {
            right = center;
        } else {
            left = center;
        }
    }
    return same(frame, key_frame(right)) ? key_value(right) : key_value(left);
}

// HermiteKey: frame, value, slope (f32 each); twelve bytes
f32 hermite_value(f32 frame, GuestAddr keys, u32 count) {
    const auto key = [keys](int i, u32 field) { return load<f32>(keys + static_cast<u32>(i) * 12 + field); };
    if (count == 1 || frame <= key(0, 0)) {
        return key(0, 4);
    }
    if (frame >= key(static_cast<int>(count) - 1, 0)) {
        return key(static_cast<int>(count) - 1, 4);
    }
    int left = 0;
    int right = static_cast<int>(count) - 1;
    while (left != right - 1 && left != right) {
        const int center = (left + right) / 2;
        if (frame <= key(center, 0)) {
            right = center;
        } else {
            left = center;
        }
    }
    if (same(frame, key(right, 0))) {
        if (right < static_cast<int>(count) - 1 && key(right, 0) == key(right + 1, 0)) {
            return key(right + 1, 4);
        }
        return key(right, 4);
    }
    // the original's operations, in its order
    const f32 t1 = frame - key(left, 0);
    const f32 t2 = 1.0f / (key(right, 0) - key(left, 0));
    const f32 v0 = key(left, 4), v1 = key(right, 4);
    const f32 s0 = key(left, 8), s1 = key(right, 8);
    const f32 t1_t1_t2 = t1 * t1 * t2;
    const f32 t1_t1_t2_t2 = t1_t1_t2 * t2;
    const f32 t1_t1_t1_t2_t2 = t1 * t1_t1_t2_t2;
    const f32 t1_t1_t1_t2_t2_t2 = t1_t1_t1_t2_t2 * t2;
    return v0 * (2.0f * t1_t1_t1_t2_t2_t2 - 3.0f * t1_t1_t2_t2 + 1.0f) +
           v1 * (-2.0f * t1_t1_t1_t2_t2_t2 + 3.0f * t1_t1_t2_t2) +
           s0 * (t1_t1_t1_t2_t2 - 2.0f * t1_t1_t2 + t1) +
           s1 * (t1_t1_t1_t2_t2 - t1_t1_t2);
}

// OSf32tou8 / OSf32tos16: a quantised store with scale 0, which clamps to the
// type's range and truncates (the runtime's psq_st does the same)
u8 to_u8(f32 value) { return static_cast<u8>(std::clamp(value, 0.0f, 255.0f)); }
s16 to_s16(f32 value) { return static_cast<s16>(std::clamp(value, -32768.0f, 32767.0f)); }

// ---------- the resource (lyt_animation.h): AnimationBlock -> content ->
// info -> target -> keys, each an offset from the one before

struct Target {
    GuestAddr addr;
    u8 id() const { return load<u8>(addr); }
    u8 target() const { return load<u8>(addr + 1); }
    u16 key_count() const { return load<u16>(addr + 4); }
    GuestAddr keys() const { return addr + load<u32>(addr + 8); }
};

// calls fn(kind, info, target count, target i) for every target of content
// `index` of the transform's resource
template <class Fn>
void each_target(GuestAddr transform, u32 index, Fn&& fn) {
    const GuestAddr res = load<u32>(transform + Common::kTransformRes);
    const GuestAddr table = res + load<u32>(res + 0x10);
    const GuestAddr content = res + load<u32>(table + index * 4);
    const u8 infos = load<u8>(content + 0x14);
    for (int i = 0; i < infos; ++i) {
        const GuestAddr info = content + load<u32>(content + 0x18 + static_cast<u32>(i) * 4);
        const u32 kind = load<u32>(info);
        const u8 targets = load<u8>(info + 4);
        for (int j = 0; j < targets; ++j) {
            fn(kind, Target{info + load<u32>(info + 8 + static_cast<u32>(j) * 4)});
        }
    }
}

constexpr u32 kind(char a, char b, char c, char d) {
    return static_cast<u32>(a) << 24 | static_cast<u32>(b) << 16 | static_cast<u32>(c) << 8 | static_cast<u32>(d);
}
constexpr u32 kPaneSrt = kind('R', 'L', 'P', 'A');
constexpr u32 kPaneVisibility = kind('R', 'L', 'V', 'I');
constexpr u32 kVertexColor = kind('R', 'L', 'V', 'C');
constexpr u32 kMaterialColor = kind('R', 'L', 'M', 'C');
constexpr u32 kTextureSrt = kind('R', 'L', 'T', 'S');
constexpr u32 kTexturePattern = kind('R', 'L', 'T', 'P');
constexpr u32 kIndirectSrt = kind('R', 'L', 'I', 'M');

// ---------- AnimTransformBasic::Animate(u32, Pane*)

template <class L>
void animate_pane(Cpu* cpu, GuestAddr transform, u32 index, GuestAddr pane) {
    const f32 frame = load<f32>(transform + Common::kTransformFrame);
    each_target(transform, index, [&](u32 kind, Target t) {
        switch (kind) {
        case kPaneSrt:  // Pane::SetSRTElement
            store<f32>(pane + Common::kPaneSrt + t.target() * 4u, hermite_value(frame, t.keys(), t.key_count()));
            break;
        case kPaneVisibility: {  // Pane::SetVisible
            const bool visible = step_value(frame, t.keys(), t.key_count()) != 0;
            const u8 flag = load<u8>(pane + L::kPaneFlag);
            store<u8>(pane + L::kPaneFlag, static_cast<u8>((flag & ~Common::kVisible) | (visible ? 1 : 0)));
            break;
        }
        case kVertexColor: {
            // through the pane's vtable: Picture, TextBox and Window colour
            // their own vertices, and a game's own pane class may too
            const u8 value = to_u8(hermite_value(frame, t.keys(), t.key_count()) + 0.5f);
            const Host& h = host();
            h.set_gpr(cpu, 3, pane);
            h.set_gpr(cpu, 4, t.target());
            h.set_gpr(cpu, 5, value);
            call_guest(cpu, method(pane, Common::kPaneSetColorElement), bindings().animate_pane, 0);
            break;
        }
        default: break;
        }
    });
}

// ---------- AnimTransformBasic::Animate(u32, Material*), 2008 and later

// Material::SetColorElement (lyt_material.cpp; offsets read off the 2009 code)
void set_material_color(GuestAddr material, u32 index, s16 value) {
    const Nums num{load<u32>(material + Common::kMaterialNum)};
    const auto clamp_u8 = [](s16 v) { return static_cast<u8>(std::min<s16>(std::max<s16>(v, 0), 255)); };
    if (index - 16 <= 15) {  // a TEV konstant colour
        const u32 reg = (index - 16) / 4;
        store<u8>(material + Common::kMaterialTevKColors + reg * 4 + (index - 16) % 4, clamp_u8(value));
    } else if (index - 4 <= 11) {  // a TEV colour register, signed 10 bits wide
        const u32 reg = (index - 4) / 4;
        store<s16>(material + Common::kMaterialTevColors + reg * 8 + ((index - 4) % 4) * 2, value);
    } else if (index <= 3) {  // the material colour, if it has one
        if (num.mat_col() < 1) {
            return;
        }
        const Nums cap{load<u32>(material + Common::kMaterialCap)};
        const GuestAddr colour = load<u32>(material + Common::kMaterialGxMem) + cap.tex_map() * Common::kTexMapSize +
                                 cap.tex_srt() * Common::kTexSrtSize + cap.tex_coord_gen() * 4 + cap.chan_ctrl() * 4;
        store<u8>(colour + index % 4, clamp_u8(value));
    }
}

// TexMap::ReplaceImage(TPLPalette*, id): the image, its size and format, and
// the palette's, from the TPL; wrap, filter and LOD stay as they were
void replace_image(GuestAddr texmap, GuestAddr palette, u32 id) {
    if (load<u32>(palette + 8) < 0x80000000u) {
        sdk::tpl::bind(palette);
    }
    const GuestAddr descriptor = sdk::tpl::get(palette, id);
    if (descriptor == 0) {
        return;
    }
    const GuestAddr header = load<u32>(descriptor);
    u32 bits = load<u32>(texmap + 0x18);
    store<u32>(texmap + 0x0, load<u32>(header + 8));
    store<u16>(texmap + 0x8, load<u16>(header + 2));  // width
    store<u16>(texmap + 0xA, load<u16>(header + 0));  // height
    bits = (bits & 0x0FFFFFFFu) | (load<u32>(header + 4) << 28);
    store<u32>(texmap + 0x18, bits);
    if (const GuestAddr clut = load<u32>(descriptor + 4); clut != 0) {
        store<u32>(texmap + 0x4, load<u32>(clut + 8));
        bits = (bits & ~0x6000u) | ((load<u32>(clut + 4) << 13) & 0x6000u);
        store<u32>(texmap + 0x18, bits);
        store<u16>(texmap + 0x16, load<u16>(clut + 0));
    } else {
        store<u32>(texmap + 0x4, 0);
        store<u32>(texmap + 0x18, bits & ~0x6000u);
        store<u16>(texmap + 0x16, 0);
    }
}

void animate_material_2008(GuestAddr transform, u32 index, GuestAddr material) {
    const f32 frame = load<f32>(transform + Common::kTransformFrame);
    each_target(transform, index, [&](u32 kind, Target t) {
        const Nums cap{load<u32>(material + Common::kMaterialCap)};
        const GuestAddr gx = load<u32>(material + Common::kMaterialGxMem);
        switch (kind) {
        case kMaterialColor: {
            const s16 value = to_s16(hermite_value(frame, t.keys(), t.key_count()) + 0.5f);
            set_material_color(material, t.target(), std::min<s16>(std::max<s16>(value, -1024), 1023));
            break;
        }
        case kTextureSrt:  // Material::SetTexSRTElement
            if (t.id() < cap.tex_srt()) {
                const f32 value = hermite_value(frame, t.keys(), t.key_count());
                store<f32>(gx + cap.tex_map() * Common::kTexMapSize + t.id() * Common::kTexSrtSize + t.target() * 4u,
                           value);
            }
            break;
        case kTexturePattern: {
            const GuestAddr files = load<u32>(transform + Common::kTransformFiles);
            if (files == 0) {
                return;  // (the original skips the whole info: so does every target here)
            }
            const Nums num{load<u32>(material + Common::kMaterialNum)};
            if (t.id() < num.tex_map()) {
                const u16 file = step_value(frame, t.keys(), t.key_count());
                if (const GuestAddr palette = load<u32>(files + file * 4u); palette != 0) {
                    replace_image(gx + t.id() * Common::kTexMapSize, palette, 0);
                }
            }
            break;
        }
        case kIndirectSrt:  // Material::SetIndTexSRTElement
            if (t.id() < cap.ind_srt()) {
                const f32 value = hermite_value(frame, t.keys(), t.key_count());
                const u32 offset = cap.tex_map() * Common::kTexMapSize + cap.tex_srt() * Common::kTexSrtSize +
                                   4 * (cap.tex_coord_gen() + cap.chan_ctrl() + cap.mat_col() + cap.tev_swap() +
                                        cap.alp_comp() + cap.blend_mode() + cap.ind_stage());
                store<f32>(gx + offset + t.id() * Common::kTexSrtSize + t.target() * 4u, value);
            }
            break;
        default: break;
        }
    });
}

// ---------- the walks: Material::Animate, Pane::AnimateSelf, Pane::Animate

// each enabled link of the list whose node is at `list`: fn(transform, index)
template <class Fn>
void each_link(GuestAddr list, Fn&& fn) {
    for (GuestAddr link = load<u32>(list); link != list && link != 0; link = load<u32>(link)) {
        if (load<u8>(link + Common::kLinkDisabled) == 0) {
            fn(load<u32>(link + Common::kLinkTransform), static_cast<u32>(load<u16>(link + Common::kLinkIndex)));
        }
    }
}

template <class L>
void material_animate(Cpu* cpu, GuestAddr material);

template <class L>
void call_animate_material(Cpu* cpu, GuestAddr transform, u32 index, GuestAddr material, const Bound* from, u32 site) {
    const GuestAddr target = method(transform, L::kAnimateMaterial);
    if constexpr (!std::is_same_v<L, Layout2007>) {
        if (is(bindings().animate_material, target)) {
            animate_material_2008(transform, index, material);
            return;
        }
    }
    const Host& h = host();
    h.set_gpr(cpu, 3, transform);
    h.set_gpr(cpu, 4, index);
    h.set_gpr(cpu, 5, material);
    call_guest(cpu, target, from, site);
}

template <class L>
void material_animate(Cpu* cpu, GuestAddr material) {
    each_link(material + Common::kMaterialAnimations, [&](GuestAddr transform, u32 index) {
        call_animate_material<L>(cpu, transform, index, material, bindings().material_animate, 0);
    });
}

template <class L>
void pane_animate_self(Cpu* cpu, GuestAddr pane, u32 option) {
    const Bindings& b = bindings();
    each_link(pane + Common::kPaneAnimations, [&](GuestAddr transform, u32 index) {
        const GuestAddr target = method(transform, L::kAnimatePane);
        if (is(b.animate_pane, target)) {
            animate_pane<L>(cpu, transform, index, pane);
            return;
        }
        const Host& h = host();
        h.set_gpr(cpu, 3, transform);
        h.set_gpr(cpu, 4, index);
        h.set_gpr(cpu, 5, pane);
        call_guest(cpu, target, b.pane_animate_self, 0);
    });
    if ((load<u8>(pane + L::kPaneFlag) & Common::kVisible) || !(option & Common::kSkipInvisible)) {
        if (const GuestAddr material = load<u32>(pane + Common::kPaneMaterial); material != 0) {
            const GuestAddr target = method(material, Common::kMaterialAnimate);
            if (is(b.material_animate, target)) {
                material_animate<L>(cpu, material);
            } else {
                host().set_gpr(cpu, 3, material);
                call_guest(cpu, target, b.pane_animate_self, 1);
            }
        }
    }
}

template <class L>
void pane_animate(Cpu* cpu, GuestAddr pane, u32 option) {
    const Bindings& b = bindings();
    const Host& h = host();
    // a Window animates its frames' materials too: its own AnimateSelf, from
    // the vtable, wherever the class is not plain Pane's
    if (const GuestAddr self = method(pane, Common::kPaneAnimateSelf); is(b.pane_animate_self, self)) {
        pane_animate_self<L>(cpu, pane, option);
    } else {
        h.set_gpr(cpu, 3, pane);
        h.set_gpr(cpu, 4, option);
        call_guest(cpu, self, b.pane_animate, 0);
    }
    if ((load<u8>(pane + L::kPaneFlag) & Common::kVisible) || !(option & Common::kSkipInvisible)) {
        const GuestAddr list = pane + Common::kPaneChildren;
        for (GuestAddr link = load<u32>(list); link != list && link != 0; link = load<u32>(link)) {
            const GuestAddr child = link - Common::kPaneChildLink;
            if (const GuestAddr target = method(child, Common::kPaneAnimate); is(b.pane_animate, target)) {
                pane_animate<L>(cpu, child, option);
            } else {
                h.set_gpr(cpu, 3, child);
                h.set_gpr(cpu, 4, option);
                call_guest(cpu, target, b.pane_animate, 1);
            }
        }
    }
}

// ---------- the entries: registers as the original takes them

template <class L>
void entry_pane_animate(Cpu* cpu) {
    const Host& h = host();
    pane_animate<L>(cpu, h.gpr(cpu, 3), h.gpr(cpu, 4));
}
template <class L>
void entry_pane_animate_self(Cpu* cpu) {
    const Host& h = host();
    pane_animate_self<L>(cpu, h.gpr(cpu, 3), h.gpr(cpu, 4));
}
template <class L>
void entry_material_animate(Cpu* cpu) {
    material_animate<L>(cpu, host().gpr(cpu, 3));
}
template <class L>
void entry_animate_pane(Cpu* cpu) {
    const Host& h = host();
    animate_pane<L>(cpu, h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5));
}
void entry_animate_material_2008(Cpu* cpu) {
    const Host& h = host();
    animate_material_2008(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5));
}

}  // namespace

// The 2007 build's AnimTransformBasic::Animate(u32, Material*) is not here: it
// swaps texture patterns through a TexMap built whole from the TPL (LOD, bias,
// filters), not ReplaceImage, and no 2007 game is at hand to check it against.
// Its walks still run natively and call that one through the vtable.
extern const Native kAnimationNatives[] = {
    WIINX_NATIVE("nw4r::lyt::Pane::Animate", kLyt_2007_06, entry_pane_animate<Layout2007>),
    WIINX_NATIVE("nw4r::lyt::Pane::Animate", kLyt_2008_03, entry_pane_animate<Layout2008>),
    WIINX_NATIVE("nw4r::lyt::Pane::Animate", kLyt_2009_04, entry_pane_animate<Layout2008>),
    WIINX_NATIVE("nw4r::lyt::Pane::AnimateSelf", kLyt_2007_06, entry_pane_animate_self<Layout2007>),
    WIINX_NATIVE("nw4r::lyt::Pane::AnimateSelf", kLyt_2008_03, entry_pane_animate_self<Layout2008>),
    WIINX_NATIVE("nw4r::lyt::Pane::AnimateSelf", kLyt_2009_04, entry_pane_animate_self<Layout2008>),
    WIINX_NATIVE("nw4r::lyt::Material::Animate", kLyt_2007_06, entry_material_animate<Layout2007>),
    WIINX_NATIVE("nw4r::lyt::Material::Animate", kLyt_2008_03, entry_material_animate<Layout2008>),
    WIINX_NATIVE("nw4r::lyt::Material::Animate", kLyt_2009_04, entry_material_animate<Layout2008>),
    WIINX_NATIVE("nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Pane*)", kLyt_2007_06, entry_animate_pane<Layout2007>),
    WIINX_NATIVE("nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Pane*)", kLyt_2008_03, entry_animate_pane<Layout2008>),
    WIINX_NATIVE("nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Pane*)", kLyt_2009_04, entry_animate_pane<Layout2008>),
    WIINX_NATIVE("nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Material*)", kLyt_2008_03, entry_animate_material_2008),
    WIINX_NATIVE("nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Material*)", kLyt_2009_04, entry_animate_material_2008),
};
extern const std::size_t kAnimationNativeCount = std::size(kAnimationNatives);

}  // namespace wiinx::nw4r::lyt
