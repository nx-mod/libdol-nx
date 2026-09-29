#include "guest_globals.h"

namespace RuntimeGuestGlobals {
namespace {
const Entry* g_entries = nullptr;
size_t g_count = 0;
}  // namespace

void install(const Entry* entries, size_t count) noexcept {
    g_entries = entries;
    g_count = count;
}

// Linear: a few dozen names, and callers cache the answer.
uint32_t find(std::string_view name) noexcept {
    for (size_t i = 0; i < g_count; ++i) {
        if (name == g_entries[i].name) {
            return g_entries[i].address;
        }
    }
    return 0;
}

}  // namespace RuntimeGuestGlobals
