#pragma once

// egg - Nintendo EAD's framework, the layer their Wii games are written on top
// of the SDK.

#include "wiinx/core/builds.hpp"
#include "wiinx/core/native.hpp"

namespace wiinx::egg {

// EGG is compiled into each game rather than linked as a versioned library, so
// there is no banner to match a build against. The natives here are the ones
// whose behaviour is the same in every game that carries them.
inline constexpr LibVersion kEgg_Any{"egg@any", "egg", "any build"};

// Every egg native, for wiinx::add_natives().
std::span<const Native> natives() noexcept;

namespace decomp {

// Yaz0 ("SZS"), the compression every EAD game stores its archives in. Reads a
// Yaz0 stream at `src` and writes the expanded bytes at `dst`, returning the
// expanded size, or 0 when the stream is malformed.
//
// The original trusts its input. This does not: a back-reference that points
// before the output, or a run that would write past the size the header
// declares, stops the decode instead of walking through the guest's memory.
u32 decode_szs(GuestAddr src, GuestAddr dst) noexcept;

}  // namespace decomp
}  // namespace wiinx::egg
