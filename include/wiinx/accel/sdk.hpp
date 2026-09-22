// sdk - Nintendo's RVL SDK and the C library games link with it.
#pragma once

#include "wiinx/core/native.hpp"

namespace wiinx::sdk {

// RVL SDK THP (movies), named as its startup banner names it:
//     << RVL_SDK - THP  release build: Aug  8 2007 01:31:54 (0x4199_60831) >>
inline constexpr LibVersion kThp_2007_08{"rvl.thp@2007-08-08", "rvl.thp", "Aug  8 2007 (0x4199_60831)"};

// Every sdk native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

}  // namespace wiinx::sdk
