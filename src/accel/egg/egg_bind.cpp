// The runtime's binding for EGG::Decomp::decodeSZS.
//
// The decoder itself is in the egg module (src/accel/egg/decomp.cpp), written
// against the host interface so it builds and is testable anywhere. This is
// what registers it for the game being built: a game whose address for the
// function is known binds it here, and a malformed archive is fatal the way the
// runtime's other unrecoverable file errors are.
#include "wiinx/accel/egg.hpp"

#include "hle_stubs.h"

#include <cstdint>
#include <cstdlib>

#include "runtime_log.h"

extern "C" uint32_t EGG_Decomp_decodeSZS_80218c2c(uint32_t src, uint32_t dst)
{
    const uint32_t expanded = wiinx::egg::decomp::decode_szs(src, dst);
    if (expanded == 0) {
        RT_LOG(RT_TAG_HLE) << "decodeSZS: malformed stream from 0x" << std::hex << src << std::dec
                           << std::endl;
        ShowRuntimeFatalPopup("corrupt compressed file",
                              "The game stopped decoding a malformed Yaz0 file.");
        std::abort();
    }
    return expanded;
}

PPC_NATIVE_OVERRIDE(80218C2C, EGG_Decomp_decodeSZS_80218c2c, uint32_t,
                    (uint32_t src, uint32_t dst), (src, dst));
