#include "doctest.h"

#include "drivelink/protocol/crc.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/scanner.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace drivelink::protocol;

namespace {

std::vector<uint8_t> makeFrame(uint8_t type, const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> out(frameSize(payload.size()));
    const size_t n = encodeFrame(out.data(), out.size(), type, payload.data(), payload.size());
    REQUIRE(n == out.size());
    return out;
}

/// A frame header (preamble, MSG_TYPE, LEN, HCRC) with no payload behind it.
std::vector<uint8_t> makeHeader(uint8_t type, size_t len, bool good_hcrc = true)
{
    std::vector<uint8_t> out = {0xAA, 0xAA, 0xAA, type, static_cast<uint8_t>(len & 0xFFU),
                                static_cast<uint8_t>(len >> 8U)};
    const uint8_t hcrc = crc8(&out[3], 3);
    out.push_back(good_hcrc ? hcrc : static_cast<uint8_t>(hcrc ^ 0x5AU));
    return out;
}

/// Push as many bytes as fit into the scanner; returns how many were taken.
size_t feed(Scanner &s, const std::vector<uint8_t> &bytes, size_t from = 0)
{
    size_t space = 0;
    uint8_t *dst = s.writable(space);
    const size_t n = std::min(space, bytes.size() - from);
    std::memcpy(dst, bytes.data() + from, n);
    s.commit(n);
    return n;
}

struct Got {
    uint8_t type;
    std::vector<uint8_t> payload;
};

/// Feed all bytes in chunks of `chunk` and collect every frame.
std::vector<Got> run(Scanner &s, const std::vector<uint8_t> &bytes, size_t chunk)
{
    std::vector<Got> got;
    size_t pos = 0;
    while (pos < bytes.size()) {
        const std::vector<uint8_t> part(bytes.begin() + static_cast<long>(pos),
                                        bytes.begin() + static_cast<long>(std::min(bytes.size(), pos + chunk)));
        const size_t taken = feed(s, part);
        REQUIRE(taken > 0); // a full window after next() returned false means the scan is stuck
        pos += taken;
        Scanner::Frame f;
        while (s.next(f)) {
            got.push_back({f.msg_type, std::vector<uint8_t>(f.payload, f.payload + f.len)});
        }
    }
    return got;
}

std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> out;
    for (const auto &p : parts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

} // namespace

TEST_CASE("Scanner finds a frame fed one byte at a time")
{
    Scanner s(32);
    const auto bytes = makeFrame(0x41, {1, 2, 3});
    const auto got = run(s, bytes, 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == 0x41);
    CHECK(got[0].payload == std::vector<uint8_t>{1, 2, 3});
    CHECK(s.counters().discarded_bytes == 0);
}

TEST_CASE("Scanner returns several frames from one chunk, in order")
{
    Scanner s(32);
    const auto bytes = concat({makeFrame(0x41, {1}), makeFrame(0x42, {}), makeFrame(0x80, {9, 9})});
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(got.size() == 3);
    CHECK(got[0].type == 0x41);
    CHECK(got[1].type == 0x42);
    CHECK(got[1].payload.empty());
    CHECK(got[2].type == 0x80);
    CHECK(got[2].payload == std::vector<uint8_t>{9, 9});
}

TEST_CASE("Scanner skips garbage before a frame and counts it")
{
    Scanner s(32);
    const auto bytes = concat({{0x00, 0x13, 0x37, 0xAA, 0xAA}, makeFrame(0x41, {5})});
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].payload == std::vector<uint8_t>{5});
    CHECK(s.counters().discarded_bytes == 5);
    CHECK(s.counters().crc_errors == 0);
}

