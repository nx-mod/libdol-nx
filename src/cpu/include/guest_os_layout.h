// Where a game's OS globals live.
//
// The Wii's OS is linked into each game, so its own variables - the run queue
// the scheduler walks, the thread it idles on, the alarm queue - sit at
// addresses that game's link decided. The console's lowmem globals
// (0x800000xx) are the hardware's and are fixed; everything here is not.
//
// libwii-nx names no game, so a game supplies these once at startup from its
// own native code, next to its game hooks (see docs/game-hooks.md). Nothing
// reads them before the guest boots.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace RuntimeGuestOs {

struct Layout {
    // OSThreadQueue RunQueue[32]: one head/tail pair per priority.
    uint32_t run_queue = 0;
    // The 32-bit mask of priorities whose run queue is not empty.
    uint32_t run_queue_bits = 0;
    // Non-zero while a thread switch is owed (OSReschedule's counter).
    uint32_t reschedule = 0;
    // OSDisableScheduler's nesting count; SelectThread returns while set.
    uint32_t scheduler_disable_count = 0;
    // The OSThread the game booted on, and the one the scheduler idles on.
    uint32_t default_thread = 0;
    uint32_t idle_thread = 0;
    // Pointer slot for OSSetSwitchThreadCallback's callback.
    uint32_t switch_thread_callback_ptr = 0;
    // Pointer slot for the interrupt handler table.
    uint32_t interrupt_handler_table_ptr = 0;
    // The alarm queue, as the distance below r13 the SDK reaches it at.
    uint32_t alarm_queue_r13_offset = 0;
    // OSLoadContext: it replaces the whole register file instead of returning,
    // so calls to it can never be guarded.
    uint32_t load_context = 0;
    // A thread entry that takes its real function from an object's vtable
    // (EGG::Thread::start and its like), which the object may not have filled
    // in yet when the thread starts. Zero when the game has none.
    uint32_t deferred_thread_entry = 0;
};

// Constant-initialized, so the addresses below are readable from any static
// initializer that runs before the game installs its own.
inline Layout g_layout{};

inline const Layout& layout() noexcept { return g_layout; }
inline void install(const Layout& layout) noexcept { g_layout = layout; }
inline bool installed() noexcept { return g_layout.run_queue != 0; }

// The fields a game left at zero, as a comma-separated list, or "" when it
// filled in every one. A game may legitimately install a partial layout -
// wiinx-scan finds the scheduler's own variables and not the rest - so this is
// for saying which, not for refusing to run.
inline std::string missing() noexcept {
    const std::pair<const char*, uint32_t> fields[]{
        {"run_queue", g_layout.run_queue},
        {"run_queue_bits", g_layout.run_queue_bits},
        {"reschedule", g_layout.reschedule},
        {"scheduler_disable_count", g_layout.scheduler_disable_count},
        {"default_thread", g_layout.default_thread},
        {"idle_thread", g_layout.idle_thread},
        {"switch_thread_callback_ptr", g_layout.switch_thread_callback_ptr},
        {"interrupt_handler_table_ptr", g_layout.interrupt_handler_table_ptr},
        {"alarm_queue_r13_offset", g_layout.alarm_queue_r13_offset},
        {"load_context", g_layout.load_context},
    };
    std::string names;
    for (const auto& [name, value] : fields) {
        if (value != 0) {
            continue;
        }
        if (!names.empty()) {
            names += ", ";
        }
        names += name;
    }
    return names;
}

}  // namespace RuntimeGuestOs
