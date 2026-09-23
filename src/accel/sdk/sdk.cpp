// The sdk module's table: every sdk native, in one list.
#include "wiinx/accel/sdk.hpp"

#include <vector>

namespace wiinx::sdk {
namespace thp {
extern const Native kThpNatives[];
extern const std::size_t kThpNativeCount;
}  // namespace thp
namespace mtx {
extern const Native kMtxNatives[];
extern const std::size_t kMtxNativeCount;
}  // namespace mtx
namespace msl {
extern const Native kMslNatives[];
extern const std::size_t kMslNativeCount;
}  // namespace msl

std::span<const Native> natives() noexcept {
    // One list out of the module's tables, built once. GD joins here.
    static const std::vector<Native> all = [] {
        std::vector<Native> table;
        table.reserve(thp::kThpNativeCount + mtx::kMtxNativeCount + msl::kMslNativeCount);
        table.insert(table.end(), thp::kThpNatives, thp::kThpNatives + thp::kThpNativeCount);
        table.insert(table.end(), mtx::kMtxNatives, mtx::kMtxNatives + mtx::kMtxNativeCount);
        table.insert(table.end(), msl::kMslNatives, msl::kMslNatives + msl::kMslNativeCount);
        return table;
    }();
    return all;
}

}  // namespace wiinx::sdk
