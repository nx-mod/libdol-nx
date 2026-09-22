// Hands the CPU runtime's guest memory and CPU to libwii-nx's natives, and
// adds the native modules this program links. Called once at startup, after
// guest memory and the renderer exist and before the game runs.
#include "wiinx/accel/nw4r.hpp"
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/host.hpp"
#include "wiinx/core/native.hpp"

#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "memory.h"

#include <cstdio>

extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);
#if defined(__SWITCH__)
void SwitchBootLogExternal(const char* text) noexcept;
#endif

namespace {
CpuContext* context(wiinx::Cpu* cpu) { return reinterpret_cast<CpuContext*>(cpu); }
}  // namespace

void WiinxInstallHost() {
    wiinx::Host host;
    host.memory = MKW_FLAT_GUEST_BASE;
    host.valid = [](wiinx::GuestAddr addr, wiinx::u32 size) { return Memory::Contains(addr, size); };
    host.gpr = [](wiinx::Cpu* cpu, int index) { return static_cast<wiinx::u32>(context(cpu)->gpr[index]); };
    host.set_gpr = [](wiinx::Cpu* cpu, int index, wiinx::u32 value) { context(cpu)->gpr[index] = value; };
    // The link register is left as the native's own caller set it: the callee
    // returns to the native, which is all a translated function needs.
    host.call = [](wiinx::Cpu* cpu, wiinx::GuestAddr target) { InvokeIndirectCpu(target, context(cpu)); };
    host.notify_write = [](wiinx::GuestAddr addr, wiinx::u32 size) { GxNotifyGuestRamDmaWrite(addr, size); };
#if defined(__SWITCH__)
    host.log = [](const char* line) { SwitchBootLogExternal(line); };
#else
    host.log = [](const char* line) { std::puts(line); };
#endif
    wiinx::set_host(host);
    wiinx::add_natives(wiinx::sdk::natives());
    wiinx::add_natives(wiinx::nw4r::natives());
}
