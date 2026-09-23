#pragma once

// The console's identity: its serial, its region, its MAC address.
//
// A game reads these and expects them to be the same every time - saves and
// online records are tied to them - so they come from the NAND rather than
// being invented per run.

#if !__has_include("console_identity.h")
#error "<wiinx/platform/console.hpp> is part of the runtime: build with the platform section on the include path."
#endif

#include "console_identity.h"
#include "console_region.h"
