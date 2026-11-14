#include "doctest.h"
#include "core/capture_service.hpp"
#include "core/command_service.hpp"
#include "core_os/capture_memory.hpp"
#include "core_os/clock.hpp"
#include "core_os/native_task.hpp"
#include "core_os/realtime.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include <atomic>
#include <thread>
#include <vector>

using namespace testmod;
namespace {
namespace cw = drivelink::protocol::core_wire;
using core::CaptureState;
using Kind = core::CaptureTriggerKind;
using core::CaptureEdge;
const core::Ref kPos{fx::kTypeId, 0, fx::kPosition}, kRaw{fx::kTypeId, 0, fx::kRaw},
    kReady{fx::kTypeId, 0, fx::kReady}, kOther{fx::kTypeId, 1, fx::kPosition};

struct Capture {
    core_os::KvStore store;
    core::Core c{testConfig()};
    Capture() {
        reset();
        bootWith(c, store, {{fx::kTypeId, 0, 0, "a"}, {fx::kTypeId, 1, 1, "b"}});
    }
    core::CaptureService &cap() { return c.capture(); }
    FullModule &m() { return *world().created[0]; }
    void tick(unsigned n = 1) { for (unsigned i = 0; i < n; ++i) { c.loop(0).runTick(1); } }
    // Tick with the module's raw input set to `values[i]` before each tick.
    void feed(std::initializer_list<uint32_t> values) { for (auto v : values) { m().raw = v; tick(); } }
    std::vector<uint32_t> readAll() {
        const auto s = cap().snapshot();
        std::vector<uint32_t> out(static_cast<size_t>(s.recorded) * cap().info().count);
        REQUIRE(cap().read(0, s.recorded, out).ok());
        return out;
    }
};
core::CaptureTrigger manual() { return {}; }
core::CaptureTrigger threshold(core::Ref ref, CaptureEdge edge, uint32_t value) { return {Kind::Threshold, ref, edge, value}; }
core::CaptureTrigger change(core::Ref ref) { return {Kind::ValueChange, ref, CaptureEdge::Rising, 0}; }
std::vector<uint8_t> request(uint8_t op, const std::vector<uint8_t> &body = {}) {
    std::vector<uint8_t> bytes{cw::kVersion, 7, static_cast<uint8_t>(defs::Family::Capture), op};
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}
} // namespace

TEST_CASE("Capture CONFIG validates everything before changing the capture") {
    Capture f;
    auto &cap = f.cap();
    const std::array<core::Ref, 2> refs{kPos, kRaw};
    CHECK(cap.capacityBytes() == 4U * 1024U * 1024U);
    CHECK(cap.refCapacity() == 16);
    CHECK(cap.configure(9, 1, 10, 0, manual(), refs).detail == core::Detail::LoopMismatch);
    CHECK(cap.configure(0, 0, 10, 0, manual(), refs).detail == core::Detail::BadValue);
    CHECK(cap.configure(0, 1, 0, 0, manual(), refs).detail == core::Detail::BadValue);
    CHECK(cap.configure(0, 1, 10, 10, manual(), refs).detail == core::Detail::BadValue);
    CHECK(cap.configure(0, 1, 10, 0, manual(), {}).detail == core::Detail::BadValue);
    std::vector<core::Ref> many(17, kPos);
    CHECK(cap.configure(0, 1, 10, 0, manual(), many).detail == core::Detail::BatchTooLarge);
    const std::array<core::Ref, 2> dup{kPos, kPos};
    CHECK(cap.configure(0, 1, 10, 0, manual(), dup).detail == core::Detail::DuplicateRef);
    CHECK(cap.configure(0, 1, 10, 0, manual(), {&kOther, 1}).detail == core::Detail::LoopMismatch);
    const core::Ref param{fx::kTypeId, 0, fx::kGain};
    CHECK(cap.configure(0, 1, 10, 0, manual(), {&param, 1}).detail == core::Detail::WrongKind);
    CHECK(cap.configure(0, 1, 10, 0, {Kind::TestgenStart, kPos, CaptureEdge::Rising, 0}, refs).detail ==
          core::Detail::BadValue);
    CHECK(cap.configure(0, 1, 10, 0, {Kind::Manual, kPos, CaptureEdge::Rising, 0}, refs).detail ==
          core::Detail::BadValue);
    CHECK(cap.configure(0, 1, 10, 0, {Kind::ValueChange, kPos, CaptureEdge::Falling, 0}, refs).detail ==
          core::Detail::BadValue);
    CHECK(cap.configure(0, 1, 10, 0, threshold(kOther, CaptureEdge::Rising, 0), refs).detail ==
          core::Detail::LoopMismatch);
    CHECK(cap.configure(0, 1, 10, 0, threshold(kPos, CaptureEdge::Rising, 0x7FC00000U), refs).detail ==
          core::Detail::NonFiniteFloat);
    CHECK(cap.configure(0, 1, 10, 0, threshold(kReady, CaptureEdge::Rising, 2), refs).result ==
          core::Result::InvalidArgument);
    CHECK(cap.configure(0, 1, 2U * 1024U * 1024U, 0, manual(), refs).detail == core::Detail::Capacity);
    CHECK_FALSE(cap.info().configured);
    CHECK(cap.arm().detail == core::Detail::WrongState);
    REQUIRE(cap.configure(0, 1, 1024U * 1024U / 2U, 0, manual(), refs).ok()); // exactly the capacity
    CHECK(cap.info().configured);
    core_os::native::failNextCaptureAlloc();
    CHECK(cap.configure(0, 1, 10, 0, manual(), refs).result == core::Result::NoMemory);
    CHECK_FALSE(cap.info().configured);
}

