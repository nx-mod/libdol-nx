#pragma once

// Hooks a game fills in, for the few places one game needs something of its own
// from the runtime.
//
// The library knows no game: it never names a game's function or address. A
// game that needs one of these installs it at startup from its own native code,
// and the runtime calls it if it is there. Nothing is required; a game that
// installs nothing behaves as the console does.
//
//   RuntimeGameHooks::install({ .surface_resized = &my_game_resized });
//
// See docs/game-hooks.md.

#if !__has_include("game_hooks.h")
#error "<wiinx/cpu/hooks.hpp> is part of the runtime: build with the cpu section on the include path."
#endif

#include "game_hooks.h"
