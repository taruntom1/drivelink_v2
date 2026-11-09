#pragma once

#include <cstdint>
#include <sys/time.h>

namespace imu_bmo_055::detail {

/// Wall-clock time in us (gettimeofday): the clock time_sync corrects, and
/// the one motor telemetry is stamped with. It can step when time_sync sets
/// the clock.
inline uint64_t wallClockUs()
{
    timeval now{};
    gettimeofday(&now, nullptr);
    return (static_cast<uint64_t>(now.tv_sec) * 1000000ULL) + static_cast<uint64_t>(now.tv_usec);
}

} // namespace imu_bmo_055::detail
