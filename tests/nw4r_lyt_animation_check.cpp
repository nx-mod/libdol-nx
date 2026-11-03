// nw4r::lyt animation on a fake layout: a root pane and a child, an animation
// resource with every kind of target, a material with its GX block, a TPL -
// walked natively from the root, as each build lays them out.
#include "wiinx/accel/nw4r.hpp"
#include "wiinx/core/guest.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace wiinx;

namespace {
std::vector<u8> g_memory(0x20000, 0);
u32 g_gpr[32];

u32 gpr(Cpu*, int i) { return g_gpr[i]; }
void set_gpr(Cpu*, int i, u32 v) { g_gpr[i] = v; }

// a call out to "game code": which function, with what lr and arguments
struct Call {
    GuestAddr target, resume;
    u32 r3, r4, r5;
};
std::vector<Call> g_calls;
void call_from(Cpu*, GuestAddr target, GuestAddr resume) {
    g_calls.push_back({target, resume, g_gpr[3], g_gpr[4], g_gpr[5]});
}

void u32_at(u32 a, u32 v) { store<u32>(a, v); }
void f32_at(u32 a, f32 v) { store<f32>(a, v); }
u32 bits(f32 v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    return b;
}

// Guest "functions": where the bindings say the natives are, and where the
// game's own overrides would be.
constexpr GuestAddr kPaneAnimate = 0x9000, kPaneAnimateSelf = 0x9100, kMaterialAnimate = 0x9200,
                    kAnimatePane = 0x9300, kAnimateMaterial = 0x9400, kSetColorElement = 0x9500,
                    kGameAnimate = 0x9600;
constexpr GuestAddr kCalls[] = {0xA000, 0xA004};  // return addresses after each bctrl

constexpr GuestAddr kRoot = 0x1000, kChild = 0x1200, kMaterial = 0x1400, kGxMem = 0x1500, kTransform = 0x1600,
                    kLinks = 0x1700, kRes = 0x2000, kTpl = 0x4000, kFiles = 0x4800, kPaneVt = 0x8000,
                    kGamePaneVt = 0x8100, kMaterialVt = 0x8200, kTransformVt = 0x8300;

u32 at_res = kRes;
u32 put(u32 size) {
    const u32 here = at_res;
    at_res += (size + 3) & ~3u;
    return here;
}

// one info of `kind` with one target whose keys follow; returns the info
u32 info(u32 kind, u8 id, u8 target, bool step, std::initializer_list<f32> keys) {
    const u32 count = static_cast<u32>(keys.size()) / (step ? 2 : 3);
    const u32 inf = put(8 + 4);
    const u32 tgt = put(12);
    const u32 key = put(count * 12);
    u32_at(inf, kind);
    store<u8>(inf + 4, 1);
    u32_at(inf + 8, tgt - inf);
    store<u8>(tgt, id);
    store<u8>(tgt + 1, target);
    store<u8>(tgt + 2, step ? 1 : 2);
    store<u16>(tgt + 4, static_cast<u16>(count));
    u32_at(tgt + 8, key - tgt);
    u32 at = key;
    auto it = keys.begin();
    for (u32 i = 0; i < count; ++i) {
        if (step) {
            f32_at(at, *it++);
            store<u16>(at + 4, static_cast<u16>(*it++));
            at += 8;
        } else {
            f32_at(at, *it++);
            f32_at(at + 4, *it++);
            f32_at(at + 8, *it++);
            at += 12;
        }
    }
    return inf;
}

u32 content(std::initializer_list<u32> infos) {
    const u32 c = put(0x18 + 4 * static_cast<u32>(infos.size()));
    store<u8>(c + 0x14, static_cast<u8>(infos.size()));
    u32 i = 0;
    for (u32 inf : infos) {
        u32_at(c + 0x18 + 4 * i++, inf - c);
    }
    return c;
}

constexpr u32 kind(const char* k) { return u32(k[0]) << 24 | u32(k[1]) << 16 | u32(k[2]) << 8 | u32(k[3]); }

struct Layout {
    const char* version;
    u32 flag;               // the pane's flag byte
    u32 animatePane, animateMaterial;  // AnimTransform's vtable slots
};

void link(u32 node, u32 list) {  // a one-link list: list's node -> node -> back
    u32_at(list, node);
    u32_at(node, list);
}

void build(const Layout& l, bool childIsGames) {
    std::fill(g_memory.begin(), g_memory.end(), 0);
    at_res = kRes + 0x14;
    g_calls.clear();

    // vtables
    u32_at(kPaneVt + 0x1C, kPaneAnimate);
    u32_at(kPaneVt + 0x20, kPaneAnimateSelf);
    u32_at(kPaneVt + 0x30, kSetColorElement);
    u32_at(kGamePaneVt + 0x1C, kGameAnimate);  // a class of the game's own
    u32_at(kMaterialVt + 0x1C, kMaterialAnimate);
    u32_at(kTransformVt + l.animatePane, kAnimatePane);
    u32_at(kTransformVt + l.animateMaterial, kAnimateMaterial);

    // panes: root -> child; the root has the animation and the material
    for (u32 pane : {kRoot, kChild}) {
        u32_at(pane, kPaneVt);
        u32_at(pane + 0x14, pane + 0x14);  // no children
        u32_at(pane + 0x20, pane + 0x20);  // no animations
        store<u8>(pane + l.flag, 0x01);    // visible
    }
    if (childIsGames) {
        u32_at(kChild, kGamePaneVt);
    }
    link(kChild + 0x04, kRoot + 0x14);
    u32_at(kRoot + 0x28, kMaterial);
    link(kLinks, kRoot + 0x20);  // pane link: transform, content 0
    u32_at(kLinks + 0x08, kTransform);
    store<u16>(kLinks + 0x0C, 0);

    // material: cap/num texMap 1, texSRT 1, texCoordGen 1, chanCtrl 1, matCol 1
    const u32 nums = 1u << 28 | 1u << 24 | 1u << 20 | 1u << 8 | 1u << 7;
    u32_at(kMaterial, kMaterialVt);
    u32_at(kMaterial + 0x38, nums);
    u32_at(kMaterial + 0x3C, nums);
    u32_at(kMaterial + 0x40, kGxMem);
    link(kLinks + 0x10, kMaterial + 0x08);  // material link: transform, content 1
    u32_at(kLinks + 0x18, kTransform);
    store<u16>(kLinks + 0x1C, 1);
    u32_at(kGxMem + 0x18, 0xF0000000u | (1u << 13));  // a texmap of format 15, palette format 1

    // transform at frame 5
    u32_at(kTransform, kTransformVt);
    u32_at(kTransform + 0x0C, kRes);
    f32_at(kTransform + 0x10, 5.0f);
    u32_at(kTransform + 0x14, kFiles);

    // the resource: content 0 (pane), content 1 (material)
    const u32 pane = content({
        // translate x: 0 -> 10 over frames 0..10, slopes 1: a straight line
        info(kind("RLPA"), 0, 0, false, {0, 0, 1, 10, 10, 1}),
        // scale y (target 7): keys at 0, 5, 5 - frame 5 lands on the doubled key
        info(kind("RLPA"), 0, 7, false, {0, 1, 0, 5, 2, 0, 5, 3, 0}),
        // visibility off from frame 4
        info(kind("RLVI"), 0, 0, true, {0, 1, 4, 0}),
        // vertex colour: through the vtable
        info(kind("RLVC"), 0, 2, false, {0, 100, 0}),
    });
    const u32 mat = content({
        // material red: 299.7 + 0.5 -> 300 (s16) -> 255 (u8)
        info(kind("RLMC"), 0, 0, false, {0, 299.7f, 0}),
        // TEV colour 1 green (index 4 + 4 + 1): clamped to 1023
        info(kind("RLMC"), 0, 9, false, {0, 5000, 0}),
        // texture SRT 0, rotate (target 2)
        info(kind("RLTS"), 0, 2, false, {0, 45, 0}),
        // texture pattern: file 1 from frame 3
        info(kind("RLTP"), 0, 0, true, {0, 0, 3, 1}),
    });
    const u32 table = put(8);
    u32_at(table, pane - kRes);
    u32_at(table + 4, mat - kRes);
    u32_at(kRes + 0x10, table - kRes);

    // TPL: one descriptor, a texture of 64x32 format 14 at +0x100 and a
    // palette of 16 entries of format 2 at +0x80; still offsets (unbound)
    u32_at(kTpl + 0x00, 2142000);
    u32_at(kTpl + 0x04, 1);
    u32_at(kTpl + 0x08, 0x10);
    u32_at(kTpl + 0x10, 0x20);  // texture header offset
    u32_at(kTpl + 0x14, 0x60);  // palette header offset
    store<u16>(kTpl + 0x20, 32);
    store<u16>(kTpl + 0x22, 64);
    u32_at(kTpl + 0x24, 14);
    u32_at(kTpl + 0x28, 0x100);
    store<u16>(kTpl + 0x60, 16);
    u32_at(kTpl + 0x64, 2);
    u32_at(kTpl + 0x68, 0x80);
    u32_at(kFiles + 4, kTpl);

    set_bound("nw4r::lyt::Pane::Animate", kPaneAnimate, kCalls);
    set_bound("nw4r::lyt::Pane::AnimateSelf", kPaneAnimateSelf, kCalls);
    set_bound("nw4r::lyt::Material::Animate", kMaterialAnimate, kCalls);
    set_bound("nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Pane*)", kAnimatePane, {kCalls, 1});
    set_bound("nw4r::lyt::AnimTransformBasic::Animate(unsigned long, nw4r::lyt::Material*)", kAnimateMaterial, {});
}

void run(const Layout& l, bool materialNative) {
    build(l, false);
    const Native* native = find_native("nw4r::lyt::Pane::Animate", l.version);
    assert(native != nullptr);
    g_gpr[3] = kRoot;
    g_gpr[4] = 0;
    native->entry(nullptr);

    // pane: a straight line at frame 5 is 5, exactly
    assert(bits(load<f32>(kRoot + 0x2C)) == bits(5.0f));
    // a frame on a doubled key takes the later key's value
    assert(load<f32>(kRoot + 0x2C + 7 * 4) == 3.0f);
    // invisible from frame 4, the other flag bits kept
    assert((load<u8>(kRoot + l.flag) & 1) == 0);
    // the vertex colour went out through the pane's vtable, from the
    // transform's one bctrl, with 100.5 truncated to 100
    std::size_t outs = 0;
    for (const Call& c : g_calls) {
        if (c.target == kSetColorElement) {
            assert(c.resume == kCalls[0] && c.r3 == kRoot && c.r4 == 2 && c.r5 == 100);
            ++outs;
        }
    }
    assert(outs == 1);

    if (materialNative) {
        // material colour: 300 clamped into a byte
        const u32 matCol = kGxMem + 28 + 20 + 4 + 4;
        assert(load<u8>(matCol) == 255);
        // TEV colour 1 green, clamped to the register's 10 bits
        assert(load<s16>(kMaterial + 0x10 + 1 * 8 + 1 * 2) == 1023);
        // texture SRT 0 rotation
        assert(load<f32>(kGxMem + 28 + 8) == 45.0f);
        // the pattern swapped in file 1, the TPL bound in place on the way
        assert(load<u32>(kTpl + 0x08) == kTpl + 0x10);
        assert(load<u32>(kGxMem + 0x0) == kTpl + 0x100);
        assert(load<u32>(kGxMem + 0x4) == kTpl + 0x80);
        assert(load<u16>(kGxMem + 0x8) == 64 && load<u16>(kGxMem + 0xA) == 32);
        assert(load<u16>(kGxMem + 0x16) == 16);
        assert(load<u32>(kGxMem + 0x18) == (14u << 28 | 2u << 13));
    } else {
        // no native for this build's material animation: out through the
        // vtable from Material::Animate's first bctrl
        bool seen = false;
        for (const Call& c : g_calls) {
            if (c.target == kAnimateMaterial) {
                assert(c.resume == kCalls[0] && c.r3 == kTransform && c.r4 == 1 && c.r5 == kMaterial);
                seen = true;
            }
        }
        assert(seen);
    }

    // the walk reached the child, which has nothing to animate, and went
    // nowhere else
    for (const Call& c : g_calls) {
        assert(c.target == kSetColorElement || c.target == kAnimateMaterial);
    }

    // a child of the game's own class is animated by the game, from
    // Pane::Animate's second bctrl
    build(l, true);
    g_gpr[3] = kRoot;
    g_gpr[4] = 0;
    native->entry(nullptr);
    bool child = false;
    for (const Call& c : g_calls) {
        if (c.target == kGameAnimate) {
            assert(c.resume == kCalls[1] && c.r3 == kChild && c.r4 == 0);
            child = true;
        }
    }
    assert(child);

    // an invisible pane's children are skipped when the option says so
    build(l, true);
    store<u8>(kRoot + l.flag, 0);
    u32_at(kRoot + 0x20, kRoot + 0x20);  // (no animation to make it visible again)
    g_gpr[3] = kRoot;
    g_gpr[4] = 1;
    native->entry(nullptr);
    for (const Call& c : g_calls) {
        assert(c.target != kGameAnimate);
    }
    std::printf("%s ok\n", l.version);
}
}  // namespace

