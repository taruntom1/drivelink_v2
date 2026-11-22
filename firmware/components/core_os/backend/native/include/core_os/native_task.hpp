#pragma once
#include <cstddef>
#include <cstdint>
namespace core_os::native {
using PeriodicStartObserver = void (*)(const char *name, int8_t core);
/// Setup-only test observation; change only while all periodic tasks are stopped.
PeriodicStartObserver setPeriodicStartObserver(PeriodicStartObserver observer);
/// One-shot setup/config failure injection; -1 disables it.
void failPeriodicStartAfter(int32_t successes);
void failNextPeriodChange();
void failPeriodChangeAfter(int32_t successes);
// Deterministic tests only. Change only while all tasks/timers are stopped.
void setManualScheduling(bool manual);
bool manualScheduling();
// Capture memory: the capacity a later Core reports, and one failing allocation.
void setCaptureCapacity(size_t bytes);
void failNextCaptureAlloc();
}
