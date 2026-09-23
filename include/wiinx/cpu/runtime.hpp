#pragma once

// The CPU context, and how a guest function is entered.
//
// Translated code is ordinary C++ functions taking this context; the runtime
// enters them, and natives receive the same context as an opaque `wiinx::Cpu*`.

#if !__has_include("ppc_runtime.h")
#error "<wiinx/cpu/runtime.hpp> is part of the runtime: build with the cpu section on the include path."
#endif

#include "ppc_runtime.h"
