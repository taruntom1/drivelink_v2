#include "doctest.h"

#include "drivelink/protocol/crc.hpp"
#include "drivelink/protocol/framing.hpp"

#include <array>
#include <vector>

using namespace drivelink::protocol;

TEST_CASE("Frame constants")
{
    CHECK(kHeaderLen == 4);
    CHECK(kOverhead == 9);
    CHECK(kMaxPayload == 1024);
    CHECK(frameSize(0) == 9);
    CHECK(frameSize(kMaxPayload) == 1033);
}

TEST_CASE("encodeFrame lays out preamble, header, HCRC, payload and little-endian CRC")
{
    const std::array<uint8_t, 3> payload = {0x11, 0x22, 0x33};
    std::array<uint8_t, 16> buf{};
    const size_t n = encodeFrame(buf.data(), buf.size(), 0x41, payload.data(), payload.size());

    REQUIRE(n == 12);
    CHECK(n == frameSize(payload.size()));
    CHECK(buf[0] == 0xAA);
    CHECK(buf[1] == 0xAA);
    CHECK(buf[2] == 0xAA);
    CHECK(buf[3] == 0x41);
    CHECK(buf[4] == 3); // LEN low byte
    CHECK(buf[5] == 0); // LEN high byte
    CHECK(buf[6] == crc8(&buf[3], 3));
    CHECK(buf[7] == 0x11);
    CHECK(buf[8] == 0x22);
    CHECK(buf[9] == 0x33);

    const uint16_t crc = crc16Ccitt(&buf[3], 4 + payload.size());
    CHECK(buf[10] == (crc & 0xFF));
    CHECK(buf[11] == (crc >> 8));
    CHECK(headerCrcValid(&buf[3]));
    CHECK(headerPayloadLen(&buf[3]) == 3);
    CHECK(validateCrc(&buf[3], payload.size()));
}

TEST_CASE("encodeFrame matches the golden frames")
{
    // Computed bit by bit in Python (CRC-8 poly 0x07, CRC-16/CCITT-FALSE), the
    // same bytes firmware/test/uart_comm/protocol.py produces. Also the worked
    // example in docs/protocol/framing.md.
    const std::array<uint8_t, 3> payload = {0x11, 0x22, 0x33};
    std::array<uint8_t, 12> buf{};
    REQUIRE(encodeFrame(buf.data(), buf.size(), 0x41, payload.data(), payload.size()) == 12);
    const std::array<uint8_t, 12> golden = {0xAA, 0xAA, 0xAA, 0x41, 0x03, 0x00, 0xD2, 0x11, 0x22, 0x33, 0x2F, 0x0D};
    CHECK(buf == golden);

    std::array<uint8_t, 9> empty{};
    REQUIRE(encodeFrame(empty.data(), empty.size(), 0x80, nullptr, 0) == 9);
    const std::array<uint8_t, 9> golden_empty = {0xAA, 0xAA, 0xAA, 0x80, 0x00, 0x00, 0x0B, 0x93, 0xE8};
    CHECK(empty == golden_empty);

    // A LEN above 255 uses both LEN bytes.
    std::vector<uint8_t> big(300);
    for (size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> big_buf(frameSize(big.size()));
    REQUIRE(encodeFrame(big_buf.data(), big_buf.size(), 0x63, big.data(), big.size()) == big_buf.size());
    CHECK(big_buf[3] == 0x63);
    CHECK(big_buf[4] == 0x2C);
    CHECK(big_buf[5] == 0x01);
    CHECK(big_buf[6] == 0x2D);
    CHECK(big_buf[big_buf.size() - 2] == 0x22);
    CHECK(big_buf[big_buf.size() - 1] == 0x71);
    CHECK(headerPayloadLen(&big_buf[3]) == 300);
}

TEST_CASE("encodeFrame with an empty payload")
{
    std::array<uint8_t, 9> buf{};
    CHECK(encodeFrame(buf.data(), buf.size(), 0x42, nullptr, 0) == 9);
    CHECK(buf[4] == 0);
    CHECK(buf[5] == 0);
    CHECK(headerCrcValid(&buf[3]));
    CHECK(validateCrc(&buf[3], 0));
}

TEST_CASE("encodeFrame of the largest payload")
{
    std::vector<uint8_t> payload(kMaxPayload, 0x5A);
    std::vector<uint8_t> buf(frameSize(kMaxPayload));
    CHECK(encodeFrame(buf.data(), buf.size(), 0x80, payload.data(), payload.size()) == buf.size());
    CHECK(buf[4] == 0x00);
    CHECK(buf[5] == 0x04);
    CHECK(headerPayloadLen(&buf[3]) == kMaxPayload);
    CHECK(headerCrcValid(&buf[3]));
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

TEST_CASE("headerCrcValid rejects any corrupted header byte")
{
    std::array<uint8_t, 16> buf{};
    const std::array<uint8_t, 2> payload = {7, 8};
    REQUIRE(encodeFrame(buf.data(), buf.size(), 0x41, payload.data(), payload.size()) > 0);
    for (size_t i = 3; i < 7; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            buf[i] ^= static_cast<uint8_t>(1U << bit);
            CHECK_FALSE(headerCrcValid(&buf[3]));
            buf[i] ^= static_cast<uint8_t>(1U << bit);
        }
    }
    CHECK(headerCrcValid(&buf[3]));
}

TEST_CASE("validateCrc rejects a corrupted frame, header and HCRC included")
{
    const std::array<uint8_t, 2> payload = {7, 8};
    std::array<uint8_t, 16> buf{};
    const size_t n = encodeFrame(buf.data(), buf.size(), 0x41, payload.data(), payload.size());
    REQUIRE(n > 0);
    for (size_t i = 3; i < n; ++i) {
        // Corrupting a LEN byte changes the length validateCrc reads, so only
        // flip bytes it covers at the encoded length: all of them.
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