TEST_CASE("A false preamble inside a payload does not hide the real frame")
{
    Scanner s(32);
    // The first frame's payload holds `AA AA AA 41 02 ...`, which looks like the
    // start of a frame. The scanner must still deliver the frame that carries it.
    const auto inner = makeFrame(0x41, {0x07, 0x08});
    const auto outer = makeFrame(0x42, inner);
    const auto bytes = concat({outer, makeFrame(0x43, {0x01})});
    const auto got = run(s, bytes, bytes.size());
    // The outer frame is valid, so it is delivered whole. The frame inside its
    // payload is not delivered on its own.
    REQUIRE(got.size() == 2);
    CHECK(got[0].type == 0x42);
    CHECK(got[0].payload == inner);
    CHECK(got[1].type == 0x43);
}

TEST_CASE("A bad CRC drops one byte, and the following frame is found")
{
    Scanner s(32);
    auto bad = makeFrame(0x41, {1, 2, 3});
    bad[8] ^= 0xFFU; // a payload byte: the header is valid, the CRC16 is not
    const auto bytes = concat({bad, makeFrame(0x42, {4})});
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == 0x42);
    CHECK(s.counters().crc_errors == 1);
    CHECK(s.counters().header_crc_errors == 0);
    CHECK(s.counters().discarded_bytes == bad.size());
}

TEST_CASE("A frame hidden inside a corrupt candidate is still found")
{
    Scanner s(32);
    // A valid header claiming a 20-byte payload, but a real frame starts
    // right after it. The scanner waits for the 20 bytes, fails the CRC16,
    // and then finds the real frame inside.
    std::vector<uint8_t> bytes = makeHeader(0x41, 20);
    const auto real = makeFrame(0x42, {1, 2});
    bytes.insert(bytes.end(), real.begin(), real.end());
    bytes.resize(bytes.size() + 20, 0x00);
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == 0x42);
}

TEST_CASE("A valid header with LEN above the limit is a length error")
{
    Scanner s(8);
    std::vector<uint8_t> bytes = makeHeader(0x41, 9);
    bytes.push_back(0x00);
    const auto got = run(s, bytes, bytes.size());
    CHECK(got.empty());
    CHECK(s.counters().length_errors == 1);
    CHECK(s.counters().header_crc_errors == 0);
}

TEST_CASE("A bad HCRC is a header CRC error, even with LEN above the limit")
{
    Scanner s(8);
    std::vector<uint8_t> bytes = makeHeader(0x41, 900, false);
    bytes.push_back(0x00);
    const auto got = run(s, bytes, bytes.size());
    CHECK(got.empty());
    CHECK(s.counters().header_crc_errors == 1);
    CHECK(s.counters().length_errors == 0);
}

TEST_CASE("A false preamble with a bad HCRC does not hold up the frame behind it")
{
    // The false header announces 1000 bytes. Without the HCRC the scanner would
    // wait for them; with it, the real frame right behind is found at once,
    // without any of the 1000 bytes ever arriving.
    Scanner s(1024);
    const auto bytes = concat({makeHeader(0x41, 1000, false), makeFrame(0x42, {9, 8})});
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == 0x42);
    CHECK(got[0].payload == std::vector<uint8_t>{9, 8});
    CHECK(s.counters().header_crc_errors == 1);
    CHECK(s.counters().crc_errors == 0);
    CHECK(s.buffered() == 0);
}

TEST_CASE("Every corrupted header byte is caught before the payload is awaited")
{
    for (size_t i = 3; i < 7; ++i) {
        Scanner s(1024);
        auto bad = makeFrame(0x41, std::vector<uint8_t>(600, 0x11));
        bad[i] ^= 0x04U;
        // Feed only the header: a rejected header leaves nothing to wait for.
        const std::vector<uint8_t> header(bad.begin(), bad.begin() + 7);
        const auto got = run(s, concat({header, makeFrame(0x43, {1})}), 64);
        REQUIRE(got.size() == 1);
        CHECK(got[0].type == 0x43);
        CHECK(s.counters().header_crc_errors == 1);
    }
}

TEST_CASE("A long run of start bytes is resync and makes progress")
{
    Scanner s(64);
    const auto bytes = concat({std::vector<uint8_t>(2000, 0xAA), makeFrame(0x41, {3})});
    const auto got = run(s, bytes, 37);
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == 0x41);
    CHECK(got[0].payload == std::vector<uint8_t>{3});
    CHECK(s.counters().discarded_bytes == 2000);
    CHECK(s.counters().header_crc_errors == 0);
    CHECK(s.counters().length_errors == 0);
    CHECK(s.counters().crc_errors == 0);
}