TEST_CASE("A manual capture records pre-trigger history around a forced trigger") {
    Capture f;
    auto &cap = f.cap();
    REQUIRE(cap.configure(0, 1, 5, 2, manual(), {&kRaw, 1}).ok());
    CHECK(cap.trigger().detail == core::Detail::WrongState);
    REQUIRE(cap.arm().ok());
    CHECK(cap.arm().detail == core::Detail::WrongState);
    CHECK(cap.configure(0, 1, 5, 2, manual(), {&kRaw, 1}).detail == core::Detail::WrongState);
    const auto before = core_os::realtimeViolations();
    f.feed({1, 2, 3, 4, 5, 6});
    auto s = cap.snapshot();
    CHECK(s.state == CaptureState::Armed);
    CHECK(s.recorded == 5);
    CHECK(s.trigger_index == core::kNoTriggerIndex);
    std::vector<uint32_t> out(5);
    CHECK(cap.read(0, 1, out).detail == core::Detail::WrongState);
    REQUIRE(cap.trigger().ok());
    f.feed({7, 8});
    CHECK(cap.snapshot().state == CaptureState::Triggered);
    CHECK(cap.snapshot().trigger_index == 3); // live: 7 in {4, 5, 6, 7, 8}
    f.feed({9});
    s = cap.snapshot();
    REQUIRE(s.state == CaptureState::Done);
    CHECK(core_os::realtimeViolations() == before); // recording never allocates
    CHECK(s.recorded == 5);
    CHECK(s.trigger_index == 2);
    CHECK(s.period_us == 1000);
    CHECK(s.trigger_us > 0);
    CHECK(f.readAll() == std::vector<uint32_t>{5, 6, 7, 8, 9});
    CHECK(cap.read(4, 2, out).detail == core::Detail::BadValue);
    REQUIRE(cap.read(3, 2, out).ok());
    CHECK(out[0] == 8);CHECK(out[1] == 9);
    f.feed({10, 11}); // Done: the loop leaves the ring alone
    CHECK(f.readAll() == std::vector<uint32_t>{5, 6, 7, 8, 9});
    core::CaptureSnapshot done;
    REQUIRE(cap.takeDone(done));
    CHECK(done.recorded == 5);
    CHECK_FALSE(cap.takeDone(done));
}

TEST_CASE("An early trigger keeps the shorter history and reports its index") {
    Capture f;
    auto &cap = f.cap();
    REQUIRE(cap.configure(0, 1, 6, 4, change(kRaw), {&kRaw, 1}).ok());
    f.m().raw = 1;
    REQUIRE(cap.arm().ok());
    f.feed({1, 2, 2, 2, 2}); // the change fires on the second sample
    const auto s = cap.snapshot();
    REQUIRE(s.state == CaptureState::Done);
    CHECK(s.trigger_index == 1);
    CHECK(s.recorded == 3);
    CHECK(f.readAll() == std::vector<uint32_t>{1, 2, 2});
    // Re-arm reuses the ring and forgets the previous samples.
    REQUIRE(cap.arm().ok());
    CHECK(cap.snapshot().recorded == 0);
    f.feed({5, 5, 5});
    CHECK(cap.snapshot().state == CaptureState::Armed);
    REQUIRE(cap.abort().ok());
    CHECK(cap.snapshot().state == CaptureState::Idle);
    CHECK(cap.info().configured);
    f.feed({6});
    CHECK(cap.snapshot().recorded == 0);
    core::CaptureSnapshot done;
    CHECK_FALSE(cap.takeDone(done));
}

