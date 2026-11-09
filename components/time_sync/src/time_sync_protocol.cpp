#include "time_sync_protocol.hpp"

namespace {

void writeU16LE(uint8_t* dst, uint16_t value)
{
    dst[0] = static_cast<uint8_t>(value & 0xFFU);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void writeI64LE(uint8_t* dst, int64_t value)
{
    uint64_t u = static_cast<uint64_t>(value);
    for (size_t i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>((u >> (i * 8U)) & 0xFFU);
    }
}

uint16_t readU16LE(const uint8_t* src)
{
    return static_cast<uint16_t>(src[0]) |
           (static_cast<uint16_t>(src[1]) << 8);
}

int64_t readI64LE(const uint8_t* src)
{
    uint64_t u = 0;
    for (size_t i = 0; i < 8; ++i) {
        u |= static_cast<uint64_t>(src[i]) << (i * 8U);
    }
    return static_cast<int64_t>(u);
}

} // namespace

namespace time_sync::protocol {

size_t encodeRequest(const TimeSyncRequest& req, uint8_t* out, size_t out_size)
{
    if (out == nullptr || out_size < kRequestEncodedSize) {
        return 0;
    }

    out[0] = kProtocolVersion;
    out[1] = 0;
    writeU16LE(out + 2, req.request_id);
    writeI64LE(out + 4, req.client_unix_send_us);
    return kRequestEncodedSize;
}

bool decodeRequest(const uint8_t* payload, size_t len, TimeSyncRequest& out)
{
    if (payload == nullptr || len != kRequestEncodedSize) {
        return false;
    }

    if (payload[0] != kProtocolVersion) {
        return false;
    }

    out.request_id = readU16LE(payload + 2);
    out.client_unix_send_us = readI64LE(payload + 4);
    return true;
}

size_t encodeResponse(const TimeSyncResponse& resp, uint8_t* out, size_t out_size)
{
    if (out == nullptr || out_size < kResponseEncodedSize) {
        return 0;
    }

    out[0] = kProtocolVersion;
    out[1] = resp.status;
    writeU16LE(out + 2, resp.request_id);
    writeI64LE(out + 4, resp.echoed_client_unix_send_us);
    writeI64LE(out + 12, resp.server_unix_rx_us);
    writeI64LE(out + 20, resp.server_unix_tx_us);
    out[28] = 0;
    out[29] = 0;
    return kResponseEncodedSize;
}

bool decodeResponse(const uint8_t* payload, size_t len, TimeSyncResponse& out)
{
    if (payload == nullptr || len != kResponseEncodedSize) {
        return false;
    }

    if (payload[0] != kProtocolVersion) {
        return false;
    }

    out.status = payload[1];
    out.request_id = readU16LE(payload + 2);
    out.echoed_client_unix_send_us = readI64LE(payload + 4);
    out.server_unix_rx_us = readI64LE(payload + 12);
    out.server_unix_tx_us = readI64LE(payload + 20);
    return true;
}

} // namespace time_sync::protocol
