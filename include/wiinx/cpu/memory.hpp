#pragma once

// The guest's address space.
//
// Natives should reach guest memory through <wiinx/core/guest.hpp> and the host
// interface instead: it resolves an address once and works in a test with no
// runtime at all. This is for the runtime itself and for code that has to know
// how the space is laid out.

#if !__has_include("memory.h")
#error "<wiinx/cpu/memory.hpp> is part of the runtime: build with the cpu section on the include path."
#endif

#include "memory.h"
