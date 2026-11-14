#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace drivelink {
struct TimeSample {
    uint16_t id = 0;
    uint64_t host_tx_us = 0, esp_rx_us = 0, esp_tx_us = 0, host_rx_us = 0;
    uint64_t boot_generation = 0;
};
// The host clock domain is steady_clock microseconds, never UTC.
uint64_t monotonicUs();
struct ClockOptions {
    uint64_t window_us = 120000000, bin_us = 5000000, min_span_us = 30000000,
             stale_us = 5000000;
    size_t min_samples = 8, capacity = 2048;
};
struct ClockModel {
    bool offset_valid = false, drift_valid = false;
    uint64_t device_reference_us = 0, host_reference_us = 0, updated_us = 0, span_us = 0, age_us = 0;
    size_t samples = 0;
    double slope = 1, drift_ppm = 0, residual_us = 0, min_rtt_us = 0, max_rtt_us = 0;
    double map(uint64_t device_us) const;
};
// One caller owns an estimator. Fixed capacity is allocated at construction.
// add() rejects bad samples; Boot/generation or backwards device time resets it.
class ClockEstimator {
  public:
    explicit ClockEstimator(ClockOptions options = {});
    bool add(const TimeSample &sample);
    ClockModel model(uint64_t host_now_us) const;
    void reset();
    static bool measure(const TimeSample &sample, double &rtt, double &device, double &host);
  private:
    struct Point { uint64_t received; double device, host, rtt; };
    ClockOptions m_options;
    std::vector<Point> m_points;
    mutable std::vector<size_t> m_selected;
    uint64_t m_generation = 0, m_last_device = 0, m_last_host = 0;
};
}
