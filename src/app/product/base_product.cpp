#include "runtime_product.h"

namespace RuntimeProduct {

// The game as the disc has it: no overlay, nothing seeded, nothing renamed.
const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        "WiiCompiled",
    };
    return descriptor;
}

} // namespace RuntimeProduct
