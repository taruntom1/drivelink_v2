#pragma once

#include <cstdint>

namespace core {

/// Consecutive crash resets that put the device in safe mode.
inline constexpr uint32_t kSafeModeCrashes = 3;

/// What the boot decides from the crash counter.
struct SafeModeDecision {
    uint32_t counter; ///< the counter to store for this boot
    bool safe_mode;   ///< boot with every configured instance offline
};

/// The safe-mode rule: a crash reset (panic, interrupt or task watchdog)
/// adds one to the stored counter, any other reset clears it, and the device
/// boots in safe mode once it reaches `kSafeModeCrashes`. Pure.
constexpr SafeModeDecision decideSafeMode(uint32_t stored, bool crash_reset)
{
    if (!crash_reset) {
        return SafeModeDecision{0U, false};
    }
    const uint32_t counter = (stored == UINT32_MAX) ? stored : stored + 1U;
    return SafeModeDecision{counter, counter >= kSafeModeCrashes};
}

} // namespace core
