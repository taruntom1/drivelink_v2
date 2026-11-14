#include "doctest.h"

#include "drivelink/protocol/crc.hpp"

#include <array>

using drivelink::protocol::crc16Ccitt;

TEST_CASE("crc16Ccitt matches the CRC-16/CCITT-FALSE check value")
{
    const std::array<uint8_t, 9> text = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc16Ccitt(text.data(), text.size()) == 0x29B1);
}

TEST_CASE("crc16Ccitt of no data is the initial value")
{
    CHECK(crc16Ccitt(nullptr, 0) == 0xFFFF);
    const uint8_t byte = 0;
    CHECK(crc16Ccitt(&byte, 0) == 0xFFFF);
}

TEST_CASE("crc16Ccitt known vectors")
{
    // The Python reference in firmware/test/uart_comm/protocol.py gives the same values.
    const std::array<uint8_t, 1> zero = {0x00};
    CHECK(crc16Ccitt(zero.data(), zero.size()) == 0xE1F0);
    const std::array<uint8_t, 4> seq = {1, 2, 3, 4};
    CHECK(crc16Ccitt(seq.data(), seq.size()) == 0x89C3);
}

TEST_CASE("crc16Ccitt changes with every byte")
{
    std::array<uint8_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint16_t base = crc16Ccitt(data.data(), data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] ^= 0x01U;
        CHECK(crc16Ccitt(data.data(), data.size()) != base);
        data[i] ^= 0x01U;
    }
}

using drivelink::protocol::crc8;

TEST_CASE("crc8 matches the CRC-8/SMBUS check value")
{
    const std::array<uint8_t, 9> text = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc8(text.data(), text.size()) == 0xF4);
}

TEST_CASE("crc8 of no data is the initial value, and known vectors")
{
    CHECK(crc8(nullptr, 0) == 0x00);
    // Computed bit by bit in Python (the same values protocol.py gives).
    const std::array<uint8_t, 1> zero = {0x00};
    CHECK(crc8(zero.data(), zero.size()) == 0x00);
    const std::array<uint8_t, 4> seq = {1, 2, 3, 4};
    CHECK(crc8(seq.data(), seq.size()) == 0xE3);
}

TEST_CASE("crc8 catches every single-bit error in a 3-byte header")
{
    std::array<uint8_t, 3> header = {0x41, 0x03, 0x00};
    const uint8_t base = crc8(header.data(), header.size());
    for (size_t i = 0; i < header.size(); ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            header[i] ^= static_cast<uint8_t>(1U << bit);
            CHECK(crc8(header.data(), header.size()) != base);
            header[i] ^= static_cast<uint8_t>(1U << bit);
        }
    }
}