TEST_CASE("Threshold triggers compare in the item's type on either edge") {
    SUBCASE("f32 rising") {
        Capture f;
        REQUIRE(f.cap().configure(0, 1, 4, 1, threshold(kPos, CaptureEdge::Rising, testmod::f(1.5F)), {&kPos, 1}).ok());
        REQUIRE(f.cap().arm().ok());
        for (float v : {2.F, 1.F, -3.F, 1.5F, 0.F, 0.F}) { f.m().position = v; f.tick(); }
        const auto s = f.cap().snapshot();
        REQUIRE(s.state == CaptureState::Done);
        CHECK(f.readAll() == std::vector<uint32_t>{testmod::f(-3.F), testmod::f(1.5F), testmod::f(0.F), testmod::f(0.F)});
    }
    SUBCASE("f32 falling across negative values") {
        Capture f;
        REQUIRE(f.cap().configure(0, 1, 2, 1, threshold(kPos, CaptureEdge::Falling, testmod::f(-1.F)), {&kPos, 1}).ok());
        REQUIRE(f.cap().arm().ok());
        for (float v : {0.F, -0.5F, -2.F, 5.F}) { f.m().position = v; f.tick(); }
        CHECK(f.readAll() == std::vector<uint32_t>{testmod::f(-0.5F), testmod::f(-2.F)});
    }
    SUBCASE("u32 above 2^31 is unsigned") {
        Capture f;
        REQUIRE(f.cap().configure(0, 1, 2, 1, threshold(kRaw, CaptureEdge::Rising, 0x80000000U), {&kRaw, 1}).ok());
        REQUIRE(f.cap().arm().ok());
        f.feed({5, 0x7FFFFFFFU, 0x90000000U, 1});
        CHECK(f.readAll() == std::vector<uint32_t>{0x7FFFFFFFU, 0x90000000U});
    }
    SUBCASE("a trigger signal need not be captured") {
        Capture f;
        REQUIRE(f.cap().configure(0, 1, 2, 0, change(kReady), {&kRaw, 1}).ok());
        REQUIRE(f.cap().arm().ok());
        f.feed({1, 2});
        f.m().ready = true;
        f.feed({3, 4});
        CHECK(f.readAll() == std::vector<uint32_t>{3, 4});
    }
}

TEST_CASE("Capture dividers, wraparound and irregular spacing") {
    Capture f;
    auto &cap = f.cap();
    const std::array<core::Ref, 2> refs{kRaw, {defs::kLoopTypeId, 0, defs::loop::kTicks}};
    REQUIRE(cap.configure(0, 3, 4, 3, manual(), refs).ok());
    REQUIRE(cap.arm().ok());
    for (uint32_t v = 0; v < 30; ++v) { f.feed({v}); } // the first tick records, then every third
    REQUIRE(cap.trigger().ok());
    f.feed({30, 31, 32});
    auto s = cap.snapshot();
    REQUIRE(s.state == CaptureState::Done);
    CHECK_FALSE(s.irregular);
    const auto values = f.readAll();
    REQUIRE(values.size() == 8);
    CHECK(values[0] == 21);CHECK(values[2] == 24);CHECK(values[4] == 27);CHECK(values[6] == 30);
    CHECK(values[7] - values[5] == 3); // loop tick statistics, three ticks apart
    // A late tick (more than 1.5 periods) marks the capture irregular.
    REQUIRE(cap.arm().ok());
    f.tick();
    { core_os::RealtimeScope guard(true); cap.sample(0, core_os::nowUs() + 100000, 1000); }
    CHECK(cap.snapshot().irregular);
    REQUIRE(cap.abort().ok());
    REQUIRE(cap.arm().ok());
    CHECK_FALSE(cap.snapshot().irregular);
    cap.sample(0, core_os::nowUs(), 500); // a rate change
    CHECK(cap.snapshot().irregular);
    REQUIRE(cap.abort().ok());
    cap.sample(1, core_os::nowUs(), 1000); // another loop never records
    CHECK(cap.snapshot().recorded == 0);
}

