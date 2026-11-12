// core: NVS A/B persistence. SAVE, load with fallback, power loss at every
// write, torn blobs, value rules, FACTORY_RESET and REVERT. A reboot is a new
// Core booted on the same native KvStore.

#include "doctest.h"

#include <array>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "core/config_blob.hpp"
#include "core/core.hpp"
#include "core/persistence.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/type_system.hpp"

using namespace testmod;
using core::Detail;
using core::Result;

namespace {

namespace sys = drivelink::protocol::defs::system;
namespace codes = drivelink::protocol::defs::diag::codes;
constexpr uint8_t kSys = drivelink::protocol::defs::kSystemTypeId;

/// A live param write without threads: stage, one tick of the loop, complete.
core::Status setNow(core::Core &c, std::vector<core::SetEntry> entries)
{
    std::vector<uint32_t> applied(entries.size());
    core::PendingSet p;
    const core::Status st = c.params().stage(entries, applied, p);
    if (!st.ok() || !p.active()) {
        return st;
    }
    p.loop->runTick(1);
    return c.params().complete(p, 0);
}

/// The generation in a layout slot (0 when it is missing or invalid).
uint32_t layoutGen(core_os::KvStore &store, int slot)
{
    std::vector<uint8_t> buf(4096);
    size_t len = buf.size();
    if (store.get("core", core::persist::kLayoutKeys[slot], buf.data(), len) != core_os::kOk) {
        return 0;
    }
    buf.resize(len);
    uint32_t gen = 0;
    std::vector<core::LayoutRecord> recs;
    return core::decodeLayoutBlob(buf, gen, recs) ? gen : 0;
}

/// Two fixture.full instances at generation 1, then a SAVE of gain 2.0 (gen 2).
void baseline(core_os::KvStore &store)
{
    reset();
    core::Core c(testConfig());
    bootWith(c, store, {{fx::kTypeId, 0, core::kControlLoop, "a"}, {fx::kTypeId, 1, core::kSensorLoop, "b"}});
    REQUIRE(setNow(c, {entry(fx::kTypeId, 0, fx::kGain, f(2.0f))}).ok());
    REQUIRE(c.save().ok());
}

/// Boot on `store` and read fixture.full 0's gain.
uint32_t bootedGain(core_os::KvStore &store, bool *loaded = nullptr)
{
    reset();
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    if (loaded != nullptr) {
        *loaded = c.configLoadedFromNvs();
    }
    return getParam(c, fx::kTypeId, 0, fx::kGain);
}

} // namespace

TEST_CASE("Persistence: SAVE, then a reboot loads the same values")
{
    reset();
    core_os::KvStore store;
    {
        core::Core c(testConfig());
        bootWith(c, store, {{fx::kTypeId, 0, core::kControlLoop, "a"}, {fx::kTypeId, 1, core::kSensorLoop, "b"}});
        CHECK(c.configLoadedFromNvs());
        CHECK_FALSE(c.nvsDirty());
        REQUIRE(setNow(c, {entry(fx::kTypeId, 0, fx::kGain, f(7.5f)), entry(fx::kTypeId, 0, fx::kOffset, 42)}).ok());
        CHECK(c.nvsDirty());
        REQUIRE(setNow(c, {entry(fx::kTypeId, 1, fx::kMode, 2)}).ok());
        REQUIRE(setNow(c, {entry(kSys, 0, sys::kControlRateHz, 500)}).ok());
        REQUIRE(c.save().ok());
        CHECK_FALSE(c.nvsDirty());
    }
    reset();
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK(c.configLoadedFromNvs());
    CHECK(c.instanceCount() == 2);
    CHECK(getParam(c, fx::kTypeId, 0, fx::kGain) == f(7.5f));
    CHECK(getParam(c, fx::kTypeId, 0, fx::kOffset) == 42);
    CHECK(getParam(c, fx::kTypeId, 1, fx::kMode) == 2);
    CHECK(getParam(c, kSys, 0, sys::kControlRateHz) == 500);
    CHECK(c.loop(core::kControlLoop).periodUs() == 2000);
    // The storage starts at the loaded values too.
    REQUIRE(world().created.size() == 2);
    CHECK(world().created[0]->gain == 7.5f);
    CHECK(world().created[1]->mode_value == FxMode::C);
    CHECK(c.findRecord(fx::kTypeId, 1)->loop == core::kSensorLoop);
    CHECK(std::string(c.findRecord(fx::kTypeId, 1)->label) == "b");
    CHECK(core_os::native::recordedReports().empty());
}

