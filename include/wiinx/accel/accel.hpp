#pragma once

// accel: native code in place of the libraries a game links.
//
// A game runs without any of it, only slower: when a native is not bound, the
// game's own translated code runs instead. Each module hands out its table of
// natives, and the program adds the ones it links:
//
//   wiinx::add_natives(wiinx::sdk::natives());
//   wiinx::add_natives(wiinx::nw4r::natives());
//
//   sdk.hpp   the RVL SDK: the C library, matrix math, THP video
//   nw4r.hpp  Nintendo's Wii middleware: layouts, models, sound, effects
//   egg.hpp   Nintendo EAD's framework, as the games built on it link it
//
// A native must give the original's results exactly wherever a game keeps them:
// a ghost replays them, a race sends them, a checksum covers them.

#include "wiinx/accel/egg.hpp"
#include "wiinx/accel/nw4r.hpp"
#include "wiinx/accel/sdk.hpp"
