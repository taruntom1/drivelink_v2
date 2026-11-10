#pragma once

#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "uart_comm.hpp"
#include "time_sync_types.hpp"

namespace time_sync {

struct TimeSyncConfig {
    uint32_t sync_interval_ms = 0;
    uint32_t response_timeout_ms = 250;
    bool enable_round_trip_compensation = true;
    uint8_t request_msg_type = kDefaultRequestMsgType;
    uint8_t response_msg_type = kDefaultResponseMsgType;
    uint32_t task_stack_size = kDefaultTaskStackSize;
    UBaseType_t task_priority = kDefaultTaskPriority;
};

class TimeSync {
public:
    explicit TimeSync(uart_comm::UartComm& uart, const TimeSyncConfig& config = {});
    ~TimeSync();

    TimeSync(const TimeSync&) = delete;
    TimeSync& operator=(const TimeSync&) = delete;
    TimeSync(TimeSync&&) = delete;
    TimeSync& operator=(TimeSync&&) = delete;

    bool isValid() const;

    esp_err_t triggerManualSync(bool wait_for_response = true, TickType_t timeout = pdMS_TO_TICKS(250));

    esp_err_t startPeriodicSync();
    void stopPeriodicSync();
    bool isPeriodicSyncRunning() const;

    int64_t getEstimatedOffsetUs() const;
    uint32_t getLastRoundTripUs() const;
    int64_t getLastServerUnixUs() const;
    uint64_t getLastSyncMonotonicUs() const;

private:
    struct Impl;
    Impl* m_impl;

    void periodicTask();
    void onResponseFrame(const uart_comm::RxFrame& frame);
    /// On failure, `failed_stage` is the time_sync.SyncFailed stage and
    /// `host_status` the host's status byte (stage 3 only). `failed_stage`
    /// stays 0 when another sync held the lock for the whole timeout.
    esp_err_t performSync(TickType_t timeout, uint8_t& failed_stage, uint8_t& host_status);
};

} // namespace time_sync
