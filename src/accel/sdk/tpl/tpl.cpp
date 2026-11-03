// TPL - the SDK's texture palette files.
//
// A TPL is loaded as it sits on disc: a version, a count and a table of
// descriptors whose headers and image data are offsets from the file's start.
// TPLBind turns the offsets into pointers once, in place; TPLGet picks a
// descriptor. nw4r's layouts call both whenever a texture-pattern animation
// swaps an image, so the lyt natives use them here rather than calling back.
//
// Logic follows the ogws decompilation (doldecomp/ogws, CC0: TPL.c). Offsets
// read off New Super Mario Bros. Wii's TPLBind, which keeps the palette
// header's "unpacked" flag at +2 (the decompilation's header says +1; the code
// both games ship reads and writes +2).
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"

namespace wiinx::sdk::tpl {
namespace {

constexpr u32 kVersion = 2142000;

struct Palette {
    static constexpr Field<u32, 0x0> version{};
    static constexpr Field<u32, 0x4> count{};
    static constexpr Field<u32, 0x8> descriptors{};  // an offset until bound
};
constexpr u32 kDescriptorSize = 8;  // the texture header, then the palette's
struct Header {
    static constexpr Field<u32, 0x8> data{};
    static constexpr Field<u8, 0x23> unpacked{};
};
struct ClutHeader {
    static constexpr Field<u32, 0x8> data{};
    static constexpr Field<u8, 0x2> unpacked{};
};

}  // namespace

void bind(GuestAddr palette) noexcept {
    const Guest<Palette> pal{palette};
    if (pal[Palette::version] != kVersion) {
        // the original stops the console here (OSPanic, "invalid version
        // number for texture palette"); binding a file that is not a TPL would
        // only rewrite whatever it is
        if (const Host& h = host(); h.log != nullptr) {
            h.log("[tpl] TPLBind: not a texture palette (version), left alone");
        }
        return;
    }
    pal.set(Palette::descriptors, pal[Palette::descriptors] + palette);
    const u32 count = pal[Palette::count];
    // (the original counts in a u16)
    for (u16 i = 0; i < count; ++i) {
        const GuestAddr descriptor = pal[Palette::descriptors] + i * kDescriptorSize;
        if (const u32 header = load<u32>(descriptor); header != 0) {
            const Guest<Header> texture{header + palette};
            store<u32>(descriptor, texture.addr);
            if (texture[Header::unpacked] == 0) {
                texture.set(Header::data, texture[Header::data] + palette);
                texture.set(Header::unpacked, u8{1});
            }
        }
        if (const u32 header = load<u32>(descriptor + 4); header != 0) {
            const Guest<ClutHeader> clut{header + palette};
            store<u32>(descriptor + 4, clut.addr);
            if (clut[ClutHeader::unpacked] == 0) {
                clut.set(ClutHeader::data, clut[ClutHeader::data] + palette);
                clut.set(ClutHeader::unpacked, u8{1});
            }
        }
    }
}

GuestAddr get(GuestAddr palette, u32 id) noexcept {
    const Guest<Palette> pal{palette};
    const u32 count = pal[Palette::count];
    return count == 0 ? 0 : pal[Palette::descriptors] + (id % count) * kDescriptorSize;
}

}  // namespace wiinx::sdk::tpl
