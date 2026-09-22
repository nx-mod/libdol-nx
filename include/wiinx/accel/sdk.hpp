// sdk - Nintendo's RVL SDK and the C library games link with it.
#pragma once

#include "wiinx/builds.hpp"
#include "wiinx/core/native.hpp"

namespace wiinx::sdk {

// The THP (movie) builds natives here are written for.
inline constexpr const LibVersion& kThp_2007_08 = builds::kRvlThp_2007_08_08;  // Mario Kart Wii

// Every sdk native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

}  // namespace wiinx::sdk