TEST_CASE("Persistence: generations alternate between the slots")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    bootWith(c, store, {{fx::kTypeId, 0, core::kControlLoop, "a"}}); // gen 1, slot b
    CHECK(layoutGen(store, 1) == 1);
    CHECK(layoutGen(store, 0) == 0);
    REQUIRE(c.save().ok()); // gen 2
    CHECK(layoutGen(store, 0) == 2);
    CHECK(layoutGen(store, 1) == 1);
    REQUIRE(c.save().ok()); // gen 3
    CHECK(layoutGen(store, 0) == 2);
    CHECK(layoutGen(store, 1) == 3);
    CHECK(core_os::native::hasKey(store, "t01i00", "p_a"));
    CHECK(core_os::native::hasKey(store, "t01i00", "p_b"));
    CHECK(core_os::native::hasKey(store, "t00i00", "p_b"));
}

TEST_CASE("Persistence: a power loss after any number of writes leaves the previous generation")
{
    // How many writes one SAVE of this layout makes.
    uint32_t writes = 0;
    {
        core_os::KvStore store;
        baseline(store);
        reset();
        core::Core c(testConfig());
        REQUIRE(bootCore(c, store).ok());
        const uint32_t before = core_os::native::writeCount(store);
        REQUIRE(c.save().ok());
        writes = core_os::native::writeCount(store) - before;
    }
    CHECK(writes == 4); // two instances, the system, the layout

    for (uint32_t k = 0; k <= writes; ++k) {
        CAPTURE(k);
        core_os::KvStore store;
        baseline(store); // gen 2: gain 2.0
        {
            reset();
            core::Core c(testConfig());
            REQUIRE(bootCore(c, store).ok());
            REQUIRE(setNow(c, {entry(fx::kTypeId, 0, fx::kGain, f(3.0f))}).ok());
            core_os::native::failWritesAfter(store, k);
            const core::Status st = c.save();
            if (k < writes) {
                CHECK(st.result == Result::Fault);
                CHECK(st.detail == Detail::StorageFailed);
                CHECK(core_os::native::reportCount(codes::core::kNvsWriteFailed) == 1);
                CHECK(c.nvsDirty()); // still unsaved
            } else {
                CHECK(st.ok());
            }
            core_os::native::clearFaults(store);
        }
        bool loaded = false;
        CHECK(bootedGain(store, &loaded) == f(k < writes ? 2.0f : 3.0f));
        CHECK(loaded);
        CHECK(core_os::native::reportCount(codes::core::kConfigLoadFailed) == 0);
    }
}

TEST_CASE("Persistence: after an incomplete newer generation, SAVE never overwrites the complete one")
{
    // gen 2 (slot a) is complete with gain 2.0; gen 3 (slot b) has a valid
    // layout but loses an instance blob, so a boot loads gen 2. The next SAVE
    // must go to slot b (gen 5), not slot a (gen 4): a power loss in it must
    // still leave gen 2.
    for (uint32_t k = 0; k <= 4U; ++k) { // 4 writes: two instances, the system, the layout
        CAPTURE(k);
        core_os::KvStore store;
        baseline(store);
        {
            reset();
            core::Core c(testConfig());
            REQUIRE(bootCore(c, store).ok());
            REQUIRE(setNow(c, {entry(fx::kTypeId, 0, fx::kGain, f(3.0f))}).ok());
            REQUIRE(c.save().ok()); // gen 3, slot b
        }
        core_os::native::removeKey(store, "t01i00", "p_b");
        {
            reset();
            core::Core c(testConfig());
            REQUIRE(bootCore(c, store).ok());
            REQUIRE(getParam(c, fx::kTypeId, 0, fx::kGain) == f(2.0f)); // gen 2
            REQUIRE(setNow(c, {entry(fx::kTypeId, 0, fx::kGain, f(4.0f))}).ok());
            core_os::native::failWritesAfter(store, k);
            const core::Status st = c.save();
            CHECK(st.ok() == (k == 4U));
            core_os::native::clearFaults(store);
            CHECK(layoutGen(store, 0) == 2); // slot a untouched
        }
        bool loaded = false;
        CHECK(bootedGain(store, &loaded) == f(k == 4U ? 4.0f : 2.0f));
        CHECK(loaded);
    }
}

