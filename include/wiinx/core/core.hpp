#pragma once

// core: what every other section is written against.
//
//   types.hpp   the Wii's integer types, guest addresses, big-endian access
//   host.hpp    what a native asks the runtime for - memory, registers, calls
//   guest.hpp   guest structures as typed fields at fixed offsets
//   native.hpp  the native registry: a name, a library build, a function
//   builds.hpp  every library build known, generated from data/builds.json
//
// core depends on the standard library and nothing else, so a native module
// built on it can be compiled and tested on any machine.

#include "wiinx/core/builds.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"
#include "wiinx/core/native.hpp"
#include "wiinx/core/types.hpp"
