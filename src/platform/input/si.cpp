// Serial Interface: the four controller ports, and the bus the GameCube pads
// and the console's own hardware sit on.
//
// These live here rather than with the OS natives they were first written
// beside, because what they replace is input hardware: pad.cpp is their peer.

#include "hle_stubs.h"
#include "abi_bridge.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

// Serial Interface (SI) - GameCube controller ports; stubbed since we don't emulate the MMIO.

// SIInit (0x801b2de0): skips MMIO setup at 0xCD006434 and controller detection.
extern "C" void SIInit_801b2de0()
{
    RT_LOG(RT_TAG_OS) << "SIInit_801b2de0 called: skipping MMIO register setup and controller detection" << std::endl;
}

// SISetSamplingRate (0x801b3acc): ignored, we don't emulate SI polling timing.
extern "C" void HLE_SISetSamplingRate_801b3acc(uint32_t msec)
{
    RT_LOG(RT_TAG_OS) << "HLE_SISetSamplingRate_801b3acc called: msec=" << msec << ": Stubbed success." << std::endl;
}

// __OSGetDIConfig (0x800ef4b8, as Mega Man 9 has it): four instructions that
// read the drive interface's configuration register at 0xCD006024 and return
// its low byte. The register is read-only on hardware, and the bit a game looks
// at says the bootrom descrambler is disabled - what a retail console reports,
// and what Dolphin's DVDInterface sets (m_DICFG.CONFIG = 1).
//
// The function is identical byte for byte in every game seen so far, which is
// why one signature binds it everywhere.
extern "C" uint32_t OS____GetDIConfig_800ef4b8()
{
    return 1;
}

PPC_NATIVE_OVERRIDE(800EF4B8, OS____GetDIConfig_800ef4b8, uint32_t, (), ());

// Video Interface (VI) - TV output.


REGISTER_NATIVE_FUNCTION(0x801B2DE0, SIInit_801b2de0);
PPC_NATIVE_OVERRIDE_VOID(801B3ACC, HLE_SISetSamplingRate_801b3acc, (uint32_t msec), (msec));