TEST_CASE("Persistence: a torn layout blob or a torn instance blob falls back to the previous generation")
{
    for (const auto mode : {core_os::native::Tear::Truncate, core_os::native::Tear::Corrupt}) {
        for (const uint32_t torn : {0U, 1U, 2U, 3U}) { // fx 0, fx 1, the system, the layout
            CAPTURE(torn);
            CAPTURE(static_cast<int>(mode));
            core_os::KvStore store;
            baseline(store);
            {
                reset();
                core::Core c(testConfig());
                REQUIRE(bootCore(c, store).ok());
                REQUIRE(setNow(c, {entry(fx::kTypeId, 0, fx::kGain, f(3.0f))}).ok());
                core_os::native::tearWriteAfter(store, torn, mode);
                CHECK(c.save().ok()); // the store said yes
            }
            bool loaded = false;
            CHECK(bootedGain(store, &loaded) == f(2.0f));
            CHECK(loaded);
        }
    }
}

TEST_CASE("Persistence: with no complete generation the device boots with the defaults and reports it")
{
    SUBCASE("an instance blob is missing in the only generation")
    {
        reset();
        core_os::KvStore store;
        writeLayout(store, {{fx::kTypeId, 0, core::kControlLoop, "a"}}, 1);
        core_os::native::removeKey(store, "t01i00", "p_b");
        core::Core c(testConfig());
        REQUIRE(bootCore(c, store).ok());
        CHECK_FALSE(c.configLoadedFromNvs());
        CHECK(c.configuredCount() == 0);
        const auto reports = core_os::native::recordedReports();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].code.code == codes::core::kConfigLoadFailed.code);
        CHECK(reports[0].arg0 == 1); // the newest valid layout generation
        CHECK(reports[0].arg1 == core::persist::kLoadIncomplete);
        // The next SAVE goes past it.
        REQUIRE(c.save().ok());
        CHECK(layoutGen(store, 0) == 2);
    }
    SUBCASE("both layout blobs are garbage")
    {
        reset();
        core_os::KvStore store;
        const uint8_t junk[] = {1, 2, 3};
        REQUIRE(store.set("core", "layout_a", junk, sizeof(junk)) == core_os::kOk);
        REQUIRE(store.set("core", "layout_b", junk, sizeof(junk)) == core_os::kOk);
        core::Core c(testConfig());
        REQUIRE(bootCore(c, store).ok());
        CHECK_FALSE(c.configLoadedFromNvs());
        REQUIRE(core_os::native::reportCount(codes::core::kConfigLoadFailed) == 1);
        CHECK(core_os::native::recordedReports()[0].arg1 == core::persist::kLoadNoLayout);
        CHECK(core_os::native::recordedReports()[0].arg0 == 0);
    }
    SUBCASE("a blank store is not a failure")
    {
        reset();
        core_os::KvStore store;
        core::Core c(testConfig());
        REQUIRE(bootCore(c, store).ok());
        CHECK_FALSE(c.configLoadedFromNvs());
        CHECK(core_os::native::recordedReports().empty());
        CHECK(getParam(c, kSys, 0, sys::kControlRateHz) == 1000);
    }
}

