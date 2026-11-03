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
namespace gx {
extern const Native kGxNatives[];
extern const std::size_t kGxNativeCount;
extern const Native kGxTransformNatives[];
extern const std::size_t kGxTransformNativeCount;
extern const Native kGxTevNatives[];
extern const std::size_t kGxTevNativeCount;
extern const Native kGxGeometryNatives[];
extern const std::size_t kGxGeometryNativeCount;
}  // namespace gx

std::span<const Native> natives() noexcept {
    // One list out of the module's tables, built once. GD joins here.
    static const std::vector<Native> all = [] {
        std::vector<Native> table;
        table.reserve(thp::kThpNativeCount + mtx::kMtxNativeCount + msl::kMslNativeCount + gx::kGxNativeCount +
                      gx::kGxTransformNativeCount);
        table.insert(table.end(), thp::kThpNatives, thp::kThpNatives + thp::kThpNativeCount);
        table.insert(table.end(), mtx::kMtxNatives, mtx::kMtxNatives + mtx::kMtxNativeCount);
        table.insert(table.end(), msl::kMslNatives, msl::kMslNatives + msl::kMslNativeCount);
        table.insert(table.end(), gx::kGxNatives, gx::kGxNatives + gx::kGxNativeCount);
        table.insert(table.end(), gx::kGxTransformNatives, gx::kGxTransformNatives + gx::kGxTransformNativeCount);
        table.insert(table.end(), gx::kGxTevNatives, gx::kGxTevNatives + gx::kGxTevNativeCount);
        table.insert(table.end(), gx::kGxGeometryNatives, gx::kGxGeometryNatives + gx::kGxGeometryNativeCount);
        return table;
    }();
    return all;
}

}  // namespace wiinx::sdk
