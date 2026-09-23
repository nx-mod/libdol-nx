// Hands the CPU runtime's guest memory and CPU to libwii-nx's natives, and
// adds the native modules this program links. Called once at startup, after
// guest memory and the renderer exist and before the game runs.
#include "wiinx/accel/nw4r.hpp"
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/host.hpp"
#include "wiinx/core/native.hpp"

#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "native_guest_return.h"
#include "memory.h"
#include "memory_access.h"

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
    // Through the runtime's own resolution, not by adding to a base: parts of
    // the guest's address space are not in the flat mapping, and a native that
    // computed its own pointer faulted on the first pane that lived in one.
    host.pointer = [](wiinx::GuestAddr addr, wiinx::u32 size) -> wiinx::u8* {
        // One lookup, inline: the page table answers "where is it" and "is it
        // there" together. Asking Contains and then GetPointer resolved the
        // same address twice, on a path a native takes for every field of
        // every object it touches.
        return MemoryInline::GetPointerFast(addr, size);
    };
    host.valid = [](wiinx::GuestAddr addr, wiinx::u32 size) { return Memory::Contains(addr, size); };
    host.gpr = [](wiinx::Cpu* cpu, int index) { return static_cast<wiinx::u32>(context(cpu)->gpr[index]); };
    host.set_gpr = [](wiinx::Cpu* cpu, int index, wiinx::u32 value) { context(cpu)->gpr[index] = value; };
    // A float argument arrives as a double, which is what the union's `d` is.
    host.fpr = [](wiinx::Cpu* cpu, int index) { return context(cpu)->fpr[index].d; };
    host.set_fpr = [](wiinx::Cpu* cpu, int index, double value) { context(cpu)->fpr[index].d = value; };
    // A callee dispatched as a jump resumes through the link register, and the
    // original function's own code continues there, so the binding's return
    // address is put in place for the call (native_guest_return.h). Without it
    // the callee resumed wherever the last caller happened to leave lr.
    host.call = [](wiinx::Cpu* cpu, wiinx::GuestAddr target) {
        CpuContext* ctx = context(cpu);
        if (const uint32_t resume = RuntimeNativeGuestReturn::Current(); resume != 0) {
            ctx->lr = resume;
        }
        InvokeIndirectCpu(target, ctx);
    };
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
