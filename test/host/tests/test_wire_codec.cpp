#include "doctest.h"

#include "wire_codec.hpp"

#include <array>
#include <cmath>
#include <limits>

using uart_comm::wire::Reader;
using uart_comm::wire::Writer;

TEST_CASE("Writer encodes little-endian fields")
{
    std::array<uint8_t, 32> buf{};
    Writer w(buf.data(), buf.size());
    w.appendU8(0xAB);
    w.appendU16Le(0x1234);
    w.appendU32Le(0xDEADBEEF);
    w.appendU64Le(0x0102030405060708ULL);
    w.appendI16Le(-2);
    REQUIRE(w.ok());
    CHECK(w.size() == 1 + 2 + 4 + 8 + 2);

    const std::array<uint8_t, 17> expected = {
        0xAB,
        0x34, 0x12,
        0xEF, 0xBE, 0xAD, 0xDE,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0xFE, 0xFF,
    };
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(buf[i] == expected[i]);
    }
}

TEST_CASE("Floats are IEEE-754 binary32, little-endian")
{
    std::array<uint8_t, 4> buf{};
    Writer w(buf.data(), buf.size());
    w.appendF32Le(1.0f); // 0x3F800000
    REQUIRE(w.ok());
    CHECK(buf[0] == 0x00);
    CHECK(buf[1] == 0x00);
    CHECK(buf[2] == 0x80);
    CHECK(buf[3] == 0x3F);

    Reader r(buf.data(), buf.size());
    CHECK(r.readF32Le() == 1.0f);
    CHECK(r.done());
}

TEST_CASE("Reader round-trips every field type")
{
    std::array<uint8_t, 64> buf{};
    Writer w(buf.data(), buf.size());
    w.appendU8(7);
    w.appendU16Le(65535);
    w.appendU32Le(123456789);
    w.appendU64Le(0xFFFFFFFFFFFFFFFFULL);
    w.appendI16Le(-32768);
    w.appendI32Le(-1);
    w.appendI64Le(std::numeric_limits<int64_t>::min());
    w.appendF32Le(-3.5f);
    REQUIRE(w.ok());

    Reader r(buf.data(), w.size());
    CHECK(r.readU8() == 7);
    CHECK(r.readU16Le() == 65535);
    CHECK(r.readU32Le() == 123456789u);
    CHECK(r.readU64Le() == 0xFFFFFFFFFFFFFFFFULL);
    CHECK(r.readI16Le() == -32768);
    CHECK(r.readI32Le() == -1);
    CHECK(r.readI64Le() == std::numeric_limits<int64_t>::min());
    CHECK(r.readF32Le() == -3.5f);
    CHECK(r.done());
}

TEST_CASE("Reader error is sticky and reads past the end return 0")
{
    const std::array<uint8_t, 3> buf = {0x01, 0x02, 0x03};
    Reader r(buf.data(), buf.size());
    CHECK(r.readU16Le() == 0x0201);
    CHECK(r.ok());
    CHECK(r.readU16Le() == 0); // only one byte left
    CHECK_FALSE(r.ok());
    CHECK(r.readU8() == 0); // would fit, but the error is sticky
    CHECK_FALSE(r.ok());
    CHECK_FALSE(r.done());
    CHECK(r.offset() == 2);
}

TEST_CASE("Reader::done() requires the whole buffer to be consumed")
{
    const std::array<uint8_t, 3> buf = {1, 2, 3};
    Reader r(buf.data(), buf.size());
    r.readU16Le();
    CHECK(r.ok());
    CHECK_FALSE(r.done());
    CHECK(r.remaining() == 1);
    r.skip(1);
    CHECK(r.done());
}

TEST_CASE("Reader with a null buffer reads nothing")
{
    Reader r(nullptr, 10);
    CHECK(r.remaining() == 0);
    CHECK(r.readU8() == 0);
    CHECK_FALSE(r.ok());
}

TEST_CASE("Writer error is sticky and nothing is written past capacity")
{
    std::array<uint8_t, 4> buf = {0xAA, 0xAA, 0xAA, 0xAA};
    Writer w(buf.data(), 3);
    w.appendU16Le(0x0102);
    w.appendU16Le(0x0304); // does not fit
    CHECK_FALSE(w.ok());
    w.appendU8(0x05); // would fit, but the error is sticky
    CHECK(w.size() == 2);
    CHECK(buf[2] == 0xAA);
    CHECK(buf[3] == 0xAA);
}

TEST_CASE("Writer::appendBytes and patchU8")
{
    std::array<uint8_t, 8> buf{};
    Writer w(buf.data(), buf.size());
    w.appendU8(0);
    const uint8_t body[] = {9, 8, 7};
    w.appendBytes(body, sizeof(body));
    w.appendBytes(nullptr, 0); // empty is fine
    REQUIRE(w.ok());
    w.patchU8(0, 3);
    REQUIRE(w.ok());
    CHECK(buf[0] == 3);
    CHECK(buf[1] == 9);
    CHECK(w.size() == 4);

    SUBCASE("patching past size() fails")
    {
        w.patchU8(4, 1);
        CHECK_FALSE(w.ok());
    }
    SUBCASE("appendBytes with null data fails")
    {
        w.appendBytes(nullptr, 1);
        CHECK_FALSE(w.ok());
    }
    SUBCASE("appendBytes that does not fit writes nothing")
    {
        const uint8_t big[8] = {};
        w.appendBytes(big, sizeof(big));
        CHECK_FALSE(w.ok());
        CHECK(w.size() == 4);
    }
}
