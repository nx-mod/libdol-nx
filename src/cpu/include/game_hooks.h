// Hooks a game fills in, for the few places where one game needs something of
// its own from the runtime.
//
// libwii-nx knows no game: it never names a game's function or address. A game
// that needs one of these installs it at startup from its own native code
// (the sources in its project's native/ folder), and the runtime calls it if it
// is there. Nothing here is required; a game that installs nothing behaves as
// the Wii does.
#pragma once

#include <cstdint>

struct CpuContext;

namespace RuntimeGameHooks {

struct Hooks {
    // The window or surface changed size. Games that scale their own canvas to
    // the display (Mario Kart Wii's dynamic aspect) recompute it here.
    void (*surface_resized)(uint32_t width, uint32_t height) = nullptr;

    // The guest is about to set a viewport. Games whose screens render to
    // fixed-size offscreen targets mark them here.
    void (*viewport_about_to_change)() = nullptr;

    // A call is about to be made to `target`, with the guest's arguments in
    // `cpu`. A game may adjust them - the runtime's graphics settings reach
    // one game's renderer this way. Called for every indirect call, so it must
    // be cheap: compare the address and return.
    void (*calling)(uint32_t target, CpuContext* cpu) = nullptr;
};

inline Hooks& mutable_hooks() noexcept {
    static Hooks hooks;
    return hooks;
}

inline const Hooks& hooks() noexcept { return mutable_hooks(); }

// Called once at startup, from the game's own code.
inline void install(const Hooks& hooks) noexcept { mutable_hooks() = hooks; }

inline void surface_resized(uint32_t width, uint32_t height) noexcept {
    if (const auto hook = hooks().surface_resized) {
        hook(width, height);
    }
}

inline void viewport_about_to_change() noexcept {
    if (const auto hook = hooks().viewport_about_to_change) {
        hook();
    }
}

inline void calling(uint32_t target, CpuContext* cpu) noexcept {
    if (const auto hook = hooks().calling) {
        hook(target, cpu);
    }
}

}  // namespace RuntimeGameHooks