TEST_CASE("A frame at exactly the limit is accepted")
{
    Scanner s(8);
    const auto bytes = makeFrame(0x41, std::vector<uint8_t>(8, 0x5A));
    const auto got = run(s, bytes, 3);
    REQUIRE(got.size() == 1);
    CHECK(got[0].payload.size() == 8);
}

TEST_CASE("The scanner limit is clamped to kMaxPayload")
{
    Scanner s(100000);
    CHECK(s.maxPayload() == kMaxPayload);
    const auto bytes = makeFrame(0x80, std::vector<uint8_t>(kMaxPayload, 0x11));
    const auto got = run(s, bytes, 7);
    REQUIRE(got.size() == 1);
    CHECK(got[0].payload.size() == kMaxPayload);
}

TEST_CASE("A run of start bytes before a preamble is resync, not an error")
{
    Scanner s(32);
    const auto bytes = concat({{0xAA, 0xAA, 0xAA, 0xAA, 0xAA}, makeFrame(0x41, {7})});
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].payload == std::vector<uint8_t>{7});
    CHECK(s.counters().length_errors == 0);
    CHECK(s.counters().crc_errors == 0);
}

TEST_CASE("An ambiguous candidate that fails its CRC is resync, not a CRC error")
{
    // A stray start byte before a real frame makes the first candidate
    // `AA AA AA | AA 41 01 00 ...` (MSG_TYPE 0xAA). It fails its HCRC (or, by
    // chance, its LEN or CRC16), which is resync and must not be counted.
    Scanner s(128);
    const auto bytes = concat({{0xAA}, makeFrame(0x41, {7}), std::vector<uint8_t>(80, 0x00)});
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == 0x41);
    CHECK(got[0].payload == std::vector<uint8_t>{7});
    CHECK(s.counters().crc_errors == 0);
    CHECK(s.counters().header_crc_errors == 0);
    CHECK(s.counters().length_errors == 0);
}

TEST_CASE("A frame whose MSG_TYPE is 0xAA is delivered")
{
    Scanner s(32);
    const auto bytes = makeFrame(0xAA, {1, 2});
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == 0xAA);
}

TEST_CASE("A trailing partial preamble is kept for the next chunk")
{
    Scanner s(32);
    const auto frame = makeFrame(0x41, {1});
    // Noise, then the first two preamble bytes of the frame.
    std::vector<uint8_t> first = {0x01, 0x02, 0xAA, 0xAA};
    CHECK(run(s, first, first.size()).empty());
    CHECK(s.buffered() == 2);
    std::vector<uint8_t> rest(frame.begin() + 2, frame.end());
    const auto got = run(s, rest, rest.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].payload == std::vector<uint8_t>{1});
}

TEST_CASE("dropStale drops one byte of a candidate and counts a timeout")
{
    Scanner s(32);
    const auto frame = makeFrame(0x41, {1, 2, 3, 4});
    const std::vector<uint8_t> part(frame.begin(), frame.begin() + 6);
    CHECK(run(s, part, part.size()).empty());
    CHECK(s.buffered() == 6);

    CHECK(s.dropStale());
    CHECK(s.counters().timeouts == 1);
    CHECK(s.buffered() == 5);
}

TEST_CASE("dropStale of a lone partial preamble drops it all, without a timeout")
{
    Scanner s(32);
    const std::vector<uint8_t> part = {0xAA, 0xAA};
    CHECK(run(s, part, part.size()).empty());
    CHECK(s.buffered() == 2);
    CHECK(s.dropStale());
    CHECK(s.buffered() == 0);
    CHECK(s.counters().timeouts == 0);
    CHECK_FALSE(s.dropStale());
}

