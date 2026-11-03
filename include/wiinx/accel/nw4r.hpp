// nw4r - Nintendo's Wii middleware (layout, 3D, sound, effects).
#pragma once

#include "wiinx/core/builds.hpp"
#include "wiinx/core/native.hpp"

namespace wiinx::nw4r {

// The LYT (layout) builds natives here are written for.
inline constexpr const LibVersion& kLyt_2007_06 = builds::kNw4rLyt_2007_06_08;  // Wii Sports
inline constexpr const LibVersion& kLyt_2008_03 = builds::kNw4rLyt_2008_03_08;  // Mario Kart Wii
inline constexpr const LibVersion& kLyt_2009_04 = builds::kNw4rLyt_2009_04_02;  // New Super Mario Bros. Wii

// nw4r's utility library (text, fonts, lists) has no banner of its own, and
// the copies read so far agree field for field (2007-11, 2009-04): its natives
// are written for any build, and its signatures' field fingerprints keep one
// laid out otherwise from binding.
inline constexpr LibVersion kUt_Any{"nw4r.ut@any", "nw4r.ut", "any build"};

// Every nw4r native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

}  // namespace wiinx::nw4r