TEST_CASE("Persistence: unknown, retired and non-persist ids are ignored; bad values default, others clamp")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {{fx::kTypeId, 0, core::kControlLoop, "a",
                         {{0x7777, 5},                 // unknown
                          {0x0400, 9},                 // retired (old_thing)
                          {fx::kEnabled, 0},           // persist: false
                          {fx::kCount, 77},            // read_only, so not persisted
                          {fx::kPosition, f(1.0f)},    // a signal
                          {fx::kLevel, 200},           // clamped to 10
                          {fx::kMode, 9},              // not an enum value: default
                          {fx::kGain, f(2.5f)}}}},     // fine
                1, {{sys::kControlRateHz, 5000}, {sys::kSensorRateHz, 70000}}); // clamp; not a u16: default
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK(c.configLoadedFromNvs());
    CHECK(getParam(c, fx::kTypeId, 0, fx::kLevel) == 10);
    CHECK(getParam(c, fx::kTypeId, 0, fx::kMode) == 1);
    CHECK(getParam(c, fx::kTypeId, 0, fx::kEnabled) == 1);
    CHECK(getParam(c, fx::kTypeId, 0, fx::kCount) == 0);
    CHECK(getParam(c, fx::kTypeId, 0, fx::kGain) == f(2.5f));
    CHECK(getParam(c, fx::kTypeId, 0, fx::kOffset) == static_cast<uint32_t>(-5)); // missing: default
    CHECK(getParam(c, kSys, 0, sys::kControlRateHz) == 2000);
    CHECK(getParam(c, kSys, 0, sys::kSensorRateHz) == 100);
    CHECK(c.loop(core::kControlLoop).periodUs() == 500);
}

TEST_CASE("Persistence: SAVE is refused while armed, and a write failure answers Fault")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    bootWith(c, store, {{fx::kTypeId, 0, core::kControlLoop, "a"}});
    c.params().setArmed(true);
    CHECK(c.save().result == Result::Armed);
    CHECK(c.factoryReset().result == Result::Armed);
    c.params().setArmed(false);

    REQUIRE(setNow(c, {entry(fx::kTypeId, 0, fx::kGain, f(9.0f))}).ok());
    core_os::native::failNextWrite(store, 0x1105);
    const core::Status st = c.save();
    CHECK(st.result == Result::Fault);
    CHECK(st.detail == Detail::StorageFailed);
    const auto reports = core_os::native::recordedReports();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].code.code == codes::core::kNvsWriteFailed.code);
    CHECK(reports[0].err == 0x1105);
    CHECK(reports[0].arg0 == 0x0100); // t01i00
    CHECK(reports[0].arg1 == core::persist::kNvsOpWrite);
    CHECK(layoutGen(store, 0) == 0); // gen 2 never committed
    CHECK(c.save().ok());            // the failure was one write
    // Past the failed gen 2, and not in gen 1's slot: gen 4, slot a.
    CHECK(layoutGen(store, 0) == 4);
    CHECK(layoutGen(store, 1) == 1);
}

TEST_CASE("Persistence: SAVE erases the namespaces of removed instances")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    bootWith(c, store, {{fx::kTypeId, 0, core::kControlLoop, "a"}, {fx::kTypeId, 1, core::kControlLoop, "b"}});
    CHECK(core_os::native::hasKey(store, "t01i01", "p_b"));
    REQUIRE(c.removeInstance(fx::kTypeId, 1).ok());
    REQUIRE(c.save().ok());
    CHECK_FALSE(core_os::native::hasKey(store, "t01i01", "p_b"));
    CHECK(core_os::native::hasKey(store, "t01i00", "p_a"));
    CHECK(core_os::native::hasKey(store, "t00i00", "p_a"));
    CHECK(core_os::native::namespaceCount(store) == 3); // core, t00i00, t01i00

    // The running device still has it until the reboot; after it, it's gone.
    CHECK(c.instanceCount() == 2);
    reset();
    core::Core next(testConfig());
    REQUIRE(bootCore(next, store).ok());
    CHECK(next.configuredCount() == 1);
    CHECK(next.findRecord(fx::kTypeId, 1) == nullptr);
}

