#pragma once

// Where the guest resumes when a native calls back into it.
//
// A native that replaces a guest function and then calls guest code has to
// leave the link register pointing where the original would have: some callees
// are dispatched as a jump and resume through it. The address belongs to the
// game, so the binding supplies it and the native stays game-agnostic.

#if !__has_include("native_guest_return.h")
#error "<wiinx/cpu/guest_return.hpp> is part of the runtime: build with the cpu section on the include path."
#endif

#include "native_guest_return.h"
