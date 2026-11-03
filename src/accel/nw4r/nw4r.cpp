// The nw4r module's table: every nw4r native, in one list.
#include "wiinx/accel/nw4r.hpp"

#include <vector>

namespace wiinx::nw4r {
namespace lyt {
extern const Native kPaneNatives[];
extern const std::size_t kPaneNativeCount;
extern const Native kAnimationNatives[];
extern const std::size_t kAnimationNativeCount;
}  // namespace lyt

std::span<const Native> natives() noexcept {
    // One list out of the module's tables, built once. g3d, snd and ef join
    // here as they are written.
    static const std::vector<Native> all = [] {
        std::vector<Native> table;
        table.insert(table.end(), lyt::kPaneNatives, lyt::kPaneNatives + lyt::kPaneNativeCount);
        table.insert(table.end(), lyt::kAnimationNatives, lyt::kAnimationNatives + lyt::kAnimationNativeCount);
        return table;
    }();
    return all;
}

}  // namespace wiinx::nw4r