TEST_CASE("Capture recording survives disarm and ignores other loops") {
    Capture f;
    auto &cap = f.cap();
    // A signal: a disarm transition resets inputs, never signals.
    REQUIRE(cap.configure(0, 1, 3, 0, manual(), {&kPos, 1}).ok());
    REQUIRE(cap.arm().ok());
    REQUIRE(cap.trigger().ok());
    f.c.disarm();
    f.m().position = 1.F; f.tick();
    f.c.loop(1).runTick(1);
    f.m().position = 2.F; f.tick();
    f.m().position = 3.F; f.tick();
    CHECK(cap.snapshot().state == CaptureState::Done);
    CHECK(f.readAll() == std::vector<uint32_t>{testmod::f(1.F), testmod::f(2.F), testmod::f(3.F)});
}

TEST_CASE("ABORT is definite against a recording loop thread") {
    Capture f;
    auto &cap = f.cap();
    const std::array<core::Ref, 2> refs{kRaw, kPos};
    std::atomic<bool> done{false};
    // The recording side alone, as the loop calls it (a preempted whole tick
    // could overrun and count LoopOverBudget, which the native sink records).
    std::thread loop([&] {
        uint32_t v = 0;
        while (!done.load()) {
            f.m().raw = ++v;
            const auto now = core_os::nowUs();
            { core_os::RealtimeScope guard(true); cap.sample(0, now, 1000); }
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 200; ++i) {
        const uint32_t samples = 50U + static_cast<uint32_t>(i % 7);
        REQUIRE(cap.configure(0, 1, samples, 10, manual(), refs).ok());
        REQUIRE(cap.arm().ok());
        if (i % 3 == 0) { (void)cap.trigger(); }
        std::this_thread::yield();
        REQUIRE(cap.abort().ok());
        CHECK(cap.snapshot().state == CaptureState::Idle);
    }
    // A complete capture under the running loop.
    REQUIRE(cap.configure(0, 1, 20, 5, manual(), refs).ok());
    REQUIRE(cap.arm().ok());
    while (cap.snapshot().recorded < 5) { std::this_thread::yield(); }
    REQUIRE(cap.trigger().ok());
    while (cap.snapshot().state != CaptureState::Done) { std::this_thread::yield(); }
    done.store(true);
    loop.join();
    CHECK(cap.snapshot().recorded == 20);
    CHECK(cap.snapshot().trigger_index == 5);
}

TEST_CASE("Capture wire ops, STATUS, READ paging and the done event") {
    Capture f;
    core::CommandService service(f.c, {});
    std::array<uint8_t, 1024> out{};
    auto run = [&](const std::vector<uint8_t> &req) { return service.handle(req, out); };
    // CONFIG: loop 0, divider 1, 300 samples, pre 2, manual, refs raw + position.
    std::vector<uint8_t> config(26 + 8);
    {
        drivelink::protocol::wire::Writer w(config.data(), config.size());
        w.appendU8(cw::kVersion);w.appendU8(7);w.appendU8(6);w.appendU8(1);
        w.appendU8(0);w.appendU16Le(1);w.appendU32Le(300);w.appendU32Le(2);
        w.appendU8(0);w.appendU32Le(0);w.appendU8(0);w.appendU32Le(0);w.appendU8(2);
        for (const auto &r : {kRaw, kPos}) { w.appendU8(r.type);w.appendU8(r.instance);w.appendU16Le(r.id); }
    }
    for (size_t length = 4; length < config.size(); ++length) {
        CHECK(service.handle({config.data(), length}, out) == 6);CHECK(out[4] != 0);
    }
    auto bad_kind = config;bad_kind[15] = 4;
    CHECK(run(bad_kind) == 6);CHECK(out[5] == static_cast<uint8_t>(core::Detail::BadValue));
    CHECK_FALSE(f.cap().info().configured);
    REQUIRE(run(config) == 6);CHECK(out[4] == 0);
    CHECK(run(request(2, {1})) == 6);CHECK(out[4] != 0); // ARM has an empty body
    REQUIRE(run(request(2)) == 6);CHECK(out[4] == 0);
    for (uint32_t v = 0; v < 400; ++v) { f.feed({v}); }
    REQUIRE(run(request(6)) == 6);CHECK(out[4] == 0);
    for (uint32_t v = 400; v < 700; ++v) { f.feed({v}); }
    // STATUS
    REQUIRE(run(request(4)) == 6 + 44 + 8);
    drivelink::protocol::wire::Reader r(out.data() + 6, 52);
    CHECK(r.readU8() == static_cast<uint8_t>(CaptureState::Done));CHECK(r.readU8() == 1);
    CHECK(r.readU8() == 0);CHECK(r.readU16Le() == 1);CHECK(r.readU32Le() == 300);CHECK(r.readU32Le() == 2);
    CHECK(r.readU8() == 0);CHECK(r.readU32Le() == 0);CHECK(r.readU8() == 0);CHECK(r.readU32Le() == 0);
    CHECK(r.readU32Le() == 300);CHECK(r.readU32Le() == 2);CHECK(r.readU32Le() == 1000);CHECK(r.readU64Le() > 0);
    CHECK(r.readU8() == 2);CHECK(r.readU8() == kRaw.type);r.readU8();CHECK(r.readU16Le() == kRaw.id);
    r.readU32Le();CHECK(r.done());
    // READ: at most 512 bytes of values per reply.
    std::vector<uint8_t> read_body(6);
    auto read = [&](uint32_t first, uint16_t count) {
        drivelink::protocol::wire::Writer w(read_body.data(), read_body.size());
        w.appendU32Le(first);w.appendU16Le(count);
        return run(request(5, read_body));
    };
    CHECK(read(0, 65) == 6);CHECK(out[5] == static_cast<uint8_t>(core::Detail::BufferTooSmall));
    CHECK(read(0, 0) == 6);
    CHECK(read(299, 2) == 6);CHECK(out[5] == static_cast<uint8_t>(core::Detail::BadValue));
    REQUIRE(read(0, 64) == 6 + 6 + 512);
    drivelink::protocol::wire::Reader page(out.data() + 6, 518);
    CHECK(page.readU32Le() == 0);CHECK(page.readU16Le() == 64);
    const uint32_t first = page.readU32Le();
    CHECK(first == 398 + 0); // two pre-trigger samples before the trigger at 400
    REQUIRE(read(299, 1) == 6 + 6 + 8);
    drivelink::protocol::wire::Reader last(out.data() + 12, 8);
    CHECK(last.readU32Le() == 697);
    // The done event, once.
    std::array<uint8_t, core::kCaptureDoneEventBytes> event{};
    REQUIRE(core::captureDoneEvent(f.c, event) == core::kCaptureDoneEventBytes);
    drivelink::protocol::wire::Reader e(event.data(), event.size());
    CHECK(e.readU8() == cw::kVersion);CHECK(e.readU8() == static_cast<uint8_t>(defs::CoreEvent::CaptureDone));
    CHECK(e.readU32Le() == 300);CHECK(e.readU32Le() == 2);CHECK(e.readU8() == 1);
    CHECK(core::captureDoneEvent(f.c, event) == 0);
    CHECK(core::captureStatusBits(f.c) == (3U << 6));
    // GET_INFO reports the capacity; ABORT and an unknown op.
    REQUIRE(run({cw::kVersion, 1, 1, 1}) > 20);
    drivelink::protocol::wire::Reader info(out.data() + 6, 64);
    info.readU8();info.skip(info.readU16Le());info.skip(info.readU16Le());info.readU32Le();
    CHECK(info.readU32Le() == 4U * 1024U * 1024U);
    REQUIRE(run(request(3)) == 6);CHECK(out[4] == 0);
    CHECK(f.cap().snapshot().state == CaptureState::Idle);
    CHECK(run(request(9)) == 6);CHECK(out[5] == static_cast<uint8_t>(core::Detail::Unsupported));
    // The capture limit is a read-only system param.
    CHECK(getParam(f.c, 0, 0, defs::system::kLimitsCaptureRefs) == 16);
}

TEST_CASE("Capture limits follow the payload and the device's memory") {
    CHECK(cw::captureRefCapacity(1024, 16) == 16);
    CHECK(cw::captureRefCapacity(1024, 200) == 64);
    CHECK(cw::captureRefCapacity(128, 64) == 19);
    CHECK(cw::captureRefCapacity(50, 16) == 0);
    reset();
    core_os::native::setCaptureCapacity(core_os::kCaptureFallbackBytes);
    core::CoreConfig cfg = testConfig();
    cfg.capture_refs = 0;
    core::Core c(cfg);
    CHECK(c.capture().capacityBytes() == core_os::kCaptureFallbackBytes);
    CHECK(c.capture().refCapacity() == 1);
    core_os::native::setCaptureCapacity(4U * 1024U * 1024U);
    cfg.capture_max_bytes = 1000;
    core::Core small(cfg);
    CHECK(small.capture().capacityBytes() == 1000);
}

TEST_CASE("Refused CONFIG and ARM never take a tick from a recording capture") {
    Capture f;
    auto &cap = f.cap();
    REQUIRE(cap.configure(0, 1, 400, 100, manual(), {&kRaw, 1}).ok());
    REQUIRE(cap.arm().ok());
    std::atomic<bool> done{false};
    const int64_t base = core_os::nowUs();
    std::thread loop([&] {
        // Evenly spaced device time: only a lost claim can mark it irregular.
        uint32_t v = 0;
        while (!done.load()) {
            ++v; f.m().raw = v;
            { core_os::RealtimeScope guard(true); cap.sample(0, base + static_cast<int64_t>(v) * 1000, 1000); }
            if (v % 8 == 0) { std::this_thread::yield(); }
        }
    });
    unsigned refusals = 0;
    const auto deadline = core_os::nowUs() + 20000000;
    auto overdue = [&](const char *where) {
        if (core_os::nowUs() < deadline) { return false; }
        const auto s = cap.snapshot();
        MESSAGE(where << " state " << int(s.state) << " recorded " << s.recorded);
        return true;
    };
    while (cap.snapshot().recorded < 300 && !overdue("fill")) {
        CHECK(cap.configure(0, 1, 10, 0, manual(), {&kRaw, 1}).detail == core::Detail::WrongState);
        CHECK(cap.arm().detail == core::Detail::WrongState);
        ++refusals;
    }
    CHECK(cap.trigger().ok());
    // Only watch now: a refusal racing the Done store could legitimately re-arm.
    while (cap.snapshot().state != CaptureState::Done && !overdue("post")) { std::this_thread::yield(); }
    done.store(true);
    loop.join();
    const auto s = cap.snapshot();
    CHECK(refusals > 0);
    CHECK_FALSE(s.irregular);
    const auto values = f.readAll();
    REQUIRE(values.size() == 400);
    for (size_t i = 1; i < values.size(); ++i) { REQUIRE(values[i] == values[i - 1] + 1); }
}

TEST_CASE("Triggers compare recorded samples at a divider, and the trigger index is live") {
    Capture f;
    auto &cap = f.cap();
    // Divider 2: the trigger sees samples 0, 2, 4 ...; a crossing between
    // recorded samples is caught against the previous recorded value.
    REQUIRE(cap.configure(0, 2, 3, 1, threshold(kRaw, CaptureEdge::Rising, 10), {&kRaw, 1}).ok());
    REQUIRE(cap.arm().ok());
    f.feed({1, 2, 3, 20, 5, 6, 7, 8});
    CHECK(cap.snapshot().state == CaptureState::Armed); // 20 fell between recorded samples
    CHECK(cap.snapshot().recorded == 3);
    REQUIRE(cap.abort().ok());
    // Threshold at a divider: recorded 3 then 11 crosses 10 rising.
    REQUIRE(cap.arm().ok());
    f.feed({3, 0, 11, 0, 12, 0});
    REQUIRE(cap.snapshot().state == CaptureState::Done);
    CHECK(cap.snapshot().trigger_index == 1);
    CHECK(f.readAll() == std::vector<uint32_t>{3, 11, 12});
    // Live while Triggered: the trigger sample sits behind the newest sample.
    REQUIRE(cap.configure(0, 1, 10, 2, manual(), {&kRaw, 1}).ok());
    REQUIRE(cap.arm().ok());
    f.feed({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}); // the ring wrapped
    REQUIRE(cap.trigger().ok());
    f.feed({13, 14, 15});
    auto s = cap.snapshot();
    REQUIRE(s.state == CaptureState::Triggered);
    CHECK(s.recorded == 10);
    CHECK(s.trigger_index == 7); // 13 is the 8th of the 10 newest samples
    f.feed({16, 17, 18, 19, 20});
    s = cap.snapshot();
    REQUIRE(s.state == CaptureState::Done);
    CHECK(s.trigger_index == 2);
    CHECK(f.readAll() == std::vector<uint32_t>{11, 12, 13, 14, 15, 16, 17, 18, 19, 20});
}
