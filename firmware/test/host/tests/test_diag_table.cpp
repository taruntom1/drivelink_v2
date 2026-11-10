#include "doctest.h"

#include "diag_codes.hpp"
#include "diag_table.hpp"

#include <array>
#include <set>
#include <utility>

using namespace diag;
using namespace diag::detail;
using diag::protocol::kNoNextPage;
using diag::protocol::kStateActive;
using diag::protocol::kStateOccurred;

namespace {

constexpr Code record(uint8_t slot, uint16_t code, bool clears = false)
{
    return Code{slot, Source::System, code, Severity::Error, CodeKind::Record, clears};
}

constexpr Code counter(uint8_t slot, uint16_t code)
{
    return Code{slot, Source::UartComm, code, Severity::Warning, CodeKind::Counter, false};
}

} // namespace

TEST_CASE("generated diag code table is consistent")
{
    std::set<std::pair<uint8_t, uint16_t>> seen;
    for (size_t i = 0; i < kCodeTable.size(); ++i) {
        const Code &c = kCodeTable[i];
        CAPTURE(i);
        CHECK(c.slot == i);
        CHECK(c.code != 0);
        CHECK(seen.insert({static_cast<uint8_t>(c.source), c.code}).second);
        if (c.kind == CodeKind::Counter) {
            CHECK_FALSE(c.clears);
        }
    }
    CHECK(kSlotCount < kNoNextPage);

    // The codes the firmware itself relies on.
    CHECK(codes::diag::kRecordDropped.kind == CodeKind::Counter);
    CHECK(codes::diag::kEventTxDropped.kind == CodeKind::Counter);
    CHECK(codes::diag::kQueryDropped.kind == CodeKind::Counter);
    CHECK(codes::system::kManagerStartFailed.kind == CodeKind::Record);
    CHECK(codes::system::kManagerStartFailed.severity == Severity::Error);
}

TEST_CASE("slot table: counts and state bits")
{
    BasicSlotTable<3> t;

    CHECK(t.read(0).count == 0);
    CHECK(t.read(0).state == 0);

    t.add(0, 0); // no-op
    CHECK(t.read(0).state == 0);

    t.add(0, 5);
    t.add(0, 2);
    CHECK(t.read(0).count == 7);
    CHECK(t.read(0).state == kStateOccurred);

    t.raise(1, false);
    CHECK(t.read(1).count == 1);
    CHECK(t.read(1).state == kStateOccurred);
    CHECK_FALSE(t.clearActive(1));

    t.raise(2, true);
    t.raise(2, true);
    CHECK(t.read(2).count == 2);
    CHECK(t.read(2).state == (kStateOccurred | kStateActive));
    CHECK(t.clearActive(2));
    CHECK_FALSE(t.clearActive(2));
    CHECK(t.read(2).state == kStateOccurred);

    // Clear occurred keeps active; reset counts keeps state.
    t.raise(2, true);
    t.clearOccurred();
    CHECK(t.read(2).state == kStateActive);
    CHECK(t.read(0).state == 0);
    t.resetCounts();
    CHECK(t.read(0).count == 0);
    CHECK(t.read(2).count == 0);
    CHECK(t.read(2).state == kStateActive);

    // Out of range is ignored.
    t.add(3, 1);
    t.raise(99, true);
    CHECK_FALSE(t.clearActive(3));
    CHECK(t.read(3).count == 0);

    // Counts wrap.
    t.add(0, 0xFFFFFFFFU);
    t.add(0, 2);
    CHECK(t.read(0).count == 1);
}

TEST_CASE("collectEntries pages through listed slots")
{
    const std::array<Code, 6> codes = {
        record(0, 1), counter(1, 2), record(2, 3, true), counter(3, 4), record(4, 5), counter(5, 6),
    };
    std::array<SlotSnapshot, 6> snaps{};
    snaps[1] = {3, kStateOccurred};
    snaps[2] = {0, kStateActive}; // listed: active without a count (counts were reset)
    snaps[4] = {1, kStateOccurred};
    snaps[5] = {9, 0};            // listed: count without state (occurred was cleared)

    std::array<protocol::EntryState, 2> out{};
    uint8_t total = 0;
    uint8_t next = 0;

    size_t n = collectEntries(codes.data(), snaps.data(), codes.size(), 0, out.data(), out.size(), total, next);
    CHECK(n == 2);
    CHECK(total == 4);
    CHECK(next == 4);
    CHECK(out[0].code == 2);
    CHECK(out[0].count == 3);
    CHECK(out[0].source == static_cast<uint8_t>(Source::UartComm));
    CHECK(out[1].code == 3);
    CHECK(out[1].state == kStateActive);

    n = collectEntries(codes.data(), snaps.data(), codes.size(), next, out.data(), out.size(), total, next);
    CHECK(n == 2);
    CHECK(total == 4);
    CHECK(next == kNoNextPage);
    CHECK(out[0].code == 5);
    CHECK(out[1].code == 6);

    // Past the end, or nothing listed.
    n = collectEntries(codes.data(), snaps.data(), codes.size(), 200, out.data(), out.size(), total, next);
    CHECK(n == 0);
    CHECK(total == 4);
    CHECK(next == kNoNextPage);
    const std::array<SlotSnapshot, 6> empty{};
    n = collectEntries(codes.data(), empty.data(), codes.size(), 0, out.data(), out.size(), total, next);
    CHECK(n == 0);
    CHECK(total == 0);
    CHECK(next == kNoNextPage);

    // A page that exactly holds the rest has no next page.
    std::array<protocol::EntryState, 4> big{};
    n = collectEntries(codes.data(), snaps.data(), codes.size(), 0, big.data(), big.size(), total, next);
    CHECK(n == 4);
    CHECK(next == kNoNextPage);
}

TEST_CASE("collectCounterDeltas picks changed counters only")
{
    const std::array<Code, 5> codes = {counter(0, 1), record(1, 2), counter(2, 3), counter(3, 4), counter(4, 5)};
    const std::array<uint32_t, 5> now = {5, 7, 2, 4, 1};
    const std::array<uint32_t, 5> last = {5, 0, 0, 0xFFFFFFFE, 0};

    std::array<protocol::CounterEntry, 2> out{};
    std::array<uint8_t, 2> slots{};
    size_t cursor = 0;

    // Slot 0 unchanged, slot 1 is a record: skipped.
    size_t n = collectCounterDeltas(codes.data(), now.data(), last.data(), codes.size(), cursor, out.data(),
                                    slots.data(), out.size());
    REQUIRE(n == 2);
    CHECK(slots[0] == 2);
    CHECK(out[0].code == 3);
    CHECK(out[0].delta == 2);
    CHECK(out[0].total == 2);
    CHECK(slots[1] == 3);
    CHECK(out[1].delta == 6); // wrapped: 0xFFFFFFFE -> 4
    CHECK(out[1].total == 4);
    CHECK(cursor == 4);

    n = collectCounterDeltas(codes.data(), now.data(), last.data(), codes.size(), cursor, out.data(), slots.data(),
                             out.size());
    REQUIRE(n == 1);
    CHECK(slots[0] == 4);
    CHECK(out[0].delta == 1);
    CHECK(cursor == codes.size());

    n = collectCounterDeltas(codes.data(), now.data(), last.data(), codes.size(), cursor, out.data(), slots.data(),
                             out.size());
    CHECK(n == 0);
}
