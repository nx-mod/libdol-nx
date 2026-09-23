// The egg module's table: every egg native, in one list.
#include "wiinx/accel/egg.hpp"

#include "wiinx/core/host.hpp"

namespace wiinx::egg {
namespace {

// EGG::Decomp::decodeSZS(const void* src, void* dst) -> u32 expanded size.
void decode_szs_native(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr src = h.gpr(cpu, 3);
    const GuestAddr dst = h.gpr(cpu, 4);
    h.set_gpr(cpu, 3, decomp::decode_szs(src, dst));
}

const Native kEggNatives[] = {
    WIINX_NATIVE("EGG::Decomp::decodeSZS", kEgg_Any, decode_szs_native),
};

}  // namespace

std::span<const Native> natives() noexcept { return kEggNatives; }

}  // namespace wiinx::egg
