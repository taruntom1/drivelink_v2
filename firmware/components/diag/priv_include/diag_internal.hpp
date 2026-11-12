#pragma once

/// Shared state between the reporting API (diag.cpp) and the Reporter.

#include <cstdint>

#include "diag_table.hpp"
#include "freertos/FreeRTOS.h"

namespace diag::detail {

enum class QueuedKind : uint8_t {
    Record,
    Cleared,
};

/// One item in the record queue. Latched when it is reported, so the
/// timestamp and the time_synced flag are those of the report instant.
struct QueuedEvent {
    uint64_t timestamp_us = 0;
    int32_t esp_err = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    uint8_t slot = 0;
    QueuedKind kind = QueuedKind::Record;
    uint8_t flags = 0; ///< reserved
};

/// The one slot table.
SlotTable &table();

/// Look at the oldest queued event without removing it. Single consumer
/// (the Reporter task). @return false if the queue is empty or not created.
bool peekEvent(QueuedEvent &out);

/// Remove the oldest queued event (after peekEvent() returned it).
void popEvent();

/// Monotonic microseconds from boot.
uint64_t wallClockUs();

/// Reserved flags, always zero.
uint8_t currentFlags();

} // namespace diag::detail