TEST_CASE("Persistence: FACTORY_RESET erases every core namespace")
{
    reset();
    core_os::KvStore store;
    const uint8_t other = 7;
    REQUIRE(store.set("wifi", "ssid", &other, 1) == core_os::kOk); // not ours
    {
        core::Core c(testConfig());
        bootWith(c, store, {{fx::kTypeId, 0, core::kControlLoop, "a"}});
        REQUIRE(c.save().ok());
        REQUIRE(c.factoryReset().ok());
        CHECK(core_os::native::rebootCount() == 0); // the caller's job
    }
    CHECK(core_os::native::namespaceCount(store) == 1);
    CHECK(core_os::native::hasKey(store, "wifi", "ssid"));
    reset();
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK(c.configuredCount() == 0);
    CHECK_FALSE(c.configLoadedFromNvs());
    CHECK(core_os::native::recordedReports().empty()); // blank, not broken

    REQUIRE(c.save().ok());
    core_os::native::failNextWrite(store, 0x1234); // the erase of `core`
    const core::Status st = c.factoryReset();
    CHECK(st.result == Result::Fault);
    CHECK(st.detail == Detail::StorageFailed);
    CHECK(core_os::native::reportCount(codes::core::kNvsWriteFailed) == 1);
    CHECK(core_os::native::hasKey(store, "core", "layout_b")); // gen 1; that erase failed
    CHECK_FALSE(core_os::native::hasKey(store, "t00i00", "p_b")); // the rest went on
}

TEST_CASE("Persistence: REVERT reloads the live params through the staged path")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    // 100 Hz loops: the staging timeout (35 ms) is above a desktop scheduler's lateness.
    writeLayout(store, {{fx::kTypeId, 0, core::kControlLoop, "a", {{fx::kGain, f(2.0f)}}},
                        {fx::kTypeId, 1, core::kSensorLoop, "b", {{fx::kLevel, 4}}}},
                1, {{sys::kControlRateHz, 100}, {sys::kSensorRateHz, 100}});
    REQUIRE(bootCore(c, store).ok());
    REQUIRE(c.start().ok());
    std::array<uint32_t, 2> applied{};
    const core::SetEntry gain = entry(fx::kTypeId, 0, fx::kGain, f(5.0f));
    REQUIRE(c.params().set({&gain, 1}, applied).ok());
    const core::SetEntry level = entry(fx::kTypeId, 1, fx::kLevel, 9);
    REQUIRE(c.params().set({&level, 1}, applied).ok());
    const core::SetEntry rate = entry(kSys, 0, sys::kSensorRateHz, 50);
    REQUIRE(c.params().set({&rate, 1}, applied).ok());
    CHECK(c.nvsDirty());

    REQUIRE(c.revert().ok());
    CHECK_FALSE(c.nvsDirty());
    CHECK(getParam(c, fx::kTypeId, 0, fx::kGain) == f(2.0f));
    CHECK(getParam(c, fx::kTypeId, 1, fx::kLevel) == 4);
    CHECK(getParam(c, kSys, 0, sys::kSensorRateHz) == 100);
    CHECK(c.loop(core::kSensorLoop).periodUs() == 10000);
    c.stop();
    CHECK(world().created[0]->gain == 2.0f); // the loop's storage too
    CHECK(world().created[1]->level == 4);

    // Nothing saved: nothing to revert to.
    core_os::KvStore blank;
    reset();
    core::Core fresh(testConfig());
    REQUIRE(bootCore(fresh, blank).ok());
    const core::Status st = fresh.revert();
    CHECK(st.result == Result::NotFound);
    CHECK(st.detail == Detail::NoSavedConfig);
}

TEST_CASE("Persistence: the ops need a booted core")
{
    reset();
    core::Core c(testConfig());
    REQUIRE(c.init(kTypes).ok());
    CHECK(c.save().detail == Detail::NotAtBoot);
    CHECK(c.revert().detail == Detail::NotAtBoot);
    CHECK(c.factoryReset().detail == Detail::NotAtBoot);
}
