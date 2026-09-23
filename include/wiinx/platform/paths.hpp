#pragma once

// Where things live on the machine running the game: the NAND shared by every
// game, this game's saves, its settings and its logs.

#if !__has_include("nand_path.h")
#error "<wiinx/platform/paths.hpp> is part of the runtime: build with the platform section on the include path."
#endif

#include "nand_path.h"
