#pragma once

// The library's own version. It changes when the surface below `wiinx/` does.

#define WIINX_VERSION_MAJOR 2
#define WIINX_VERSION_MINOR 0
#define WIINX_VERSION_PATCH 0
#define WIINX_VERSION_STRING "2.0.0"

namespace wiinx {

// "2.0.0", for a log line or an about screen.
constexpr const char* version() { return WIINX_VERSION_STRING; }

}  // namespace wiinx
