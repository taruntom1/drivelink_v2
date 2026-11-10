#include "doctest.h"

#include "drivelink/protocol/crc.hpp"
#include "drivelink/protocol/framing.hpp"

#include <array>
#include <vector>

using namespace drivelink::protocol;

TEST_CASE("encodeFrame lays out preamble, header, payload and little-endian CRC")
{
    const std::array<uint8_t, 3> payload = {0x11, 0x22, 0x33};
    std::array<uint8_t, 16> buf{};
    const size_t n = encodeFrame(buf.data(), buf.size(), 0x41, payload.data(), payload.size());

    REQUIRE(n == 10);
    CHECK(n == frameSize(payload.size()));
    CHECK(buf[0] == 0xAA);
    CHECK(buf[1] == 0xAA);
    CHECK(buf[2] == 0xAA);
    CHECK(buf[3] == 0x41);
    CHECK(buf[4] == 3);
    CHECK(buf[5] == 0x11);
    CHECK(buf[6] == 0x22);
    CHECK(buf[7] == 0x33);

    const uint16_t crc = crc16Ccitt(&buf[3], 2 + payload.size());
    CHECK(buf[8] == (crc & 0xFF));
    CHECK(buf[9] == (crc >> 8));
    CHECK(validateCrc(&buf[3], payload.size()));
}

TEST_CASE("encodeFrame with an empty payload")
{
    std::array<uint8_t, 8> buf{};
    CHECK(encodeFrame(buf.data(), buf.size(), 0x42, nullptr, 0) == 7);
    CHECK(buf[4] == 0);
    CHECK(validateCrc(&buf[3], 0));
}

TEST_CASE("encodeFrame of the largest payload")
{
    std::vector<uint8_t> payload(kMaxPayload, 0x5A);
    std::vector<uint8_t> buf(frameSize(kMaxPayload));
    CHECK(encodeFrame(buf.data(), buf.size(), 0x80, payload.data(), payload.size()) == buf.size());
    CHECK(buf[4] == 255);
    CHECK(validateCrc(&buf[3], kMaxPayload));
}

TEST_CASE("encodeFrame rejects bad arguments and writes nothing")
{
    const std::array<uint8_t, 4> payload = {1, 2, 3, 4};
    std::array<uint8_t, 16> buf;
    buf.fill(0xEE);

    CHECK(encodeFrame(buf.data(), frameSize(4) - 1, 0x41, payload.data(), 4) == 0); // does not fit
    CHECK(encodeFrame(buf.data(), buf.size(), 0x41, nullptr, 4) == 0);              // null payload
    CHECK(encodeFrame(nullptr, 16, 0x41, payload.data(), 4) == 0);                  // null buffer
    std::vector<uint8_t> big(kMaxPayload + 1);
    std::vector<uint8_t> big_buf(frameSize(kMaxPayload + 1));
    CHECK(encodeFrame(big_buf.data(), big_buf.size(), 0x41, big.data(), big.size()) == 0); // LEN too large
    for (uint8_t b : buf) {
        CHECK(b == 0xEE);
    }
}

TEST_CASE("validateCrc rejects a corrupted frame")
{
    const std::array<uint8_t, 2> payload = {7, 8};
    std::array<uint8_t, 16> buf{};
    const size_t n = encodeFrame(buf.data(), buf.size(), 0x41, payload.data(), payload.size());
    REQUIRE(n > 0);
    for (size_t i = 3; i < n; ++i) {
        buf[i] ^= 0x10U;
        CHECK_FALSE(validateCrc(&buf[3], payload.size()));
        buf[i] ^= 0x10U;
    }
    CHECK(validateCrc(&buf[3], payload.size()));
}

TEST_CASE("MSG_TYPE range predicates")
{
    CHECK_FALSE(isCommand(0x00));
    CHECK(isCommand(0x01));
    CHECK(isCommand(0x3F));
    CHECK(isResponse(0x40));
    CHECK(isResponse(0x7F));
    CHECK(isStream(0x80));
    CHECK(isStream(0xBF));
    CHECK(isEvent(0xC0));
    CHECK(isEvent(0xFE));
    CHECK_FALSE(isEvent(0xFF));
    CHECK(kStreamChannelCount == 64);
}
