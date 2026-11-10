#include "doctest.h"

#include "time_sync_protocol.hpp"

#include <array>

using namespace time_sync::protocol;

TEST_CASE("time_sync request encodes to the v1 wire layout")
{
    TimeSyncRequest req{};
    req.request_id = 0x1234;
    req.client_unix_send_us = 0x0102030405060708LL;

    std::array<uint8_t, kRequestEncodedSize> buf{};
    REQUIRE(encodeRequest(req, buf.data(), buf.size()) == kRequestEncodedSize);

    const std::array<uint8_t, kRequestEncodedSize> expected = {
        kProtocolVersion, 0x00,                         // version, reserved
        0x34, 0x12,                                     // request_id
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // client_unix_send_us
    };
    CHECK(buf == expected);
}

TEST_CASE("time_sync request encode fails on a short buffer")
{
    std::array<uint8_t, kRequestEncodedSize - 1> buf{};
    CHECK(encodeRequest(TimeSyncRequest{}, buf.data(), buf.size()) == 0);
    CHECK(encodeRequest(TimeSyncRequest{}, nullptr, 64) == 0);
}

TEST_CASE("time_sync request round-trips")
{
    TimeSyncRequest req{};
    req.request_id = 65535;
    req.client_unix_send_us = -42;
    std::array<uint8_t, kRequestEncodedSize> buf{};
    REQUIRE(encodeRequest(req, buf.data(), buf.size()) == kRequestEncodedSize);

    TimeSyncRequest out{};
    REQUIRE(decodeRequest(buf.data(), buf.size(), out));
    CHECK(out.request_id == req.request_id);
    CHECK(out.client_unix_send_us == req.client_unix_send_us);
}

TEST_CASE("time_sync response round-trips")
{
    TimeSyncResponse resp{};
    resp.request_id = 7;
    resp.status = 2;
    resp.echoed_client_unix_send_us = 1700000000000000LL;
    resp.server_unix_rx_us = 1700000000000100LL;
    resp.server_unix_tx_us = 1700000000000200LL;

    std::array<uint8_t, kResponseEncodedSize> buf{};
    REQUIRE(encodeResponse(resp, buf.data(), buf.size()) == kResponseEncodedSize);
    CHECK(buf[0] == kProtocolVersion);
    CHECK(buf[1] == 2);
    CHECK(buf[28] == 0); // reserved
    CHECK(buf[29] == 0);

    TimeSyncResponse out{};
    REQUIRE(decodeResponse(buf.data(), buf.size(), out));
    CHECK(out.request_id == 7);
    CHECK(out.status == 2);
    CHECK(out.echoed_client_unix_send_us == resp.echoed_client_unix_send_us);
    CHECK(out.server_unix_rx_us == resp.server_unix_rx_us);
    CHECK(out.server_unix_tx_us == resp.server_unix_tx_us);
}

TEST_CASE("time_sync decoders require the exact length and version")
{
    std::array<uint8_t, kResponseEncodedSize + 1> buf{};
    REQUIRE(encodeResponse(TimeSyncResponse{}, buf.data(), buf.size()) == kResponseEncodedSize);

    TimeSyncResponse resp{};
    CHECK_FALSE(decodeResponse(buf.data(), kResponseEncodedSize - 1, resp));
    CHECK_FALSE(decodeResponse(buf.data(), kResponseEncodedSize + 1, resp));
    CHECK_FALSE(decodeResponse(nullptr, kResponseEncodedSize, resp));
    buf[0] = kProtocolVersion + 1;
    CHECK_FALSE(decodeResponse(buf.data(), kResponseEncodedSize, resp));

    std::array<uint8_t, kRequestEncodedSize> req_buf{};
    REQUIRE(encodeRequest(TimeSyncRequest{}, req_buf.data(), req_buf.size()) == kRequestEncodedSize);
    TimeSyncRequest req{};
    CHECK_FALSE(decodeRequest(req_buf.data(), kRequestEncodedSize - 1, req));
    req_buf[0] = 0;
    CHECK_FALSE(decodeRequest(req_buf.data(), kRequestEncodedSize, req));
}
