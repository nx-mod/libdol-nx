#pragma once

// What this build is.
//
// Each executable links one small descriptor: what it calls itself, the folder
// its logs and saves sit under, whether it overlays the disc with a mod's files,
// and anything it wants in the guest's low memory before the game boots. Keeping
// the choice out of compiler definitions lets the runtime be built once and
// shared by every product.

#if !__has_include("runtime_product.h")
#error "<wiinx/platform/product.hpp> is part of the runtime: build with the platform section on the include path."
#endif

#include "runtime_product.h"
