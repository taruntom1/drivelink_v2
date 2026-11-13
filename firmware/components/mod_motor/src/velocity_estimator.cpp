#include "mod_motor/velocity_estimator.hpp"

#include <cmath>

namespace mod_motor {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

} // namespace

float countsToRadians(int32_t counts, float counts_per_rev, float gear_ratio)
{
    return static_cast<float>(counts) * kTwoPi / (counts_per_rev * gear_ratio);
}

float countsPerSecToRadPerSec(float counts_per_sec, float counts_per_rev, float gear_ratio)
{
    return counts_per_sec * kTwoPi / (counts_per_rev * gear_ratio);
}

float lowPassAlpha(float cutoff_hz, float dt_s)
{
    if (cutoff_hz <= 0.0f) {
        return 1.0f;
    }
    return 1.0f - std::exp(-kTwoPi * cutoff_hz * dt_s);
}

float VelocityEstimator::update(int32_t count, float dt_s, float counts_per_rev, float gear_ratio,
                                float cutoff_hz)
{
    if (!m_primed) {
        // First sample: there is no previous count to difference against.
        m_prev_count = count;
        m_estimate = 0.0f;
        m_primed = true;
        return m_estimate;
    }

    if (!(dt_s > 0.0f) || !std::isfinite(dt_s)) {
        return m_estimate;
    }

    // The PCNT accumulator makes the count effectively unbounded; difference
    // in unsigned arithmetic so a wrap of the int32 value stays well-defined.
    const int32_t delta = static_cast<int32_t>(
        static_cast<uint32_t>(count) - static_cast<uint32_t>(m_prev_count));
    m_prev_count = count;

    const float raw = countsPerSecToRadPerSec(static_cast<float>(delta) / dt_s, counts_per_rev, gear_ratio);
    const float alpha = lowPassAlpha(cutoff_hz, dt_s);
    m_estimate += alpha * (raw - m_estimate);
    return m_estimate;
}

void VelocityEstimator::reset()
{
    m_primed = false;
    m_prev_count = 0;
    m_estimate = 0.0f;
}

} // namespace mod_motor
