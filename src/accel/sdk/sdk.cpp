// The sdk module's table: every sdk native, in one list.
#include "wiinx/accel/sdk.hpp"

namespace wiinx::sdk {
namespace thp {
extern const Native kThpNatives[];
extern const std::size_t kThpNativeCount;
}  // namespace thp

std::span<const Native> natives() noexcept {
    // THP only so far; MSL, PSMTX and GD join as further tables.
    return {thp::kThpNatives, thp::kThpNativeCount};
}

}  // namespace wiinx::sdk