int main() {
    Host h;
    h.memory = g_memory.data();
    h.gpr = gpr;
    h.set_gpr = set_gpr;
    h.call_from = call_from;
    set_host(h);
    add_natives(nw4r::natives());

    run({"nw4r.lyt@2007-06-08", 0xB7, 0x18, 0x1C}, false);
    run({"nw4r.lyt@2008-03-08", 0xBB, 0x1C, 0x20}, true);
    run({"nw4r.lyt@2009-04-02", 0xBB, 0x1C, 0x20}, true);

    // DrawQuad, the alpha form: two textures, colours with their alpha scaled
    std::vector<u8> fifo;
    static std::vector<u8>* sink;
    sink = &fifo;
    Host q = h;
    q.gx_write = [](const u8* data, u32 size) { sink->insert(sink->end(), data, data + size); };
    set_host(q);
    set_bound("GXBegin", 0x9700, {});
    constexpr DirectCall begin[] = {{0x9700, 0xA100}};
    set_bound("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, unsigned char, "
              "const nw4r::math::VEC2[4]*, const nw4r::ut::Color*)", 0x9800, {}, begin);
    std::fill(g_memory.begin(), g_memory.end(), 0);
    g_calls.clear();
    f32_at(0x100, 10.0f);  // base
    f32_at(0x104, 20.0f);
    f32_at(0x108, 4.0f);   // size
    f32_at(0x10C, 6.0f);
    for (u32 i = 0; i < 16; ++i) {
        f32_at(0x200 + i * 4, static_cast<f32>(i));  // two textures' four corners
    }
    for (u32 i = 0; i < 4; ++i) {
        u32_at(0x300 + i * 4, 0x10203000u | (0xC0 + i));
    }
    const Native* quad = find_native("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, "
                                     "unsigned char, const nw4r::math::VEC2[4]*, const nw4r::ut::Color*, unsigned char)",
                                     "nw4r.lyt@2008-03-08");
    assert(quad != nullptr);
    g_gpr[3] = 0x100;
    g_gpr[4] = 0x108;
    g_gpr[5] = 2;
    g_gpr[6] = 0x200;
    g_gpr[7] = 0x300;
    g_gpr[8] = 128;
    quad->entry(nullptr);
    assert(g_calls.size() == 1 && g_calls[0].target == 0x9700 && g_calls[0].resume == 0xA100 && g_calls[0].r3 == 0x80);
    // four vertices of x, y, colour and two coordinate pairs: 28 bytes
    assert(fifo.size() == 4 * 28);
    const auto word = [&](size_t at) {
        return u32(fifo[at]) << 24 | u32(fifo[at + 1]) << 16 | u32(fifo[at + 2]) << 8 | fifo[at + 3];
    };
    // right bottom (the third vertex): x + width, y - height (2008 subtracts),
    // colour 3 with alpha 0xC3 * 128 / 255, texture 0's +24 and texture 1's
    assert(word(56) == bits(14.0f) && word(60) == bits(14.0f));
    assert(word(64) == (0x10203000u | (0xC3u * 128 / 255)));
    assert(word(68) == bits(6.0f) && word(72) == bits(7.0f) && word(76) == bits(14.0f) && word(80) == bits(15.0f));
    // the 2007 build adds the height
    const Native* quad2007 = find_native("nw4r::lyt::detail::DrawQuad(const nw4r::math::VEC2&, const nw4r::lyt::Size&, "
                                         "unsigned char, const nw4r::math::VEC2[4]*, const nw4r::ut::Color*)",
                                         "nw4r.lyt@2007-06-08");
    fifo.clear();
    // (the arguments again: the call to GXBegin left its own in r3..r5, as it
    // would on the console)
    g_gpr[3] = 0x100;
    g_gpr[4] = 0x108;
    g_gpr[5] = 2;
    g_gpr[6] = 0x200;
    g_gpr[7] = 0;  // no colours
    quad2007->entry(nullptr);
    assert(fifo.size() == 4 * 24 && word(48 + 4) == bits(26.0f));
    std::printf("DrawQuad ok\n");
}
