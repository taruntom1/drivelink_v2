#include "time_sync_protocol.hpp"

#include "drivelink/protocol/wire_codec.hpp"

namespace time_sync::protocol {

size_t encodeRequest(const TimeSyncRequest& req, uint8_t* out, size_t out_size)
{
    drivelink::protocol::wire::Writer w(out, out_size);
    w.appendU8(kProtocolVersion);
    w.appendU8(0); // reserved
    w.appendU16Le(req.request_id);
    w.appendI64Le(req.client_unix_send_us);
    return w.ok() ? w.size() : 0U;
}

bool decodeRequest(const uint8_t* payload, size_t len, TimeSyncRequest& out)
{
    if (len != kRequestEncodedSize) {
        return false;
    }
    drivelink::protocol::wire::Reader r(payload, len);
    if (r.readU8() != kProtocolVersion) {
        return false;
    }
    r.skip(1); // reserved
    TimeSyncRequest req{};
    req.request_id = r.readU16Le();
    req.client_unix_send_us = r.readI64Le();
    if (!r.done()) {
        return false;
    }
    out = req;
    return true;
}

size_t encodeResponse(const TimeSyncResponse& resp, uint8_t* out, size_t out_size)
{
    drivelink::protocol::wire::Writer w(out, out_size);
    w.appendU8(kProtocolVersion);
    w.appendU8(resp.status);
    w.appendU16Le(resp.request_id);
    w.appendI64Le(resp.echoed_client_unix_send_us);
    w.appendI64Le(resp.server_unix_rx_us);
    w.appendI64Le(resp.server_unix_tx_us);
    w.appendU16Le(0); // reserved
    return w.ok() ? w.size() : 0U;
}

bool decodeResponse(const uint8_t* payload, size_t len, TimeSyncResponse& out)
{
    if (len != kResponseEncodedSize) {
        return false;
    }
    drivelink::protocol::wire::Reader r(payload, len);
    if (r.readU8() != kProtocolVersion) {
        return false;
    }
    TimeSyncResponse resp{};
    resp.status = r.readU8();
    resp.request_id = r.readU16Le();
    resp.echoed_client_unix_send_us = r.readI64Le();
    resp.server_unix_rx_us = r.readI64Le();
    resp.server_unix_tx_us = r.readI64Le();
    r.skip(2); // reserved
    if (!r.done()) {
        return false;
    }
    out = resp;
    return true;
}

} // namespace time_sync::protocol
