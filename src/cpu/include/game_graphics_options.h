#pragma once

#include <atomic>
#include <cstdint>

// Which post-processing a player turned off. The runtime holds the setting;
// a game applies it to its own renderer through game_hooks.h, since only the
// game knows which call carries it and what its bits mean.
namespace RuntimeGameGraphicsOptions {

inline std::atomic<uint32_t>& DisabledPostProcessingPathsState() noexcept {
    static std::atomic<uint32_t> disabledMask{0};
    return disabledMask;
}

inline uint32_t DisabledPostProcessingPaths() noexcept {
    return DisabledPostProcessingPathsState().load(std::memory_order_relaxed);
}

inline void SetDisabledPostProcessingPaths(uint32_t disabledMask) noexcept {
    DisabledPostProcessingPathsState().store(disabledMask, std::memory_order_relaxed);
}

} // namespace RuntimeGameGraphicsOptions
