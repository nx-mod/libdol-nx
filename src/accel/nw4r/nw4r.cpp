// The nw4r module's table: every nw4r native, in one list.
#include "wiinx/accel/nw4r.hpp"

#include <array>

namespace wiinx::nw4r {
namespace lyt {
extern const Native kPaneNatives[];
extern const std::size_t kPaneNativeCount;
}  // namespace lyt

std::span<const Native> natives() noexcept {
    // One sub-library's natives today (lyt); g3d, snd and ef join here as
    // they are written, each as its own table appended below.
    return {lyt::kPaneNatives, lyt::kPaneNativeCount};
}

}  // namespace wiinx::nw4r
