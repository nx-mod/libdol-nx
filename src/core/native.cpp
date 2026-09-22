#include "wiinx/core/native.hpp"

#include <array>

namespace wiinx {

// Each module's table. A module appears here once it exists; its table is
// a plain array, so linking the module is all it takes to include it.
namespace modules {
}  // namespace modules

std::span<const Native> natives() noexcept {
    // No modules yet: the table is empty until the first one lands.
    static const std::array<Native, 0> kAll{};
    return kAll;
}

const Native* find_native(std::string_view name, std::string_view version_id) noexcept {
    for (const Native& native : natives()) {
        if (native.name == name && native.version != nullptr && native.version->id == version_id) {
            return &native;
        }
    }
    return nullptr;
}

}  // namespace wiinx
