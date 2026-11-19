// Routing policy, the virtual subscription-group map and LIST synthesis.
#include "groups.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include <doctest.h>

using namespace drivelink::agent;
namespace defs = drivelink::protocol::defs;
namespace msg = drivelink::protocol::defs::msg;

namespace
{
constexpr uint8_t kSystem = 1, kGroup = 5, kParam = 4;
std::vector<uint8_t> listBody(uint16_t total, uint16_t next, std::vector<GroupEntry> entries)
{
    std::vector<uint8_t> b{static_cast<uint8_t>(total), static_cast<uint8_t>(total >> 8),
                           static_cast<uint8_t>(next), static_cast<uint8_t>(next >> 8), static_cast<uint8_t>(entries.size())};
    for (const auto &e : entries)
    {
        b.insert(b.end(), {e.kind, e.group, e.flags, e.loop, e.epoch, static_cast<uint8_t>(e.divider),
                           static_cast<uint8_t>(e.divider >> 8), static_cast<uint8_t>(e.refs.size())});
        for (const auto &r : e.refs) { b.insert(b.end(), r.begin(), r.end()); }
    }
    return b;
}
GroupEntry configured(uint8_t group, uint8_t epoch, size_t refs = 1)
{
    GroupEntry e;
    e.kind = 0; e.group = group; e.flags = 3; e.loop = 0; e.epoch = epoch; e.divider = 10;
    for (size_t i = 0; i < refs; ++i) { e.refs.push_back({0x70, 0, static_cast<uint8_t>(i), 1}); }
    return e;
}
} // namespace

TEST_CASE("classify: the interim rule, DISARM paths and virtualized group ops")
{
    CHECK(classify(msg::kCoreCommand, kSystem, 2) == Route::RefuseLease);  // ARM
    CHECK(classify(msg::kCoreCommand, kSystem, 3) == Route::Disarm);       // system DISARM
    CHECK(classify(msg::kDisarm, kSystem, 3) == Route::Disarm);            // fast path
    CHECK(classify(msg::kDisarm, kSystem, 2) == Route::Malformed);
    CHECK(classify(msg::kDisarm, kGroup, 3) == Route::Malformed);
    CHECK(classify(msg::kCoreCommand, kSystem, 1) == Route::Forward);      // GET_INFO
    CHECK(classify(msg::kCoreCommand, kSystem, 5) == Route::Forward);      // SAVE (exclusive in H3a-2)
    CHECK(classify(msg::kCoreCommand, kGroup, 4) == Route::RefuseLease);   // CMD_CONFIG
    CHECK(classify(msg::kCoreCommand, kGroup, 5) == Route::RefuseLease);   // CMD_CLEAR
    for (uint8_t op : {1, 2, 3, 6}) { CHECK(classify(msg::kCoreCommand, kGroup, op) == Route::Group); }
    CHECK(classify(msg::kCoreCommand, kGroup, 99) == Route::Forward);
    CHECK(classify(msg::kCoreCommand, kParam, 2) == Route::Forward);
    CHECK(classify(msg::kDiagQuery) == Route::Diag);
    CHECK(classify(msg::kAgentCommand) == Route::Agent);
    CHECK(classify(msg::kTimeRequest) == Route::Drop);
    for (unsigned ch = defs::kCmdGroupChannelFirst; ch < defs::kCmdGroupChannelFirst + defs::kCmdGroupChannelCount; ++ch)
    {
        CHECK(classify(static_cast<uint8_t>(ch)) == Route::Drop);
    }
    CHECK(classify(msg::kCoreResponse) == Route::Drop);
    CHECK(classify(msg::kStatusStream) == Route::Drop);
    CHECK(classify(msg::kAgentResponse) == Route::Drop);
}

TEST_CASE("parseListPage: exact entries")
{
    const auto body = listBody(16, kListEnd, {configured(0, 4, 2), GroupEntry{}});
    auto page = parseListPage(body);
    REQUIRE(page);
    CHECK(page->total == 16);
    CHECK(page->next == kListEnd);
    REQUIRE(page->entries.size() == 2);
    CHECK(page->entries[0].refs.size() == 2);
    CHECK(page->entries[0].epoch == 4);
    CHECK(page->entries[1].loop == 0xFF);
    for (size_t n = 0; n < body.size(); ++n)
    {
        CHECK_FALSE(parseListPage(std::span(body).first(n)));
    }
    auto longer = body;
    longer.push_back(0);
    CHECK_FALSE(parseListPage(longer));
    auto bad_kind = listBody(1, kListEnd, {GroupEntry{}});
    bad_kind[5] = 2;
    CHECK_FALSE(parseListPage(bad_kind));
}

