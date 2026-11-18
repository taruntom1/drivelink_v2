#pragma once
// Tuning math (H2e): pure, deterministic and unit-tested, so the GUI and the
// CLI compute the same numbers. No device access.
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace drivelink {
struct StepOptions
{
    double band = 0.02;  // settling band, a fraction of the step size
    double tail = 0.10;  // steady-state window, a fraction of the samples after the step
    double end_time = std::numeric_limits<double>::infinity(); // ignore samples after it
};
// The response to one step of the target from `from` to `to` at `step_time`.
// Every metric is relative to the step size, so a negative step works the same.
struct StepMetrics
{
    std::optional<double> rise_s;        // first 10 % crossing to the first 90 % crossing
    std::optional<double> overshoot_pct; // the peak beyond `to`, in % of the step (0 when none)
    std::optional<double> settling_s;    // from the step to the last entry into the band
    std::optional<double> steady_error;  // `to` minus the mean response over the tail
    std::string problem;                 // why a metric is missing; empty when all are computed
};
// `t` (seconds, increasing) and `response` have the same length; NaN samples
// are skipped. A sample at exactly `step_time` counts as after the step.
StepMetrics stepMetrics(std::span<const double> t, std::span<const double> response, double step_time, double from,
                        double to, const StepOptions &options = {});
} // namespace drivelink
