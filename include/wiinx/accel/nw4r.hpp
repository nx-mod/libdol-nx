// nw4r - Nintendo's Wii middleware (layout, 3D, sound, effects).
#pragma once

#include "wiinx/builds.hpp"
#include "wiinx/core/native.hpp"

namespace wiinx::nw4r {

// The LYT (layout) builds natives here are written for.
inline constexpr const LibVersion& kLyt_2007_06 = builds::kNw4rLyt_2007_06_08;  // Wii Sports
inline constexpr const LibVersion& kLyt_2008_03 = builds::kNw4rLyt_2008_03_08;  // Mario Kart Wii

// Every nw4r native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

}  // namespace wiinx::nw4r
