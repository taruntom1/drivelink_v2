#pragma once

#include <cstdint>
#include <memory>

#include "drivelink/protocol/gen/registry.hpp"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace uart_comm {
class UartComm;
}

namespace diag {

struct ReporterConfig {
    uint8_t event_msg_type = drivelink::protocol::defs::msg::kDiagEvent;                   ///< 0xC0–0xFE
    uint8_t query_msg_type = drivelink::protocol::defs::msg::kDiagQuery;                   ///< 0x01–0x3F
    uint8_t query_response_msg_type = drivelink::protocol::defs::msg::kDiagQueryResponse; ///< 0x40–0x7F
    uint8_t instances_ok = 0;
    uint8_t instances_failed = 0;
    uint8_t safe_mode = 0;                                                              ///< safe-mode flag
    uint32_t task_stack_size = CONFIG_DIAG_TASK_STACK;
    UBaseType_t task_priority = CONFIG_DIAG_TASK_PRIO;
};

/// Sends diag events to the host and answers diag queries.
///
/// Owns one task that, in order: sends the Boot event once, sends queued
/// records and Cleared events, answers queries, and every
/// CONFIG_DIAG_COUNTER_PERIOD_MS sends CounterDelta events for changed
/// counters (after folding in UartComm::getErrorCounters()). When the
/// UART TX queue is full, events stay queued and are retried; nothing is
/// lost except query responses (counted as diag.EventTxDropped).
///
/// Construct it after the managers (so Boot carries the final
/// instance counts); records reported before then wait in the queue.
/// Must not outlive the UartComm. Calls diag::init() itself.
class Reporter {
public:
    Reporter(uart_comm::UartComm &uart, const ReporterConfig &config = {});
    ~Reporter();

    Reporter(const Reporter &) = delete;
    Reporter &operator=(const Reporter &) = delete;
    Reporter(Reporter &&) = delete;
    Reporter &operator=(Reporter &&) = delete;

    /// True when the query callback is registered and the task is running.
    bool isValid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace diag
