// THPVideoDecode's argument and header checks, against fake guest memory.
// Decoding real frames needs a game's movie, which never belongs here; the
// runtime's copy of this decoder was checked frame-for-frame against the SDK's.
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/host.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace wiinx;

namespace {
std::vector<u8> g_memory(0x200000, 0);
u32 g_gpr[32];
u32 gpr(Cpu*, int i) { return g_gpr[i]; }
void set_gpr(Cpu*, int i, u32 v) { g_gpr[i] = v; }
bool valid(GuestAddr a, u32 size) { return a + static_cast<u64>(size) <= g_memory.size(); }

s32 decode(u32 file, u32 y, u32 u, u32 v, u32 work) {
    const Native* n = find_native("THPVideoDecode", "rvl.thp@2007-08-08");
    assert(n != nullptr);
    g_gpr[3] = file; g_gpr[4] = y; g_gpr[5] = u; g_gpr[6] = v; g_gpr[7] = work;
    n->entry(nullptr);
    return static_cast<s32>(g_gpr[3]);
}
}  // namespace

int main() {
    Host h;
    h.memory = g_memory.data();
    h.valid = valid;
    h.gpr = gpr;
    h.set_gpr = set_gpr;
    set_host(h);
    add_natives(sdk::natives());

    assert(decode(0, 0x1000, 0x2000, 0x3000, 0x4000) == 25);       // no input
    assert(decode(0x100, 0, 0x2000, 0x3000, 0x4000) == 27);        // no output
    assert(decode(0x100, 0x1000, 0x2000, 0x3000, 0) == 26);        // no work area
    assert(decode(0x100, 0x1000, 0x2000, 0x3000, 0x4000) != 0);    // zeros are not a frame
    std::puts("rvl.thp@2007-08-08 checks ok");
}
