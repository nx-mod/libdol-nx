// nw4r - Nintendo's Wii middleware (layout, 3D, sound, effects).
#pragma once

#include "wiinx/core/native.hpp"

namespace wiinx::nw4r {

// The builds of nw4r's LYT (layout) library natives here know, named as the
// game's own startup banner names them.
inline constexpr LibVersion kLyt_2007_06{"nw4r.lyt@2007-06-08", "nw4r.lyt", "Jun  8 2007 (0x4199_60831)"};
inline constexpr LibVersion kLyt_2008_03{"nw4r.lyt@2008-03-08", "nw4r.lyt", "Mar  8 2008 (0x4201_127)"};

// Every nw4r native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

}  // namespace wiinx::nw4r
