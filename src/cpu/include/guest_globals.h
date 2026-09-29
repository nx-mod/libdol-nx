// Where a game keeps the SDK's own variables, by name.
//
// Functions move between games through bindings.json; the SDK's data (VI's
// retrace count, GX's FIFO objects) moves the same way through globals.json,
// which tools/wiinx-make-globals turns into a table the game installs at
// startup. Names are the SDK's, prefixed by library: "vi.retraceCount".
//
// find() returns 0 for a name the game did not give, and every caller treats 0
// as "not known here": it skips the write, or falls back, rather than touching
// an address that belongs to some other game's layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace RuntimeGuestGlobals {

struct Entry {
    const char* name;
    uint32_t address;
};

void install(const Entry* entries, size_t count) noexcept;
uint32_t find(std::string_view name) noexcept;

}  // namespace RuntimeGuestGlobals
