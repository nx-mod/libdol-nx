#pragma once

// cpu - the guest CPU the translated game runs on.
//
// Everything here is part of the running console, so it needs the runtime's own
// headers on the include path. A program that only writes natives or reads
// formats does not include this section at all.
//
//   hooks.hpp         the few places a game needs something of its own
//   guest_os.hpp      where that game's copy of the OS keeps its globals
//   guest_return.hpp  where the guest resumes when a native calls back into it
//   bindings.hpp      which address each replaced function has in this game
//   memory.hpp        the guest's address space
//   runtime.hpp       the CPU context and how a guest function is entered
//   log.hpp           the runtime's log tags

#include "wiinx/cpu/bindings.hpp"
#include "wiinx/cpu/guest_os.hpp"
#include "wiinx/cpu/guest_return.hpp"
#include "wiinx/cpu/hooks.hpp"
