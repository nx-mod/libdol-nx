// Binds THPVideoDecode for the CPU runtime: the translator finds this
// registration by its name and signature, so translated calls go straight to
// the native. The address is the default binding (Mario Kart Wii's); a game's
// bindings put it at its own.
#include "hle_stubs.h"
#include "wiinx/accel/sdk.hpp"

#include <cstdint>

extern "C" int32_t THPVideoDecode_HLE(uint32_t file, uint32_t tileY, uint32_t tileU,
                                      uint32_t tileV, uint32_t work) {
    return wiinx::sdk::thp::decode_frame(file, tileY, tileU, tileV, work);
}

PPC_NATIVE_OVERRIDE(801B3BAC, THPVideoDecode_HLE, int32_t,
                    (uint32_t file, uint32_t tileY, uint32_t tileU, uint32_t tileV, uint32_t work),
                    (file, tileY, tileU, tileV, work));
