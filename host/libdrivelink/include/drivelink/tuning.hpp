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

// Plant identification (H2e-3): a first-order-plus-dead-time model of the
// response to one step of an open-loop input,
//   y(t) = y0 + K·Δu·(1 − exp(−(t − ts − L)/T))  for t > ts + L, else y0.
struct FopdtOptions
{
    double tail = 0.20;         // the final level: the mean of this fraction of the samples after the step
    double baseline = 0.20;     // the start level: the mean of this fraction of the level before the step
    double settled = 0.05;      // settled when the final window drifts less than this fraction of Δy
    double min_snr = 5.0;       // |Δy| must exceed this many standard errors of the two levels (and one sample's noise)
};
struct FopdtFit
{
    // The step, found in the input: the last change to its final value.
    double step_time = 0, input_from = 0, input_to = 0;
    double response_from = 0, response_to = 0; // baseline and final means
    std::optional<double> gain;                 // K: response change per unit of input change
    std::optional<double> time_constant_s;      // T
    std::optional<double> dead_time_s;          // L, from the step
    // RMS residual after the step beyond the noise, in % of |Δy|.
    std::optional<double> error_pct;
    double noise = 0;                           // the tail's standard deviation around its trend
    bool settled = false;                       // the tail drifts less than 5 % of Δy or than its noise explains
    bool steady_start = false;                  // the same test on the start level (a pre-level still moving)
    std::string problem;                        // why the model is missing or not usable; empty when good to use
};
// `t` (seconds, increasing), `input` and `response` have the same length;
// samples with a NaN are skipped.
FopdtFit fitFopdt(std::span<const double> t, std::span<const double> input, std::span<const double> response,
                  const FopdtOptions &options = {});
// The model at time `t` (NaN without a model).
double fopdtValue(const FopdtFit &, double t);
enum class FitQuality { Good, Fair, Poor };
// Good up to 5 %, fair up to 15 %, poor above (or no error).
FitQuality fitQuality(const FopdtFit &);

// SIMC (Skogestad) PI for a parallel PID u = kp·e + ki·∫e dt + kd·de/dt:
// kp = T / (K(τc + L)), ki = kp / min(T, 4(τc + L)), kd = 0. τc is 3L
// (Robust), L (Balanced) or L/2 (Fast), at least four loop periods.
enum class TuneSpeed { Robust, Balanced, Fast };
struct PiGains
{
    double kp = 0, ki = 0, kd = 0;
    double closed_loop_s = 0; // τc
    std::string problem;      // why no gains are suggested; empty when they are
};
PiGains simcPi(const FopdtFit &, TuneSpeed, double loop_period_s);
} // namespace drivelink
