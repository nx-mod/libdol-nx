#include "wiinx/core/native.hpp"

#include <array>

namespace wiinx {
namespace {
// One entry per linked module; there are only a handful of modules.
std::array<std::span<const Native>, 32> g_tables{};
std::size_t g_tableCount = 0;
}  // namespace

void add_natives(std::span<const Native> table) noexcept {
    for (std::size_t i = 0; i < g_tableCount; ++i) {
        if (g_tables[i].data() == table.data()) {
            return;  // already added
        }
    }
    if (g_tableCount < g_tables.size()) {
        g_tables[g_tableCount++] = table;
    }
}

namespace {
std::array<const LibVersion*, 64> g_detected{};
std::size_t g_detectedCount = 0;
}  // namespace

void set_detected(std::span<const LibVersion* const> builds) noexcept {
    g_detectedCount = 0;
    for (const LibVersion* build : builds) {
        if (build != nullptr && g_detectedCount < g_detected.size()) {
            g_detected[g_detectedCount++] = build;
        }
    }
}

const LibVersion* detected(std::string_view lib) noexcept {
    for (std::size_t i = 0; i < g_detectedCount; ++i) {
        if (g_detected[i]->lib == lib) {
            return g_detected[i];
        }
    }
    return nullptr;
}

namespace detail {
std::span<const std::span<const Native>> native_tables() noexcept {
    return {g_tables.data(), g_tableCount};
}
}  // namespace detail

const Native* find_native(std::string_view name, std::string_view version_id) noexcept {
    for (std::span<const Native> table : detail::native_tables()) {
        for (const Native& native : table) {
            if (native.name == name && native.version != nullptr && native.version->id == version_id) {
                return &native;
            }
        }
    }
    return nullptr;
}

}  // namespace wiinx
