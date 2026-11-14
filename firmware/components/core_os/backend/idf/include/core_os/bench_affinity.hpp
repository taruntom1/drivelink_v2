#pragma once

#include "sdkconfig.h"

#if CONFIG_CORE_BENCH_CRASH
namespace core_os {
/// Console-only bench observations. Call from the owner that serializes
/// PeriodicTask start/stop; never from a loop, ISR or timer callback.
void logBenchTaskAffinities();
} // namespace core_os
#endif
