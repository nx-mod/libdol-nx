#pragma once

// libdol-nx: the machine the GameCube and the Wii both are.
//
// The SDK is six sections, each one a folder of headers under `wiinx/` and a
// static library of the same name. They stack, and dependencies only ever point
// downwards:
//
//   app       the program: window, main loop, settings, overlay
//   accel     native replacements for the libraries a game links
//   platform  the console: os, fs, gx, audio, input, system, net
//   cpu       the guest CPU: registers, memory, calls, threads
//   format    the Wii's own file formats - discs, NAND, WADs. Depends on
//             nothing, so tools and a launcher use it on a PC as well
//   core      types, typed guest memory, the host interface, the native registry
//
// This header pulls in the sections that build anywhere: core, accel and
// format. `cpu`, `platform` and `app` are the running console and need its
// headers on the include path, so they are included by section:
//
//   #include <wiinx/wiinx.hpp>            // core + accel + format
//   #include <wiinx/platform/platform.hpp>  // in a runtime build
//
// Nothing here names a game. What a game needs of its own it installs at
// startup through <wiinx/cpu/hooks.hpp> and <wiinx/cpu/guest_os.hpp>.

#include "wiinx/version.hpp"

#include "wiinx/core/core.hpp"
#include "wiinx/accel/accel.hpp"
#include "wiinx/format/format.hpp"
