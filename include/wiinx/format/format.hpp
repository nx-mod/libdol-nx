#pragma once

// format: the Wii's own file formats, read and written as data.
//
//   nand/   the console's internal memory: settings, Miis, titles, WADs
//   disc/   disc images, GameCube and Wii, in the containers dumps come in
//
// This section depends on the standard library alone - no host, no runtime, no
// guest memory. The runtime uses it to serve a game, the tools use it on a PC,
// and a launcher uses it to show what is installed, all from one copy.

#include "wiinx/format/nand/nand.hpp"
