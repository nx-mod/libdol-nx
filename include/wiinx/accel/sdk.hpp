// sdk - Nintendo's RVL SDK and the C library games link with it.
#pragma once

#include "wiinx/builds.hpp"
#include "wiinx/core/native.hpp"

namespace wiinx::sdk {

// The THP (movie) builds natives here are written for.
inline constexpr const LibVersion& kThp_2007_08 = builds::kRvlThp_2007_08_08;  // Mario Kart Wii

// MTX is hand-written assembly in the SDK and identical in every build of it,
// like the cache and interrupt primitives, so its natives are not written for
// one: a game's bindings say where the functions are, and the code is the same
// wherever they are.
inline constexpr LibVersion kMtx_Any{"rvl.mtx@any", "rvl.mtx", "any build"};

// Every sdk native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

namespace mtx {
// The SDK's paired-single matrix library, for callers that already have the
// addresses unpacked (the runtime binding, a test).
void concat_at(GuestAddr a, GuestAddr b, GuestAddr ab) noexcept;
}  // namespace mtx

namespace thp {
// THPVideoDecode(file, tileY, tileU, tileV, work) for callers that already
// have the arguments unpacked - the runtime binding, a test.
int32_t decode_frame(uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work);
}  // namespace thp

}  // namespace wiinx::sdk
