#pragma once

// The runtime's log, and the tag every message carries.
//
//   RT_LOG(RT_TAG_GX) << "..." << std::endl;
//   RT_LOGF(RT_TAG_DVD, "read %u bytes\n", size);

#if !__has_include("runtime_log.h")
#error "<wiinx/cpu/log.hpp> is part of the runtime: build with the cpu section on the include path."
#endif

#include "runtime_log.h"
