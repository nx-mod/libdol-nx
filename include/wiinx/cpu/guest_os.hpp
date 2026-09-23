#pragma once

// Where a game's OS globals live.
//
// The console's OS is linked into each game, so its variables - the run queue
// the scheduler walks, the thread it idles on, the alarm queue - sit at
// addresses that game's link decided. The hardware's own low memory is fixed;
// none of this is.
//
// A game supplies them once at startup from its own native code:
//
//   RuntimeGuestOs::install(layout);
//
// A partial layout is allowed and reported, not refused.

#if !__has_include("guest_os_layout.h")
#error "<wiinx/cpu/guest_os.hpp> is part of the runtime: build with the cpu section on the include path."
#endif

#include "guest_os_layout.h"
