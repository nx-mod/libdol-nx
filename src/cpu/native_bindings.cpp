// The default binding table: empty, meaning every registration keeps the address
// written at its call site. That is right for the game the runtime was written
// against (Mario Kart Wii), which needs no table at all.
//
// Another game links a generated table instead of this one - see
// example-wii-nx/scripts/make-bindings - and these definitions are weak so that
// file simply replaces them.
#include "native_bindings.h"

namespace NativeBindings {

__attribute__((weak)) const Binding* Table() noexcept { return nullptr; }

__attribute__((weak)) std::size_t TableSize() noexcept { return 0; }

}  // namespace NativeBindings
