#pragma once

// Which physical control answers which guest button.
//
// A binding is an expression over the host's controls, so a player can map one
// stick to two guest inputs, or a chord to a button the console had.

#if !__has_include("input_bindings.h")
#error "<wiinx/platform/input.hpp> is part of the runtime: build with the platform section on the include path."
#endif

#include "input_bindings.h"
