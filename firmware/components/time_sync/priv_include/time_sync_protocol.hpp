#pragma once

#include <cstddef>
#include <cstdint>

namespace time_sync::protocol {

constexpr uint8_t kProtocolVersion = 1;

struct TimeSyncRequest {
    uint16_t request_id;
    int64_t client_unix_send_us;
};

struct TimeSyncResponse {
    uint16_t request_id;
    uint8_t status;
    int64_t echoed_client_unix_send_us;
    int64_t server_unix_rx_us;
    int64_t server_unix_tx_us;
};

constexpr size_t kRequestEncodedSize = 12;
constexpr size_t kResponseEncodedSize = 30;

size_t encodeRequest(const TimeSyncRequest& req, uint8_t* out, size_t out_size);
bool decodeRequest(const uint8_t* payload, size_t len, TimeSyncRequest& out);

size_t encodeResponse(const TimeSyncResponse& resp, uint8_t* out, size_t out_size);
bool decodeResponse(const uint8_t* payload, size_t len, TimeSyncResponse& out);

} // namespace time_sync::protocol
