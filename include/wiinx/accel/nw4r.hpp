// nw4r - Nintendo's Wii middleware (layout, 3D, sound, effects).
#pragma once

#include "wiinx/core/builds.hpp"
#include "wiinx/core/native.hpp"

namespace wiinx::nw4r {

// The LYT (layout) builds natives here are written for.
inline constexpr const LibVersion& kLyt_2007_06 = builds::kNw4rLyt_2007_06_08;  // Wii Sports
inline constexpr const LibVersion& kLyt_2008_03 = builds::kNw4rLyt_2008_03_08;  // Mario Kart Wii

// Every nw4r native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;


namespace lyt {
// The guest address this game binds Pane::CalculateMtx at. A pane's children
// are reached through their vtables, and for an ordinary pane that entry is
// this native again: knowing its address lets the walk recurse here directly
// instead of leaving through the guest dispatcher and coming straight back.
// The binding sets it; zero means every child goes the long way.
void set_pane_calculate_mtx_address(GuestAddr address) noexcept;
}  // namespace lyt
}  // namespace wiinx::nw4r
