#pragma once

// nand: the console's internal memory, as files.
//
//   isfs.hpp     title ids, the paths IOS keeps things at, setting.txt
//   sysconf.hpp  the settings every game reads: language, aspect, sound
//   mii.hpp      RFL_DB.dat, the Mii database
//   title.hpp    tickets and TMDs: what a title is and what it carries
//   wad.hpp      a title packed as one file, installable or boot2
//
// Reading and writing bytes is the caller's: these take and return buffers, so
// the same code serves a NAND on an SD card, a NAND in a folder on a PC, and a
// WAD being installed into either.

#include "wiinx/format/nand/isfs.hpp"
#include "wiinx/format/nand/mii.hpp"
#include "wiinx/format/nand/sysconf.hpp"
#include "wiinx/format/nand/title.hpp"
#include "wiinx/format/nand/wad.hpp"
