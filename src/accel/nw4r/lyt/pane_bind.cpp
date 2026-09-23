// Binds nw4r::lyt::Pane::CalculateMtx for the CPU runtime (see thp_bind.cpp).
// The runtime's CpuContext is what natives see as wiinx::Cpu.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "native_guest_return.h"
#include "wiinx/accel/nw4r.hpp"

// Where the original resumes after calling a child's CalculateMtx: the guest
// address of the instruction after that call, in the function this native
// replaces. A child dispatched as a jump returns through it.
constexpr uint32_t kChildCallReturn = 0x800791A4u;

// Where this game binds the native, so a child pane whose vtable points back
// here is computed directly instead of through the guest dispatcher.
constexpr uint32_t kBoundAddress = 0x80078EF0u;

extern "C" void LytPaneCalculateMtx_HLE(CpuContext* ctx) {
    // Bound for the build Mario Kart Wii links until bindings carry the build.
    static const wiinx::Native* const native = [] {
        wiinx::nw4r::lyt::set_pane_calculate_mtx_address(kBoundAddress);
        return wiinx::find_native("nw4r::lyt::Pane::CalculateMtx", wiinx::nw4r::kLyt_2008_03.id);
    }();
    const RuntimeNativeGuestReturn::Scope resume{kChildCallReturn};
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

PPC_NATIVE_OVERRIDE_VOID(80078EF0, LytPaneCalculateMtx_HLE, (CpuContext* ctx), (ctx));