TEST_CASE("GroupMap: each client gets its own slot for the same id")
{
    GroupMap map;
    map.reset(3, 8);
    CHECK(map.subs() == 3);
    CHECK(map.cmds() == 8);
    auto a = map.acquire(1, 0);
    auto b = map.acquire(2, 0);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a == 0);
    CHECK(*b == 1);
    map.finish(*a, map.slot(*a).serial, true, 7, false, false);
    map.finish(*b, map.slot(*b).serial, true, 9, false, false);
    CHECK(map.find(1, 0) == a);
    CHECK(map.find(2, 0) == b);
    // Streams go to the owner only with its current epoch.
    CHECK(map.route(*a, 7) == std::pair<ClientId, uint8_t>{1, 0});
    CHECK_FALSE(map.route(*a, 6));
    CHECK(map.route(*b, 9) == std::pair<ClientId, uint8_t>{2, 0});
    CHECK_FALSE(map.route(2, 0));
    CHECK_FALSE(map.route(200, 0));
    // A second CONFIG reuses the slot; its epoch replaces the old one.
    CHECK(map.acquire(1, 0) == a);
    map.finish(*a, map.slot(*a).serial, true, 8, false, false);
    CHECK_FALSE(map.route(*a, 7));
    CHECK(map.route(*a, 8));
    CHECK(map.lastEpoch(1, 0) == 8);
}

TEST_CASE("GroupMap: exhaustion, refused and uncertain mutations")
{
    GroupMap map;
    map.reset(2, 0);
    auto a = map.acquire(1, 0);
    auto b = map.acquire(1, 1);
    REQUIRE(a);
    REQUIRE(b);
    CHECK_FALSE(map.acquire(2, 0)); // pool exhausted
    // A refused CONFIG on a new slot gives it back.
    map.finish(*b, map.slot(*b).serial, false, 0, false, false);
    CHECK_FALSE(map.find(1, 1));
    auto c = map.acquire(2, 0);
    REQUIRE(c);
    CHECK(*c == *b);
    // An unanswered CONFIG keeps the slot: it may be configured on the device.
    map.finish(*a, map.slot(*a).serial, false, 0, false, true);
    CHECK(map.find(1, 0) == a);
    CHECK(map.slot(*a).maybe);
    CHECK_FALSE(map.route(*a, 0));
    // With two in flight, a refusal of the first keeps it for the second.
    map.finish(*c, map.slot(*c).serial, false, 0, false, false);
    auto d = map.acquire(3, 5);
    REQUIRE(d);
    map.begin(*d);
    map.finish(*d, map.slot(*d).serial, false, 0, false, false);
    CHECK(map.find(3, 5) == d);
    map.finish(*d, map.slot(*d).serial, true, 2, false, false);
    CHECK(map.route(*d, 2));
}

TEST_CASE("GroupMap: CLEAR frees a slot and records the epoch")
{
    GroupMap map;
    map.reset(1, 0);
    auto a = map.acquire(1, 4);
    REQUIRE(a);
    map.finish(*a, map.slot(*a).serial, true, 3, false, false);
    map.begin(*a);
    map.finish(*a, map.slot(*a).serial, true, 4, true, false); // accepted CLEAR
    CHECK_FALSE(map.find(1, 4));
    CHECK(map.lastEpoch(1, 4) == 4);
    CHECK_FALSE(map.route(*a, 4));
    // A refused CLEAR leaves it configured.
    a = map.acquire(1, 4);
    CHECK(map.slot(*a).epoch == 4);
    map.finish(*a, map.slot(*a).serial, true, 5, false, false);
    map.begin(*a);
    map.finish(*a, map.slot(*a).serial, false, 0, true, false);
    CHECK(map.route(*a, 5));
}

TEST_CASE("GroupMap: a leaving client's slots stay Dirty until cleared")
{
    GroupMap map;
    map.reset(3, 0);
    auto a = map.acquire(1, 0);
    auto b = map.acquire(1, 1);
    auto c = map.acquire(2, 0);
    map.finish(*a, map.slot(*a).serial, true, 1, false, false);
    map.finish(*b, map.slot(*b).serial, false, 0, false, true); // uncertain
    map.finish(*c, map.slot(*c).serial, true, 1, false, false);
    auto pending = map.acquire(1, 2);
    CHECK_FALSE(pending); // all three in use
    auto dirty = map.release(1);
    CHECK(dirty == std::vector<uint8_t>{*a, *b});
    CHECK(map.dirty() == dirty);
    CHECK_FALSE(map.route(*a, 1));
    CHECK(map.route(*c, 1)); // the other client is untouched
    CHECK_FALSE(map.acquire(3, 0)); // dirty slots are not handed out
    CHECK(map.lastEpoch(1, 0) == 0); // records of the client are gone
    // A late answer for a dirty slot changes nothing.
    map.finish(*a, map.slot(*a).serial, true, 9, false, false);
    CHECK(map.slot(*a).state == GroupMap::State::Dirty);
    map.cleaned(*a);
    CHECK(map.dirty() == std::vector<uint8_t>{*b});
    CHECK(map.acquire(3, 0) == a);
    // A client with only an unconfigured, idle slot leaves nothing dirty.
    map.reset(1, 0);
    auto idle = map.acquire(4, 0);
    map.finish(*idle, map.slot(*idle).serial, false, 0, false, false);
    CHECK(map.release(4).empty());
    map.reset(1, 0);
    idle = map.acquire(4, 0); // still in flight
    CHECK(map.release(4) == std::vector<uint8_t>{0});
}

