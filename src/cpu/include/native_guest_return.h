// Where the guest resumes when a native calls back into it.
//
// A native that replaces a guest function and then calls guest code - a pane
// walking its children through their vtables - has to leave the link register
// pointing where the original would have: some callees are dispatched as a
// jump and resume through it, and the original function's own code continues
// there. The address is the game's, so the binding supplies it and the native
// itself stays game-agnostic.
#pragma once

#include <cstdint>

namespace RuntimeNativeGuestReturn {

inline thread_local uint32_t t_returnAddress = 0;

// Set for the duration of one native call, by the binding that knows the
// address. Nested natives restore the outer one.
class Scope {
public:
    explicit Scope(uint32_t address) noexcept : previous_(t_returnAddress) {
        t_returnAddress = address;
    }
    ~Scope() noexcept { t_returnAddress = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    uint32_t previous_;
};

inline uint32_t Current() noexcept { return t_returnAddress; }

}  // namespace RuntimeNativeGuestReturn
