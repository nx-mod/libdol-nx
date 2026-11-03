// Natives: a library function's name, the library version it matches, and
// the code that replaces it.
//
// Each module lists its natives in one plain table and hands it out through
// a function; the program adds the tables of the modules it links. There are
// no self-registering statics, so nothing is silently dropped when a module is
// linked as a static library, and core never depends on a module.
//
//     wiinx::add_natives(wiinx::nw4r::natives());
#pragma once

#include "wiinx/core/types.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace wiinx {

struct Cpu;

// One build of one versioned thing: a library the game links, the SDK or IOS
// it was built for, the game's own revision or region. For libraries it is
// the banner Nintendo's code registers at startup, e.g.
//     << NW4R    - LYT    final   build: Mar  8 2008 20:59:41 (0x4201_127) >>
struct LibVersion {
    std::string_view id;     // stable id used in bindings: "nw4r.lyt@2008-03-08"
    std::string_view lib;    // what is versioned, e.g. "nw4r.lyt", "rvl.os", "game.RMCP01"
    std::string_view build;  // how the game names this build: "Mar  8 2008 (0x4201_127)"
};

// The builds detected in the running game, set by the host from the game's
// bindings at startup. Modules ask for one when behavior (not just a native)
// depends on it:
//     if (wiinx::detected("rvl.os") == &kOs_2008_06) { ... }
void set_detected(std::span<const LibVersion* const> builds) noexcept;
const LibVersion* detected(std::string_view lib) noexcept;

using NativeFn = void (*)(Cpu*);

struct Native {
    std::string_view name;       // the original's qualified name
    const LibVersion* version;   // which build of the library it matches
    NativeFn entry;
};

#define WIINX_NATIVE(name, version, fn) ::wiinx::Native{name, &(version), fn}

// Adds a module's table. Call once per module at startup, before binding.
void add_natives(std::span<const Native> table) noexcept;

// Calls `fn` for every native added so far.
template <class Fn>
void for_each_native(Fn&& fn);

// The native for `name` in `version_id`, or null when none is known.
const Native* find_native(std::string_view name, std::string_view version_id) noexcept;

// Where this game binds a native, set by its generated bindings at startup:
// the guest address, and the return address after each indirect call (bctrl)
// the original makes, in code order. A library of natives calls itself - a
// pane animates its children through their vtables - and needs both: an entry
// that is one of its own natives is run directly, not sent out through the
// guest dispatcher and back, and a call into game code leaves lr where the
// original's would have been (some callees resume through it).
struct Bound {
    GuestAddr address = 0;
    std::span<const GuestAddr> calls;  // after the 1st, 2nd, ... bctrl
    // the return address of the original's nth indirect call, or 0
    GuestAddr call(std::size_t n) const noexcept { return n < calls.size() ? calls[n] : 0; }
};
void set_bound(std::string_view name, GuestAddr address, std::span<const GuestAddr> calls) noexcept;
// This game's binding of `name`, or null when it binds none. The name is the
// native's (Native::name): one game binds one build of each.
const Bound* bound(std::string_view name) noexcept;

namespace detail {
std::span<const std::span<const Native>> native_tables() noexcept;
}  // namespace detail

template <class Fn>
void for_each_native(Fn&& fn) {
    for (std::span<const Native> table : detail::native_tables()) {
        for (const Native& native : table) {
            fn(native);
        }
    }
}

}  // namespace wiinx