TEST_CASE("GroupMap: an answer for an earlier allocation is ignored")
{
    GroupMap map;
    map.reset(1, 0);
    auto a = map.acquire(1, 0);
    const auto old = map.slot(*a).serial;
    map.finish(*a, old, true, 2, false, false);
    map.begin(*a); // a CONFIG still unanswered when client 1 leaves
    CHECK(map.release(1) == std::vector<uint8_t>{*a});
    map.cleaned(*a);
    auto b = map.acquire(2, 0);
    REQUIRE(b == a);
    CHECK(map.slot(*b).serial != old);
    map.finish(*a, old, false, 0, false, true); // client 1's late timeout
    CHECK(map.slot(*b).inflight == 1);
    CHECK_FALSE(map.slot(*b).maybe);
    map.finish(*b, map.slot(*b).serial, false, 0, false, false); // refused: freed
    CHECK_FALSE(map.find(2, 0));
}

TEST_CASE("GroupMap: accepted CLEAR with another request in flight, and markDirty")
{
    GroupMap map;
    map.reset(2, 0);
    auto a = map.acquire(1, 0);
    const auto serial = map.slot(*a).serial;
    map.finish(*a, serial, true, 1, false, false);
    map.begin(*a);
    map.begin(*a);
    map.finish(*a, serial, true, 2, true, false); // CLEAR accepted, one more in flight
    CHECK(map.find(1, 0) == a);
    CHECK_FALSE(map.route(*a, 2));
    map.finish(*a, serial, true, 3, false, false); // the later CONFIG landed
    CHECK(map.route(*a, 3));
    map.markDirty(1);
    CHECK(map.dirty() == std::vector<uint8_t>{1});
    CHECK_FALSE(map.acquire(2, 0));
    map.markDirty(*a); // owned: unchanged
    CHECK(map.route(*a, 3));
    map.cleaned(1);
    CHECK(map.acquire(2, 0) == std::optional<uint8_t>{1});
}

TEST_CASE("synthesizeList: the client's own view, paged")
{
    GroupMap map;
    map.reset(4, 2);
    auto a = map.acquire(1, 2); // client 1 id 2 -> device slot 0
    auto b = map.acquire(2, 0); // client 2 id 0 -> device slot 1
    map.finish(*a, map.slot(*a).serial, true, 6, false, false);
    map.finish(*b, map.slot(*b).serial, true, 3, false, false);
    map.setLastEpoch(1, 3, 9);
    std::vector<GroupEntry> device{configured(0, 6, 2), configured(1, 3), GroupEntry{0, 2, 0, 0xFF, 0, 1, {}},
                                   GroupEntry{0, 3, 0, 0xFF, 0, 1, {}}, GroupEntry{1, 0, 0, 0xFF, 0, 1, {}},
                                   GroupEntry{1, 1, 0, 0xFF, 0, 1, {}}};
    auto all = synthesizeList(map, 1, device, 0, 1018);
    REQUIRE(all);
    REQUIRE_FALSE(all->too_small);
    auto page = parseListPage(all->body);
    REQUIRE(page);
    CHECK(page->total == 6);
    CHECK(page->next == kListEnd);
    REQUIRE(page->entries.size() == 6);
    for (size_t i = 0; i < 6; ++i)
    {
        CHECK(page->entries[i].kind == (i < 4 ? 0 : 1));
        CHECK(page->entries[i].group == (i < 4 ? i : i - 4));
    }
    CHECK(page->entries[2].flags == 3); // its own group, rewritten to id 2
    CHECK(page->entries[2].epoch == 6);
    CHECK(page->entries[2].refs.size() == 2);
    CHECK(page->entries[0].flags == 0); // client 2's slot is hidden
    CHECK(page->entries[0].loop == 0xFF);
    CHECK(page->entries[3].epoch == 9);  // last epoch it saw
    CHECK(page->entries[3].refs.empty());
    // Paging: whole entries within the budget, resumed from next.
    auto first = synthesizeList(map, 1, device, 0, 5 + 8 + 8 + 15);
    REQUIRE(first);
    auto p1 = parseListPage(first->body);
    REQUIRE(p1);
    CHECK(p1->entries.size() == 2);
    CHECK(p1->next == 2);
    auto second = synthesizeList(map, 1, device, 2, 5 + 15);
    REQUIRE(second);
    CHECK(second->too_small); // id 2 has two refs (16 bytes) and does not fit
    auto tail = synthesizeList(map, 1, device, 6, 100);
    REQUIRE(tail);
    auto p3 = parseListPage(tail->body);
    REQUIRE(p3);
    CHECK(p3->entries.empty());
    CHECK(p3->next == kListEnd);
    CHECK_FALSE(synthesizeList(map, 1, device, 7, 100));
}
