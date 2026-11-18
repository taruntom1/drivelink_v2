#include "drivelink/tuning.hpp"

#include <cmath>
#include <vector>

namespace drivelink {
namespace {
struct Point { double t, z, y; };
// The time at which the segment a→b crosses `level` (a.z < level <= b.z).
double crossing(const Point &a, const Point &b, double level)
{
    if (b.z == a.z) { return b.t; }
    return a.t + (b.t - a.t) * (level - a.z) / (b.z - a.z);
}
} // namespace

StepMetrics stepMetrics(std::span<const double> t, std::span<const double> response, double step_time, double from,
                        double to, const StepOptions &options)
{
    StepMetrics m;
    const double size = to - from;
    if (t.size() != response.size()) { m.problem = "time and response lengths differ"; return m; }
    if (!std::isfinite(size) || std::fabs(size) <= 1e-12 * std::max(1.0, std::fabs(to))) {
        m.problem = "no step";
        return m;
    }
    // Normalized response after the step: 0 at `from`, 1 at `to`.
    std::vector<Point> p;
    p.reserve(t.size());
    for (size_t i = 0; i < t.size(); ++i) {
        if (!std::isfinite(t[i]) || !std::isfinite(response[i]) || t[i] < step_time || t[i] > options.end_time) {
            continue;
        }
        p.push_back({t[i], (response[i] - from) / size, response[i]});
    }
    if (p.size() < 3) { m.problem = "too few samples after the step"; return m; }

    // Rise: the first crossing of 10 %, then the first crossing of 90 %.
    std::optional<double> t10, t90;
    for (size_t i = 0; i < p.size(); ++i) {
        const Point prev = i == 0 ? Point{step_time, p[0].z, p[0].y} : p[i - 1];
        if (!t10 && p[i].z >= 0.1) { t10 = i == 0 ? p[0].t : crossing(prev, p[i], 0.1); }
        if (t10 && p[i].z >= 0.9) { t90 = i == 0 ? p[0].t : crossing(prev, p[i], 0.9); break; }
    }
    if (t10 && t90) { m.rise_s = *t90 - *t10; }

    double peak = p.front().z;
    for (const auto &q : p) { peak = std::max(peak, q.z); }
    m.overshoot_pct = std::max(0.0, peak - 1.0) * 100.0;

    // Settling: the sample after the last one outside the band.
    std::optional<size_t> outside;
    for (size_t i = 0; i < p.size(); ++i) {
        if (std::fabs(p[i].z - 1.0) > options.band) { outside = i; }
    }
    if (!outside) { m.settling_s = p.front().t - step_time; }
    else if (*outside + 1 < p.size()) { m.settling_s = p[*outside + 1].t - step_time; }

    const size_t tail = std::max<size_t>(1, static_cast<size_t>(std::ceil(options.tail * static_cast<double>(p.size()))));
    double sum = 0;
    for (size_t i = p.size() - tail; i < p.size(); ++i) { sum += p[i].y; }
    m.steady_error = to - sum / static_cast<double>(tail);

    if (!m.rise_s) { m.problem = "the response does not reach 90 % of the step"; }
    else if (!m.settling_s) { m.problem = "the response does not settle within the window"; }
    return m;
}
} // namespace drivelink