TEST_CASE("A real frame is found after a stale partial is dropped")
{
    Scanner s(32);
    const auto broken = makeFrame(0x41, {1, 2, 3, 4});
    const std::vector<uint8_t> part(broken.begin(), broken.begin() + 7);
    run(s, part, part.size());
    while (s.dropStale()) {
    }
    const auto good = makeFrame(0x42, {9});
    const auto got = run(s, good, good.size());
    REQUIRE(got.size() == 1);
    CHECK(got[0].type == 0x42);
}

TEST_CASE("Frame data stays valid until the next call")
{
    Scanner s(32);
    const auto bytes = concat({makeFrame(0x41, {1, 2, 3}), makeFrame(0x42, {4, 5})});
    feed(s, bytes);
    Scanner::Frame f;
    REQUIRE(s.next(f));
    CHECK(f.len == 3);
    CHECK(f.payload[0] == 1);
    CHECK(f.payload[2] == 3);
    REQUIRE(s.next(f));
    CHECK(f.msg_type == 0x42);
    CHECK(f.payload[1] == 5);
}

TEST_CASE("The deferred consume happens before the window is refilled, as uart_comm does")
{
    Scanner s(32);
    const auto first = makeFrame(0x41, {1, 2, 3});
    const auto second = makeFrame(0x42, {4, 5});
    feed(s, first);

    Scanner::Frame f;
    REQUIRE(s.next(f));
    CHECK(f.msg_type == 0x41);
    CHECK(s.buffered() == 0);

    // The caller returns after one frame, then refills: writable() must have
    // consumed the first frame, or the second would land after it.
    feed(s, second);
    CHECK(s.buffered() == second.size());
    REQUIRE(s.next(f));
    CHECK(f.msg_type == 0x42);
    CHECK(f.len == 2);
    CHECK(f.payload[0] == 4);
    CHECK(f.payload[1] == 5);
    CHECK_FALSE(s.next(f));
}

TEST_CASE("dropStale after a returned frame consumes that frame first")
{
    Scanner s(32);
    const auto frame = makeFrame(0x41, {1});
    const std::vector<uint8_t> partial = {0xAA, 0xAA};
    feed(s, concat({frame, partial}));

    Scanner::Frame f;
    REQUIRE(s.next(f));
    CHECK(s.buffered() == 2);   // the frame no longer counts
    CHECK(s.dropStale());       // drops the partial preamble, not part of the frame
    CHECK(s.buffered() == 0);
    CHECK(s.counters().timeouts == 0);
}

TEST_CASE("A full window of noise always makes progress")
{
    Scanner s(16);
    // Fill the window with start bytes and noise that never forms a valid frame.
    std::vector<uint8_t> noise(1000);
    for (size_t i = 0; i < noise.size(); ++i) {
        noise[i] = (i % 5U < 3U) ? 0xAAU : static_cast<uint8_t>(i);
    }
    const auto good = makeFrame(0x41, {1});
    const auto bytes = concat({noise, good});
    const auto got = run(s, bytes, bytes.size());
    REQUIRE(!got.empty());
    CHECK(got.back().type == 0x41);
    CHECK(got.back().payload == std::vector<uint8_t>{1});
}

TEST_CASE("Back-to-back frames fed in odd chunk sizes all arrive")
{
    // Scanner(40) has a 98-byte window: the large chunks fill it, so the
    // unconsumed tail is moved to the front on every refill.
    for (size_t chunk : {1U, 2U, 3U, 5U, 7U, 13U, 50U, 97U, 98U}) {
        Scanner s(40);
        std::vector<uint8_t> bytes;
        for (uint8_t i = 0; i < 20; ++i) {
            const auto f = makeFrame(static_cast<uint8_t>(0x41 + (i % 3U)), std::vector<uint8_t>(i, i));
            bytes.insert(bytes.end(), f.begin(), f.end());
        }
        const auto got = run(s, bytes, chunk);
        REQUIRE(got.size() == 20);
        for (uint8_t i = 0; i < 20; ++i) {
            CHECK(got[i].payload == std::vector<uint8_t>(i, i));
        }
    }
}
