#pragma once

// platform - the console itself.
//
// A game does not call this: it calls its own SDK, which the runtime answers at
// the same boundaries. What a *program* configures is small, and it is what
// this section's headers expose.
//
//   product.hpp   what this build calls itself, and what it puts in low memory
//   console.hpp   the console's identity: serial, region, MAC
//   paths.hpp     where the NAND, the saves and the logs live
//   input.hpp     which physical control answers which guest button
//
// Everything else - threads, the disc, graphics, sound - is answered rather
// than exposed. The modules that do the answering each have a README:
// os, fs, gx, audio, input, net, system.

#include "wiinx/platform/console.hpp"
#include "wiinx/platform/paths.hpp"
#include "wiinx/platform/product.hpp"
