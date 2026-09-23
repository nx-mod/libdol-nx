#pragma once

// format: the consoles' own file formats, read and written as data.
//
//   disc/     disc images, GameCube and Wii, in the containers dumps come in
//   nand/     the Wii's internal memory: settings, Miis, titles, WADs
//   archive/  what games pack their files in: U8, Yaz0
//   media/    THP video
//
// This section depends on the standard library alone - no host, no runtime, no
// guest memory. The runtime uses it to serve a game, the tools use it on a PC,
// and a launcher uses it to show what is installed, all from one copy.
//
// Two things are deliberately the caller's: keys and compressors. A Wii disc's
// cipher and a dump's decompressor are passed in, so nothing here carries a key
// or depends on a compression library.

#include "wiinx/format/archive/cx.hpp"
#include "wiinx/format/archive/u8.hpp"
#include "wiinx/format/archive/yaz0.hpp"
#include "wiinx/format/disc/container.hpp"
#include "wiinx/format/disc/image.hpp"
#include "wiinx/format/disc/rvz.hpp"
#include "wiinx/format/media/banner.hpp"
#include "wiinx/format/media/thp.hpp"
#include "wiinx/format/nand/nand.hpp"
