#pragma once

#include <cstdint>

namespace mod_motor {

/// Output-shaft angle in radians for an encoder count.
/// counts_per_rev and gear_ratio must be > 0.
float countsToRadians(int32_t counts, float counts_per_rev, float gear_ratio);

/// Output-shaft angular velocity in rad/s for an encoder rate in counts/s.
/// counts_per_rev and gear_ratio must be > 0.
float countsPerSecToRadPerSec(float counts_per_sec, float counts_per_rev, float gear_ratio);

/// Smoothing factor of a first-order low-pass filter with cutoff `cutoff_hz`,
/// sampled `dt_s` seconds after the previous sample. A cutoff <= 0 returns 1,
/// which means "no filtering".
float lowPassAlpha(float cutoff_hz, float dt_s);

/// Velocity estimate from successive encoder counts: the count delta over
/// dt, converted to rad/s at the output shaft, then low-pass filtered.
///
/// Pure math with no hardware or RTOS dependency. Not thread-safe: one task
/// (the control loop) owns it.
class VelocityEstimator {
public:
    /// Feed one encoder count, sampled dt_s seconds after the previous one.
    ///
    /// - The first sample after construction or reset() only records the
    ///   count, and the estimate is 0.
    /// - A dt_s that is not > 0 (or not finite) changes nothing and returns
    ///   the previous estimate.
    /// - The count delta is taken in unsigned arithmetic, so an int32 wrap
    ///   between samples still gives the right delta.
    /// - cutoff_hz <= 0 disables the filter.
    ///
    /// @return The new estimate in rad/s.
    float update(int32_t count, float dt_s, float counts_per_rev, float gear_ratio, float cutoff_hz);

    /// Forget the previous count, so the next update() re-primes. Use it when
    /// the encoder count jumps (for example after the count is cleared).
    void reset();

    /// @return The current estimate in rad/s (0 until the second sample).
    float estimate() const { return m_estimate; }

    /// @return true once a first count has been recorded.
    bool isPrimed() const { return m_primed; }

private:
    bool m_primed = false;
    int32_t m_prev_count = 0;
    float m_estimate = 0.0f;
};

} // namespace mod_motor
