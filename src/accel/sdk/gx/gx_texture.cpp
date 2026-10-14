// GX textures, natively: GXInitTexObj, GXInitTexObjCI, GXInitTexObjLOD, and
// GXLoadTexObj / GXLoadTexObjPreLoaded.
//
// The Init functions only fill in the game's own GXTexObj (32 bytes, in guest
// memory): no GX state and no FIFO, so each stands alone. The Load functions
// send the object's registers and note them in gxData (gx_internal.hpp). They are among the hottest SDK GX functions in
// a Mario Kart Wii menu frame (every material sets its textures up each time).
//
// Written from Mario Kart Wii's own code - doldecomp/mkw lib/rvl/gx/gxTexture.c
// (CC0), the PAL build's instructions at 0x801707F8.. - with the field meanings
// from doldecomp/ogws and projectPiki/pikmin2's GX (CC0). The tables and
// constants are the game's: read from its main.dol, not assumed. Results are
// the original's bit for bit, odd inputs included: each register is used as the
// original uses it (whole, where it compares or counts the whole register).
#include "gx_internal.hpp"
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace wiinx::sdk::gx {
namespace {

// GXTexObj, as this build lays it out
constexpr u32 kMode0 = 0x00;      // wrap S/T, filters, LOD bias, edge LOD, clamp, anisotropy
constexpr u32 kMode1 = 0x04;      // min and max LOD, 4.4 fixed point
constexpr u32 kImage0 = 0x08;     // width - 1, height - 1, format
constexpr u32 kImage3 = 0x0C;     // the image's address >> 5
constexpr u32 kFormat = 0x14;     // the format as passed
constexpr u32 kTlut = 0x18;       // GXInitTexObjCI's palette
constexpr u32 kLoadCount = 0x1C;  // u16: TMEM tiles a load takes
constexpr u32 kLoadFormat = 0x1E; // u8: the tile shape class
constexpr u32 kFlags = 0x1F;      // u8: 1 mipmapped, 2 not paletted
constexpr u32 kTexObjSize = 0x20;

// GX's min filters as the hardware numbers them (the game's table at
// 0x80385980: GX_NEAR, GX_LINEAR, GX_NEAR_MIP_NEAR, GX_LIN_MIP_NEAR,
// GX_NEAR_MIP_LIN, GX_LIN_MIP_LIN)
constexpr u8 kHwMinFilter[] = {0, 4, 1, 5, 2, 6, 0, 0};

// What each format's low nibble loads as: the tile class and the tile's width
// and height as shifts (the game's jump table at 0x8028A580; nibbles 7, 11-13
// take the default)
struct TileShape {
    u8 loadFormat, shiftS, shiftT;
};
constexpr TileShape kTileShape[16] = {
    {1, 3, 3}, {2, 3, 2}, {2, 3, 2}, {2, 2, 2}, {2, 2, 2}, {2, 2, 2}, {3, 2, 2}, {2, 2, 2},
    {1, 3, 3}, {2, 3, 2}, {2, 2, 2}, {2, 2, 2}, {2, 2, 2}, {2, 2, 2}, {0, 3, 3}, {2, 2, 2},
};

// PowerPC's fctiwz: toward zero, saturating, NaN as the most negative int
s32 fctiwz(f32 value) noexcept {
    if (std::isnan(value)) {
        return INT32_MIN;
    }
    if (value >= 2147483648.0f) {
        return INT32_MAX;
    }
    if (value <= -2147483648.0f) {
        return INT32_MIN;
    }
    return static_cast<s32>(value);
}

using detail::insert;

u32 cntlzw(u32 value) noexcept { return value ? static_cast<u32>(__builtin_clz(value)) : 32u; }

// a 0..10 LOD, clamped as the original clamps it (NaN to 0), as 4.4 fixed
// point. The argument is the register's double, multiplied and only then
// rounded to single, as fmuls does: a double that is not a single rounds once.
s32 lod_fixed(f64 lod) noexcept {
    if (!(lod >= 0.0)) {
        lod = 0.0;
    } else if (lod > 10.0) {
        lod = 10.0;
    }
    return fctiwz(static_cast<f32>(16.0 * lod));
}

}  // namespace

void init_tex_obj(GuestAddr obj, u32 image, u32 width, u32 height, u32 format, u32 wrapS, u32 wrapT,
                  u32 mipmap) noexcept {
    for (u32 i = 0; i < kTexObjSize; i += 4) {
        store<u32>(obj + i, 0);
    }
    u32 mode0 = insert(0, wrapS, 0, 0x3);
    mode0 = insert(mode0, wrapT, 2, 0xC);
    mode0 |= 0x10;  // magnification linear
    if (mipmap != 0) {
        store<u8>(obj + kFlags, static_cast<u8>(load<u8>(obj + kFlags) | 1));
        // paletted formats filter their mips nearest-to-linear, the rest linear
        mode0 = insert(mode0, (format - 8) <= 2 ? 5u : 6u, 5, 0xE0);
        // the largest level's LOD: log2 of the longer side, as an unsigned
        // integer converted to float (the original's 0x4330 trick), times 16
        const u32 longer = width > height ? width : height;
        const u32 log2 = 31u - cntlzw(longer);
        const f32 lod = static_cast<f32>(static_cast<f64>(log2));
        store<u32>(obj + kMode1, insert(load<u32>(obj + kMode1), static_cast<u32>(fctiwz(16.0f * lod)), 8, 0xFF00));
    } else {
        mode0 = insert(mode0, 4u, 5, 0xE0);  // linear, no mips
    }
    store<u32>(obj + kMode0, mode0);

    u32 image0 = insert(0, width - 1, 0, 0x3FF);
    image0 = insert(image0, height - 1, 10, 0xFFC00);
    image0 = insert(image0, format, 20, 0xF00000);
    store<u32>(obj + kFormat, format);
    store<u32>(obj + kImage3, insert(load<u32>(obj + kImage3), image, 27, 0x00FFFFFF));
    store<u32>(obj + kImage0, image0);

    const TileShape& shape = kTileShape[format & 0xF];
    store<u8>(obj + kLoadFormat, shape.loadFormat);
    store<u8>(obj + kFlags, static_cast<u8>(load<u8>(obj + kFlags) | 2));
    const s32 tilesS = static_cast<s32>(width + (1u << shape.shiftS) - 1) >> shape.shiftS;
    const s32 tilesT = static_cast<s32>(height + (1u << shape.shiftT) - 1) >> shape.shiftT;
    store<u16>(obj + kLoadCount, static_cast<u16>(static_cast<u32>(tilesS * tilesT) & 0x7FFF));
}

void init_tex_obj_ci(GuestAddr obj, u32 image, u32 width, u32 height, u32 format, u32 wrapS, u32 wrapT,
                     u32 mipmap, u32 tlut) noexcept {
    init_tex_obj(obj, image, width, height, format, wrapS, wrapT, mipmap);
    store<u32>(obj + kTlut, tlut);
    store<u8>(obj + kFlags, static_cast<u8>(load<u8>(obj + kFlags) & ~2u));
}

void init_tex_obj_lod(GuestAddr obj, u32 minFilter, u32 magFilter, f64 minLod, f64 maxLod, f64 lodBias,
                      u32 biasClamp, u32 edgeLod, u32 maxAniso) noexcept {
    // the bias: below -4 (or NaN) is -4, 4 and above is 3.99 (the game's
    // single-precision 3.99)
    if (!(lodBias >= -4.0)) {
        lodBias = -4.0;
    } else if (lodBias >= 4.0) {
        lodBias = static_cast<f64>(3.99f);
    }
    u32 mode0 = load<u32>(obj + kMode0);
    mode0 = insert(mode0, static_cast<u32>(fctiwz(static_cast<f32>(32.0 * lodBias))), 9, 0x1FE00);
    mode0 = insert(mode0, cntlzw(magFilter - 1), 31, 0x10);  // linear iff magFilter is 1
    // (the table is indexed by the whole register, as in the original)
    mode0 = insert(mode0, minFilter < sizeof(kHwMinFilter) ? kHwMinFilter[minFilter] : 0u, 5, 0xE0);
    mode0 = insert(mode0, cntlzw(edgeLod), 3, 0x100);  // set iff edgeLod is 0
    mode0 &= ~0x00060000u;
    mode0 = insert(mode0, maxAniso, 19, 0x180000);
    mode0 = insert(mode0, biasClamp, 21, 0x200000);
    store<u32>(obj + kMode0, mode0);

    u32 mode1 = load<u32>(obj + kMode1);
    mode1 = insert(mode1, static_cast<u32>(lod_fixed(minLod)), 0, 0xFF);
    mode1 = insert(mode1, static_cast<u32>(lod_fixed(maxLod)), 8, 0xFF00);
    store<u32>(obj + kMode1, mode1);
}

namespace {
// The BP registers a texture map's words go to (the game's tables at
// r13 - 29368 .. - 29320, read from its main.dol): maps 0-3, then 4-7
constexpr u8 kRegMode0[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
constexpr u8 kRegMode1[8] = {0x84, 0x85, 0x86, 0x87, 0xA4, 0xA5, 0xA6, 0xA7};
constexpr u8 kRegImage0[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
constexpr u8 kRegImage1[8] = {0x8C, 0x8D, 0x8E, 0x8F, 0xAC, 0xAD, 0xAE, 0xAF};
constexpr u8 kRegImage2[8] = {0x90, 0x91, 0x92, 0x93, 0xB0, 0xB1, 0xB2, 0xB3};
constexpr u8 kRegImage3[8] = {0x94, 0x95, 0x96, 0x97, 0xB4, 0xB5, 0xB6, 0xB7};
constexpr u8 kRegTlut[8] = {0x98, 0x99, 0x9A, 0x9B, 0xB8, 0xB9, 0xBA, 0xBB};

// a register word with its register number in the top byte
u32 with_reg(u32 word, u8 reg) noexcept { return insert(word, reg, 24, 0xFF000000); }

// Calls a guest callback with up to two arguments and returns its r3.
u32 call_guest(Cpu* cpu, GuestAddr target, u32 a, u32 b) noexcept {
    const Host& h = host();
    h.set_gpr(cpu, 3, a);
    h.set_gpr(cpu, 4, b);
    h.call(cpu, target);
    return h.gpr(cpu, 3);
}
}  // namespace

void load_tex_obj_preloaded(Cpu* cpu, GuestAddr obj, GuestAddr region, u32 map) noexcept {
    using L = detail::GxData2007;
    map &= 7;  // (the original indexes its tables by the whole register; a map is 0-7)
    const u32 mode0 = with_reg(load<u32>(obj + kMode0), kRegMode0[map]);
    const u32 mode1 = with_reg(load<u32>(obj + kMode1), kRegMode1[map]);
    const u32 image0 = with_reg(load<u32>(obj + kImage0), kRegImage0[map]);
    const u32 image1 = with_reg(load<u32>(region + 0), kRegImage1[map]);
    const u32 image2 = with_reg(load<u32>(region + 4), kRegImage2[map]);
    const u32 image3 = with_reg(load<u32>(obj + kImage3), kRegImage3[map]);
    detail::Fifo fifo;
    fifo.bp(mode0);
    fifo.bp(mode1);
    fifo.bp(image0);
    fifo.bp(image1);
    fifo.bp(image2);
    // (the register numbers stay in the object and the region, as the original leaves them)
    store<u32>(obj + kMode0, mode0);
    store<u32>(obj + kMode1, mode1);
    store<u32>(obj + kImage0, image0);
    store<u32>(region + 0, image1);
    store<u32>(region + 4, image2);
    store<u32>(obj + kImage3, image3);
    fifo.bp(image3);
    const GuestAddr gx = detail::data();
    if (!(load<u8>(obj + kFlags) & 2) && gx != 0) {
        // paletted: the palette's region from the game's callback, and its register
        fifo.flush();  // (in order, whatever the callback does)
        const GuestAddr tlutRegion =
            call_guest(cpu, load<u32>(gx + L::kTlutRegionCallback), load<u32>(obj + kTlut), 0);
        const u32 tlut = with_reg(load<u32>(tlutRegion + 4), kRegTlut[map]);
        store<u32>(tlutRegion + 4, tlut);
        fifo.bp(tlut);
    }
    if (gx != 0) {
        store<u32>(gx + L::kTImage0 + map * 4, image0);
        store<u32>(gx + L::kTMode0 + map * 4, mode0);
        store<u32>(gx + L::kDirtyState, load<u32>(gx + L::kDirtyState) | 1);
        store<u16>(gx + L::kBpSentNot, 0);
    }
}

void load_tex_obj(Cpu* cpu, GuestAddr obj, u32 map) noexcept {
    const GuestAddr gx = detail::data();
    if (gx == 0) {
        return;
    }
    // the region the game's callback gives this object
    const GuestAddr region = call_guest(cpu, load<u32>(gx + detail::GxData2007::kTexRegionCallback), obj, map);
    load_tex_obj_preloaded(cpu, obj, region, map);
}

namespace {

void GXLoadTexObjPreLoaded(Cpu* cpu) {
    const Host& h = host();
    load_tex_obj_preloaded(cpu, h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5));
}

void GXLoadTexObj(Cpu* cpu) {
    const Host& h = host();
    load_tex_obj(cpu, h.gpr(cpu, 3), h.gpr(cpu, 4));
}

void GXInitTexObj(Cpu* cpu) {
    const Host& h = host();
    init_tex_obj(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5), h.gpr(cpu, 6), h.gpr(cpu, 7), h.gpr(cpu, 8),
                 h.gpr(cpu, 9), h.gpr(cpu, 10));
}

void GXInitTexObjCI(Cpu* cpu) {
    const Host& h = host();
    // the ninth argument is on the stack, eight bytes above the caller's r1
    init_tex_obj_ci(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5), h.gpr(cpu, 6), h.gpr(cpu, 7), h.gpr(cpu, 8),
                    h.gpr(cpu, 9), h.gpr(cpu, 10), load<u32>(h.gpr(cpu, 1) + 8));
}

void GXInitTexObjLOD(Cpu* cpu) {
    const Host& h = host();
    // (the float arguments as the registers hold them: doubles)
    init_tex_obj_lod(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5), h.fpr(cpu, 1), h.fpr(cpu, 2), h.fpr(cpu, 3),
                     h.gpr(cpu, 6), h.gpr(cpu, 7), h.gpr(cpu, 8));
}

}  // namespace

extern const Native kGxNatives[] = {
    WIINX_NATIVE("GXInitTexObj", kGx_2007_12, GXInitTexObj),
    WIINX_NATIVE("GXInitTexObjCI", kGx_2007_12, GXInitTexObjCI),
    WIINX_NATIVE("GXInitTexObjLOD", kGx_2007_12, GXInitTexObjLOD),
    WIINX_NATIVE("GXLoadTexObjPreLoaded", kGx_2007_12, GXLoadTexObjPreLoaded),
    WIINX_NATIVE("GXLoadTexObj", kGx_2007_12, GXLoadTexObj),
};
extern const std::size_t kGxNativeCount = std::size(kGxNatives);

}  // namespace wiinx::sdk::gx
