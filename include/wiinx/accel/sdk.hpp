// sdk - Nintendo's RVL SDK and the C library games link with it.
#pragma once

#include "wiinx/core/builds.hpp"
#include "wiinx/core/native.hpp"

namespace wiinx::sdk {

// The THP (movie) builds natives here are written for.
inline constexpr const LibVersion& kThp_2007_08 = builds::kRvlThp_2007_08_08;  // Mario Kart Wii

// MTX is hand-written assembly in the SDK and identical in every build of it,
// like the cache and interrupt primitives, so its natives are not written for
// one: a game's bindings say where the functions are, and the code is the same
// wherever they are.
inline constexpr LibVersion kMtx_Any{"rvl.mtx@any", "rvl.mtx", "any build"};

// The C library the game's compiler linked. Its semantics are the standard's,
// so a native serves every game whatever compiler wrote it.
inline constexpr LibVersion kMsl_Any{"msl@any", "msl", "any build"};

// The GX build natives here are written for: Mario Kart Wii's, shared with
// Pikmin 2's Wii release and a few channels (builds.hpp lists them).
inline constexpr const LibVersion& kGx_2007_12 = builds::kRvlGx_2007_12_11;

// Every sdk native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

namespace mtx {
// The SDK's paired-single matrix library, for callers that already have the
// addresses unpacked (the runtime binding, a test).
void concat_at(GuestAddr a, GuestAddr b, GuestAddr ab) noexcept;
}  // namespace mtx

namespace gx {
// GX texture objects, for callers that already have the arguments unpacked (a
// test, the runtime). Registers are passed whole, as the original uses them.
void init_tex_obj(GuestAddr obj, u32 image, u32 width, u32 height, u32 format, u32 wrapS, u32 wrapT,
                  u32 mipmap) noexcept;
void init_tex_obj_ci(GuestAddr obj, u32 image, u32 width, u32 height, u32 format, u32 wrapS, u32 wrapT,
                     u32 mipmap, u32 tlut) noexcept;
void init_tex_obj_lod(GuestAddr obj, u32 minFilter, u32 magFilter, f64 minLod, f64 maxLod, f64 lodBias,
                      u32 biasClamp, u32 edgeLod, u32 maxAniso) noexcept;
// these send registers and note them in gxData; the region callbacks are the
// game's, called on `cpu`
void load_tex_obj_preloaded(Cpu* cpu, GuestAddr obj, GuestAddr region, u32 map) noexcept;
void load_tex_obj(Cpu* cpu, GuestAddr obj, u32 map) noexcept;
}  // namespace gx

namespace thp {
// THPVideoDecode(file, tileY, tileU, tileV, work) for callers that already
// have the arguments unpacked - the runtime binding, a test.
int32_t decode_frame(uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work);
}  // namespace thp

}  // namespace wiinx::sdk
