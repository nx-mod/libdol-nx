// A title's own diagnostics, switched on. Nintendo's libraries print what went
// wrong only above a verbosity level kept in a global - the Wii Menu's CDB and
// VF libraries ("VFMountDriveNANDFlashEx VFErr=%d", "NANDCreateDir() error =
// %d") say nothing at their default - and those messages are the quickest way
// to learn why a title gave up. A title's globals.json can name such levels as
// debug.verbosity and debug.verbosity1..7; each is set to the highest level on
// the first vblank, after the title's own start-up has set its defaults and
// before anything later runs. A title that names none is untouched.
#include "guest_globals.h"
#include "hle_stubs.h"
#include "memory.h"

#include <cstdio>
#include <string>

namespace {

void RaiseVerbosity(CpuContext*) {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    for (int i = 0; i < 8; ++i) {
        const std::string name = i == 0 ? "debug.verbosity" : "debug.verbosity" + std::to_string(i);
        if (const uint32_t address = RuntimeGuestGlobals::find(name)) {
            try {
                Memory::Write32(address, 0x7FFFFFFFu);
            } catch (const Memory::AccessViolation&) {
            }
        }
    }
}

const bool g_registered = [] {
    VI_HLE_AddRetraceHook(RaiseVerbosity);
    return true;
}();

}  // namespace
