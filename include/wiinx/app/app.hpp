#pragma once

// app - the program around the game.
//
// The main loop, the settings a player changes, and the overlay they change
// them in. A game project does not normally touch this: it links a product
// descriptor (<wiinx/platform/product.hpp>) and the app does the rest.

#if !__has_include("settings_overlay.h")
#error "<wiinx/app/app.hpp> is part of the runtime: build with the app section on the include path."
#endif

#include "settings_overlay.h"
