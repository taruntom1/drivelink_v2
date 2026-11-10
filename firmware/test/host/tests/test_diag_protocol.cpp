#include "doctest.h"

#include "diag_protocol.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace diag::protocol;

namespace {

std::vector<uint8_t> bytes(const uint8_t *data, size_t len)
{
    return std::vector<uint8_t>(data, data + len);
}

} // namespace

TEST_CASE("diag page sizes")
{
    CHECK(maxCounterEntries(128) == 10);  // (128 - 14) / 11
    CHECK(maxEntriesPerPage(128) == 15);  // (128 - 7) / 8
    CHECK(maxCounterEntries(14) == 0);
    CHECK(maxEntriesPerPage(7) == 0);
    CHECK(maxCounterEntries(0) == 0);
    CHECK(maxCounterEntries(100000) == 255);
    CHECK(kMaxFixedEventSize == 29);
    CHECK(kMaxInfoResponseSize == 56);
}

TEST_CASE("diag Record event")
{
    RecordEvent ev{};
    ev.timestamp_us = 0x0102030405060708ULL;
    ev.severity = 1;
    ev.source = 0x10;
    ev.code = 0xA1B2;
    ev.esp_err = -1; // ESP_FAIL
    ev.arg0 = 0x11223344;
    ev.arg1 = 0xDEADBEEF;

    std::array<uint8_t, 64> buf{};
    const size_t len = encodeRecordEvent(0x1234, kFlagTimeSynced, ev, buf.data(), buf.size());
    REQUIRE(len == kEventHeaderSize + kRecordBodySize);
    const std::vector<uint8_t> expected = {
        kProtocolVersion, 0x34, 0x12, 0x01, 0x01,             // header: seq LE, kind Record, flags
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,       // timestamp
        0x01, 0x10, 0xB2, 0xA1,                               // severity, source, code
        0xFF, 0xFF, 0xFF, 0xFF,                               // esp_err -1
        0x44, 0x33, 0x22, 0x11,                               // arg0
        0xEF, 0xBE, 0xAD, 0xDE,                               // arg1
    };
    CHECK(bytes(buf.data(), len) == expected);

    CHECK(encodeRecordEvent(0, 0, ev, buf.data(), len - 1) == 0);
}

TEST_CASE("diag Cleared and Boot events")
{
    std::array<uint8_t, 64> buf{};

    ClearedEvent clr{};
    clr.timestamp_us = 5;
    clr.source = 0x12;
    clr.code = 0x0102;
    size_t len = encodeClearedEvent(7, 0, clr, buf.data(), buf.size());
    REQUIRE(len == kEventHeaderSize + kClearedBodySize);
    CHECK(buf[3] == static_cast<uint8_t>(EventKind::Cleared));
    CHECK(buf[13] == 0x12);
    CHECK(buf[14] == 0x02);
    CHECK(buf[15] == 0x01);

    BootEvent boot{};
    boot.timestamp_us = 1;
    boot.reset_reason = 9;
    boot.started_mask = 0x05;
    len = encodeBootEvent(0xFFFF, 0, boot, buf.data(), buf.size());
    REQUIRE(len == kEventHeaderSize + kBootBodySize);
    CHECK(buf[1] == 0xFF);
    CHECK(buf[2] == 0xFF);
    CHECK(buf[3] == static_cast<uint8_t>(EventKind::Boot));
    CHECK(buf[13] == 9);
    CHECK(buf[14] == 0x05);
}

