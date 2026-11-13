#pragma once
#include <cstdint>
namespace core_os::native {
/// One-shot setup/config failure injection; -1 disables it.
void failPeriodicStartAfter(int32_t successes);
void failNextPeriodChange();
void failPeriodChangeAfter(int32_t successes);
// Deterministic tests only. Change only while all tasks/timers are stopped.
void setManualScheduling(bool manual);
bool manualScheduling();
}
