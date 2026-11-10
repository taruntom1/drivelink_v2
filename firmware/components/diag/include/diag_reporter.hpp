#pragma once

#include <cstdint>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

namespace uart_comm {
class UartComm;
}

namespace diag {

/// GetInfo / Boot `started_mask` bits: which managers app_main started.
constexpr uint8_t kStartedMotorManager = 0x01;
constexpr uint8_t kStartedTimeSync = 0x02;
constexpr uint8_t kStartedImu = 0x04;

/// Manager ids used as arg0 of system.ManagerStartFailed.
constexpr uint32_t kManagerIdMotorManager = 1;
constexpr uint32_t kManagerIdTimeSync = 2;
constexpr uint32_t kManagerIdImu = 3;

struct ReporterConfig {
    uint8_t event_msg_type = CONFIG_DIAG_EVENT_MSG_TYPE;                   ///< 0xC0–0xFE
    uint8_t query_msg_type = CONFIG_DIAG_QUERY_MSG_TYPE;                   ///< 0x01–0x3F
    uint8_t query_response_msg_type = CONFIG_DIAG_QUERY_RESPONSE_MSG_TYPE; ///< 0x40–0x7F
    uint8_t started_mask = 0;                                              ///< kStarted* bits
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
/// started_mask); records reported before then wait in the queue.
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
