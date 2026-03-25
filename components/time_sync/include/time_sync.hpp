#pragma once

#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "uart_comm.hpp"
#include "time_sync_protocol.hpp"

namespace time_sync {

struct TimeSyncConfig {
    uint32_t sync_interval_ms = 0;
    uint32_t response_timeout_ms = 250;
    bool enable_round_trip_compensation = true;
    uint8_t request_msg_type = protocol::kDefaultRequestMsgType;
    uint8_t response_msg_type = protocol::kDefaultResponseMsgType;
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

    static void periodicTaskEntry(void* arg);
    void periodicTask();
    void onResponseFrame(const uart_comm::RxFrame& frame);
    esp_err_t performSync(TickType_t timeout);
};

} // namespace time_sync