TEST_CASE("diag CounterDelta event")
{
    std::array<uint8_t, 128> buf{};

    SUBCASE("no entries")
    {
        const size_t len = encodeCounterDeltaEvent(1, 0, 42, nullptr, 0, buf.data(), buf.size());
        REQUIRE(len == kEventHeaderSize + kCounterDeltaFixedSize);
        CHECK(buf[3] == static_cast<uint8_t>(EventKind::CounterDelta));
        CHECK(buf[13] == 0); // n
    }
    SUBCASE("two entries")
    {
        const std::array<CounterEntry, 2> entries = {{
            {0x02, 0x0001, 3, 10},
            {0x10, 0xBEEF, 0x01020304, 0xFFFFFFFF},
        }};
        const size_t len = encodeCounterDeltaEvent(2, 0, 0, entries.data(), entries.size(), buf.data(), buf.size());
        REQUIRE(len == kEventHeaderSize + kCounterDeltaFixedSize + 2 * kCounterEntrySize);
        CHECK(buf[13] == 2);
        const std::vector<uint8_t> second = {0x10, 0xEF, 0xBE, 0x04, 0x03, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(bytes(&buf[14 + kCounterEntrySize], kCounterEntrySize) == second);
    }
    SUBCASE("does not fit")
    {
        const std::array<CounterEntry, 1> entries = {{{1, 1, 1, 1}}};
        CHECK(encodeCounterDeltaEvent(0, 0, 0, entries.data(), 1, buf.data(), 24) == 0);
        CHECK(encodeCounterDeltaEvent(0, 0, 0, nullptr, 1, buf.data(), buf.size()) == 0);
        CHECK(encodeCounterDeltaEvent(0, 0, 0, entries.data(), 256, buf.data(), buf.size()) == 0);
    }
}

TEST_CASE("diag query header")
{
    QueryHeader hdr{};
    const uint8_t *body = nullptr;
    size_t body_len = 99;

    SUBCASE("valid with a body")
    {
        const std::vector<uint8_t> p = {kProtocolVersion, 0x21, 0x02, 0x05};
        REQUIRE(decodeQueryHeader(p.data(), p.size(), hdr, body, body_len));
        CHECK(hdr.seq == 0x21);
        CHECK(hdr.subcommand == 0x02);
        CHECK(body == &p[3]);
        CHECK(body_len == 1);
    }
    SUBCASE("short, version and seq present: seq echoed")
    {
        const std::vector<uint8_t> p = {kProtocolVersion, 0x33};
        CHECK_FALSE(decodeQueryHeader(p.data(), p.size(), hdr, body, body_len));
        CHECK(hdr.seq == 0x33);
        CHECK(hdr.subcommand == kUnparsedHeaderByte);
        CHECK(body == nullptr);
        CHECK(body_len == 0);
    }
    SUBCASE("wrong version: nothing echoed")
    {
        const std::vector<uint8_t> p = {0x7F, 0x33, 0x01};
        CHECK_FALSE(decodeQueryHeader(p.data(), p.size(), hdr, body, body_len));
        CHECK(hdr.seq == 0);
        CHECK(hdr.subcommand == kUnparsedHeaderByte);
    }
    SUBCASE("empty")
    {
        CHECK_FALSE(decodeQueryHeader(nullptr, 0, hdr, body, body_len));
        CHECK(hdr.subcommand == kUnparsedHeaderByte);
    }
}

TEST_CASE("diag query bodies need their exact length")
{
    const uint8_t one[] = {0x03};
    const uint8_t two[] = {0x00, 0x00};

    CHECK(decodeGetInfo(nullptr, 0));
    CHECK_FALSE(decodeGetInfo(one, 1));

    uint8_t start = 0xAA;
    CHECK(decodeGetEntries(one, 1, start));
    CHECK(start == 0x03);
    start = 0xAA;
    CHECK_FALSE(decodeGetEntries(two, 2, start));
    CHECK_FALSE(decodeGetEntries(nullptr, 0, start));
    CHECK(start == 0xAA);

    uint8_t what = 0xAA;
    CHECK(decodeClear(one, 1, what));
    CHECK(what == kClearAllBits);
    what = 0xAA;
    const uint8_t reserved[] = {0x04};
    CHECK_FALSE(decodeClear(reserved, 1, what));
    CHECK_FALSE(decodeClear(two, 2, what));
    CHECK(what == 0xAA);
}

TEST_CASE("diag GetInfo body")
{
    std::array<uint8_t, 128> buf{};
    InfoBody info{};
    info.flags = kFlagTimeSynced;
    info.uptime_us = 0x0A0B;
    info.reset_reason = 4;
    info.started_mask = 0x07;
    info.records_dropped = 2;
    info.events_tx_dropped = 3;

    SUBCASE("short version")
    {
        info.version = "v1.2";
        info.version_len = 4;
        const size_t len = encodeInfoBody(info, buf.data(), buf.size());
        REQUIRE(len == kInfoFixedSize + 4);
        CHECK(buf[0] == kFlagTimeSynced);
        CHECK(buf[1] == 0x0B);
        CHECK(buf[2] == 0x0A);
        CHECK(buf[9] == 4);
        CHECK(buf[10] == 0x07);
        CHECK(buf[11] == 2);
        CHECK(buf[15] == 3);
        CHECK(buf[19] == 4);
        CHECK(std::memcmp(&buf[20], "v1.2", 4) == 0);
    }
    SUBCASE("long version is truncated")
    {
        const char long_version[] = "0123456789012345678901234567890123456789";
        info.version = long_version;
        info.version_len = sizeof(long_version) - 1;
        const size_t len = encodeInfoBody(info, buf.data(), buf.size());
        REQUIRE(len == kInfoFixedSize + kMaxVersionLen);
        CHECK(buf[19] == kMaxVersionLen);
    }
    SUBCASE("no version")
    {
        CHECK(encodeInfoBody(info, buf.data(), buf.size()) == kInfoFixedSize);
        CHECK(buf[19] == 0);
    }
}

TEST_CASE("diag GetEntries body and response framing")
{
    std::array<uint8_t, 128> body{};
    const std::array<EntryState, 2> entries = {{
        {0x01, 0x0001, kStateOccurred, 1},
        {0x12, 0x0203, kStateOccurred | kStateActive, 0x01000000},
    }};
    const size_t body_len = encodeEntriesBody(5, 9, entries.data(), entries.size(), body.data(), body.size());
    REQUIRE(body_len == kEntriesFixedSize + 2 * kEntrySize);
    const std::vector<uint8_t> expected = {
        5, 9, 2,
        0x01, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
        0x12, 0x03, 0x02, 0x03, 0x00, 0x00, 0x00, 0x01,
    };
    CHECK(bytes(body.data(), body_len) == expected);

    std::array<uint8_t, 128> out{};
    QueryHeader hdr{};
    hdr.seq = 0x44;
    hdr.subcommand = static_cast<uint8_t>(QuerySubcommand::GetEntries);
    const size_t len = encodeQueryResponse(hdr, ResultCode::Ok, body.data(), body_len, out.data(), out.size());
    REQUIRE(len == kQueryResponseHeaderSize + body_len);
    CHECK(out[0] == kProtocolVersion);
    CHECK(out[1] == 0x44);
    CHECK(out[2] == 0x02);
    CHECK(out[3] == 0x00);
    CHECK(bytes(&out[4], body_len) == expected);

    // Empty body (Clear, or an error) and a too-small buffer.
    CHECK(encodeQueryResponse(QueryHeader{}, ResultCode::InvalidArgument, nullptr, 0, out.data(), out.size()) ==
          kQueryResponseHeaderSize);
    CHECK(out[2] == kUnparsedHeaderByte);
    CHECK(out[3] == 0x01);
    CHECK(encodeQueryResponse(hdr, ResultCode::Ok, body.data(), body_len, out.data(), 10) == 0);
}
