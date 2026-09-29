// Binds nw4r::lyt::Pane::CalculateMtx for the CPU runtime (see thp_bind.cpp).
// The runtime's CpuContext is what natives see as wiinx::Cpu.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "native_guest_return.h"
#include "wiinx/accel/nw4r.hpp"

// The reference address this native is registered at (native_bindings.h).
constexpr uint32_t kReference = 0x80078EF0u;

// Where the original resumes after calling a child's CalculateMtx, as an offset
// into the function. The native only binds where the build's signature matched,
// so the code - and this offset - are the same in every game that binds it.
constexpr uint32_t kChildCallReturnOffset = 0x2B4u;

extern "C" void LytPaneCalculateMtx_HLE(CpuContext* ctx) {
    // Where this game has the function, so a child pane whose vtable points
    // back here is computed directly instead of through the guest dispatcher.
    static const uint32_t bound = ::NativeBindings::Resolve(kReference);
    static const wiinx::Native* const native = [] {
        wiinx::nw4r::lyt::set_pane_calculate_mtx_address(bound);
        return wiinx::find_native("nw4r::lyt::Pane::CalculateMtx", wiinx::nw4r::kLyt_2008_03.id);
    }();
    const RuntimeNativeGuestReturn::Scope resume{bound + kChildCallReturnOffset};
    native->entry(reinterpret_cast<wiinx::Cpu*>(ctx));
}

PPC_NATIVE_OVERRIDE_VOID(80078EF0, LytPaneCalculateMtx_HLE, (CpuContext* ctx), (ctx));
