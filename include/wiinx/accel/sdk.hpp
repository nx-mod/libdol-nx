// sdk - Nintendo's RVL SDK and the C library games link with it.
#pragma once

#include "wiinx/builds.hpp"
#include "wiinx/core/native.hpp"

namespace wiinx::sdk {

// The THP (movie) builds natives here are written for.
inline constexpr const LibVersion& kThp_2007_08 = builds::kRvlThp_2007_08_08;  // Mario Kart Wii

// Every sdk native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

namespace thp {
// THPVideoDecode(file, tileY, tileU, tileV, work) for callers that already
// have the arguments unpacked - the runtime binding, a test.
int32_t decode_frame(uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work);
}  // namespace thp

}  // namespace wiinx::sdk
