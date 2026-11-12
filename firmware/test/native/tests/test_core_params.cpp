// core: the param service, driven deterministically: stage(), then
// Loop::runTick() by hand, then complete(). No threads.

#include "doctest.h"

#include <array>
#include <limits>
#include <vector>

#include "core/core.hpp"
#include "core_os/clock.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/type_system.hpp"

using namespace testmod;
using core::Detail;
using core::Result;

namespace {

namespace sys = drivelink::protocol::defs::system;

constexpr uint8_t kSys = drivelink::protocol::defs::kSystemTypeId;

/// Instances: full 0 in control, full 1 in sensor, sensor 0 in sensor.
struct Rig {
    Rig() : core(testConfig())
    {
        reset();
        bootWith(core, store,
                 {{fx::kTypeId, 0, core::kControlLoop, "ctl"},
                  {fx::kTypeId, 1, core::kSensorLoop, "sen"},
                  {fxs::kTypeId, 0, core::kSensorLoop, "s"}});
        REQUIRE(world().created.size() == 2);
        ctl = world().created[0];
        sen = world().created[1];
    }

    core::ParamService &params() { return core.params(); }
    core::Loop &control() { return core.loop(core::kControlLoop); }
    core::Loop &sensor() { return core.loop(core::kSensorLoop); }

    /// stage + one control tick + complete(0).
    core::Status setNow(std::span<const core::SetEntry> entries, std::span<uint32_t> applied)
    {
        core::PendingSet p;
        const core::Status st = params().stage(entries, applied, p);
        if (!st.ok() || !p.active()) {
            return st;
        }
        p.loop->runTick(1);
        return params().complete(p, 0);
    }

    uint32_t get(uint8_t type, uint8_t inst, uint16_t id)
    {
        const core::Ref r = ref(type, inst, id);
        uint32_t v = 0xA5A5A5A5;
        REQUIRE(params().get({&r, 1}, {&v, 1}).ok());
        return v;
    }

    core_os::KvStore store;
    core::Core core;
    FullModule *ctl = nullptr;
    FullModule *sen = nullptr;
};

} // namespace

TEST_CASE("Param set: a loop batch is applied at the next tick")
{
    Rig rig;
    const std::array<core::SetEntry, 2> batch{entry(fx::kTypeId, 0, fx::kGain, f(2.5f)),
                                              entry(fx::kTypeId, 0, fx::kOffset, static_cast<uint32_t>(-7))};
    std::array<uint32_t, 2> applied{};
    core::PendingSet p;
    REQUIRE(rig.params().stage(batch, applied, p).ok());
    CHECK(p.loop == &rig.control());
    CHECK(rig.control().stagingState() == core::StagingState::Ready);
    CHECK(rig.ctl->gain == 1.5f); // not before the tick
    CHECK(rig.get(fx::kTypeId, 0, fx::kGain) == f(1.5f));

    rig.control().runTick(1);
    CHECK(rig.ctl->gain == 2.5f);
    CHECK(rig.ctl->offset == -7);
    CHECK(rig.control().stagingState() == core::StagingState::Applied);

    REQUIRE(rig.params().complete(p, 0).ok());
    CHECK_FALSE(p.active());
    CHECK(rig.control().stagingState() == core::StagingState::Empty);
    CHECK(applied[0] == f(2.5f));
    CHECK(rig.get(fx::kTypeId, 0, fx::kGain) == f(2.5f));
    CHECK(rig.get(fx::kTypeId, 0, fx::kOffset) == static_cast<uint32_t>(-7));
    CHECK(rig.sen->gain == 1.5f); // the other instance is untouched
}

TEST_CASE("Param set: applied values are clamped, and get returns them")
{
    Rig rig;
    const std::array<core::SetEntry, 3> batch{entry(fx::kTypeId, 0, fx::kGain, f(5000.0f)),
                                              entry(fx::kTypeId, 0, fx::kLevel, 0),
                                              entry(fx::kTypeId, 0, fx::kOffset, static_cast<uint32_t>(-1000))};
    std::array<uint32_t, 3> applied{};
    REQUIRE(rig.setNow(batch, applied).ok());
    CHECK(applied[0] == f(1000.0f));
    CHECK(applied[1] == 1);
    CHECK(applied[2] == static_cast<uint32_t>(-100));
    CHECK(rig.ctl->gain == 1000.0f);
    CHECK(rig.ctl->level == 1);
    CHECK(rig.ctl->offset == -100);
    CHECK(rig.get(fx::kTypeId, 0, fx::kGain) == f(1000.0f));
    CHECK(rig.get(fx::kTypeId, 0, fx::kLevel) == 1);
}

TEST_CASE("Param set: one bad entry means nothing is applied, and its index is reported")
{
    Rig rig;
    const std::array<core::SetEntry, 3> batch{entry(fx::kTypeId, 0, fx::kGain, f(2.0f)),
                                              entry(fx::kTypeId, 0, fx::kOffset, 1),
                                              entry(fx::kTypeId, 0, fx::kLevel, 300)};
    std::array<uint32_t, 3> applied{};
    core::PendingSet p;
    const core::Status st = rig.params().stage(batch, applied, p);
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::BadValue);
    CHECK(st.index == 2);
    CHECK_FALSE(p.active());
    CHECK(rig.control().stagingState() == core::StagingState::Empty);
    rig.control().runTick(1);
    CHECK(rig.ctl->gain == 1.5f);
    CHECK(rig.ctl->offset == -5);
    CHECK(rig.get(fx::kTypeId, 0, fx::kGain) == f(1.5f));
}

TEST_CASE("Param set: each validation rule")
{
    Rig rig;
    std::array<uint32_t, 1> applied{};
    auto one = [&](core::SetEntry e) { return rig.setNow({&e, 1}, applied); };

    core::Status st = one(entry(fx::kTypeId, 9, fx::kGain, 0)); // no instance 9
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::UnknownRef);
    CHECK(one(entry(fx::kTypeId, 0, 0x7777, 0)).detail == Detail::UnknownRef);
    CHECK(one(entry(0x66, 0, fx::kGain, 0)).detail == Detail::UnknownRef);

    st = one(entry(fx::kTypeId, 0, fx::kPosition, 0)); // a signal
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::WrongKind);
    CHECK(one(entry(fx::kTypeId, 0, fx::kVelocitySp, 0)).detail == Detail::WrongKind); // an input
    CHECK(one(entry(fx::kTypeId, 0, fx::kReset, 0)).detail == Detail::WrongKind);      // an action

    st = one(entry(fx::kTypeId, 0, fx::kCount, 10));
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::ReadOnly);

    // reboot_required: the pending layout (test_core_layout.cpp). 0x0101 names
    // fixture.full 1, which is not an actuator.
    st = one(entry(fx::kTypeId, 0, fx::kActuator, 0x0101));
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::BadRef);
    st = one(entry(fx::kTypeId, 0, fx::kActuator, 0));
    CHECK(st.result == Result::PendingReboot);
    CHECK(st.accepted());

    CHECK(one(entry(fx::kTypeId, 0, fx::kMode, 2)).ok()); // disarmed: fine
    CHECK(rig.ctl->mode_value == FxMode::C);
    rig.params().setArmed(true);
    st = one(entry(fx::kTypeId, 0, fx::kMode, 0));
    CHECK(st.result == Result::Armed);
    CHECK(st.detail == Detail::LockedWhileArmed);
    CHECK(one(entry(fx::kTypeId, 0, fx::kGain, f(3.0f))).ok()); // not locked
    st = one(entry(kSys, 0, sys::kControlRateHz, 500));
    CHECK(st.result == Result::Armed);
    rig.params().setArmed(false);
    CHECK(rig.ctl->mode_value == FxMode::C);

    CHECK(one(entry(fx::kTypeId, 0, fx::kGain, f(std::numeric_limits<float>::quiet_NaN()))).detail ==
          Detail::NonFiniteFloat);
    CHECK(one(entry(fx::kTypeId, 0, fx::kMode, 9)).detail == Detail::BadEnum);
    CHECK(one(entry(fx::kTypeId, 0, fx::kEnabled, 2)).detail == Detail::BadValue);
}

TEST_CASE("Param set: batch shape")
{
    Rig rig;
    std::array<uint32_t, 2> applied{};

    const std::array<core::SetEntry, 2> dup{entry(fx::kTypeId, 0, fx::kGain, f(1.0f)),
                                            entry(fx::kTypeId, 0, fx::kGain, f(2.0f))};
    core::Status st = rig.setNow(dup, applied);
    CHECK(st.detail == Detail::DuplicateRef);
    CHECK(st.index == 1);

    std::vector<core::SetEntry> big(core::kMaxBatchEntries + 1, entry(fx::kTypeId, 0, fx::kGain, 0));
    std::vector<uint32_t> big_out(big.size());
    CHECK(rig.setNow(big, big_out).detail == Detail::BatchTooLarge);

    std::array<uint32_t, 1> small{};
    CHECK(rig.setNow(dup, small).detail == Detail::BufferTooSmall);

    CHECK(rig.setNow({}, {}).ok()); // an empty batch does nothing
}

TEST_CASE("Param set: a batch must not span loops or mix system and loop params")
{
    Rig rig;
    std::array<uint32_t, 2> applied{};

    const std::array<core::SetEntry, 2> cross{entry(fx::kTypeId, 0, fx::kGain, f(2.0f)),
                                              entry(fx::kTypeId, 1, fx::kGain, f(2.0f))};
    core::Status st = rig.setNow(cross, applied);
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::CrossLoopBatch);
    CHECK(st.index == 1);

    const std::array<core::SetEntry, 2> mixed{entry(kSys, 0, sys::kSensorRateHz, 50),
                                              entry(fx::kTypeId, 1, fx::kGain, f(2.0f))};
    st = rig.setNow(mixed, applied);
    CHECK(st.detail == Detail::CrossLoopBatch);
    CHECK(st.index == 1);
    CHECK(rig.sensor().periodUs() == 10000); // nothing applied

    // Two instances in the same loop are fine.
    const std::array<core::SetEntry, 2> same{entry(fx::kTypeId, 1, fx::kGain, f(4.0f)),
                                             entry(fx::kTypeId, 1, fx::kLevel, 4)};
    core::PendingSet p;
    REQUIRE(rig.params().stage(same, applied, p).ok());
    CHECK(p.loop == &rig.sensor());
    rig.sensor().runTick(1);
    REQUIRE(rig.params().complete(p, 0).ok());
    CHECK(rig.sen->gain == 4.0f);
}

TEST_CASE("Param set: a second batch for a busy loop gets StagingBusy")
{
    Rig rig;
    std::array<uint32_t, 1> a_out{};
    std::array<uint32_t, 1> b_out{};
    const core::SetEntry a = entry(fx::kTypeId, 0, fx::kGain, f(2.0f));
    const core::SetEntry b = entry(fx::kTypeId, 0, fx::kLevel, 5);
    const core::SetEntry other = entry(fx::kTypeId, 1, fx::kLevel, 6);

    core::PendingSet pa;
    REQUIRE(rig.params().stage({&a, 1}, a_out, pa).ok());
    core::PendingSet pb;
    const core::Status st = rig.params().stage({&b, 1}, b_out, pb);
    CHECK(st.result == Result::Busy);
    CHECK(st.detail == Detail::StagingBusy);
    CHECK_FALSE(pb.active());

    // The other loop has its own slot.
    core::PendingSet pc;
    REQUIRE(rig.params().stage({&other, 1}, b_out, pc).ok());

    rig.control().runTick(1);
    rig.sensor().runTick(1);
    CHECK(rig.params().complete(pa, 0).ok());
    CHECK(rig.params().complete(pc, 0).ok());
    CHECK(rig.ctl->gain == 2.0f);
    CHECK(rig.ctl->level == 3); // b was never applied
    CHECK(rig.sen->level == 6);

    // Free again.
    CHECK(rig.setNow({&b, 1}, b_out).ok());
    CHECK(rig.ctl->level == 5);
}

TEST_CASE("Param set: a batch the loop doesn't take in time is withdrawn")
{
    Rig rig;
    std::array<uint32_t, 1> out{};
    const core::SetEntry e = entry(fx::kTypeId, 0, fx::kGain, f(3.0f));

    core::PendingSet p;
    REQUIRE(rig.params().stage({&e, 1}, out, p).ok());
    const core::Status st = rig.params().complete(p, 0); // no tick ran
    CHECK(st.result == Result::Busy);
    CHECK(st.detail == Detail::Withdrawn);
    CHECK(rig.control().stagingState() == core::StagingState::Empty);

    rig.control().runTick(1); // a later tick applies nothing
    CHECK(rig.ctl->gain == 1.5f);
    CHECK(rig.ctl->hook_ids.empty());
    CHECK(rig.get(fx::kTypeId, 0, fx::kGain) == f(1.5f));

    // set() with nothing ticking times out after 3 periods + 5 ms.
    const int64_t t0 = core_os::nowUs();
    const core::Status timed = rig.params().set({&e, 1}, out);
    CHECK(timed.detail == Detail::Withdrawn);
    CHECK(core_os::nowUs() - t0 >= core::ParamService::stagingTimeoutUs(rig.control().periodUs()));
    CHECK(core::ParamService::stagingTimeoutUs(1000) == 8000);
    rig.control().runTick(1);
    CHECK(rig.ctl->gain == 1.5f);
}

TEST_CASE("Param set: hooks run in the loop, after every value of the batch is copied")
{
    Rig rig;
    // gain's hook comes first in the batch and records the level it sees.
    const std::array<core::SetEntry, 3> batch{entry(fx::kTypeId, 0, fx::kGain, f(2.0f)),
                                              entry(fx::kTypeId, 0, fx::kLevel, 7),
                                              entry(fx::kTypeId, 0, fx::kOffset, 3)};
    std::array<uint32_t, 3> applied{};
    core::PendingSet p;
    REQUIRE(rig.params().stage(batch, applied, p).ok());
    CHECK(rig.ctl->hook_ids.empty()); // not on the config side

    rig.control().runTick(1);
    REQUIRE(rig.ctl->hook_ids.size() == 2); // gain and level have hooks, offset none
    CHECK(rig.ctl->hook_ids[0] == fx::kGain);
    CHECK(rig.ctl->hook_ids[1] == fx::kLevel);
    CHECK(rig.ctl->hook_saw_level == 7);  // the first hook saw the second value
    CHECK(rig.ctl->hook_saw_gain == 2.0f);
    REQUIRE(rig.params().complete(p, 0).ok());
}

TEST_CASE("Param set: system params apply on the caller and set the loop periods")
{
    Rig rig;
    CHECK(rig.control().periodUs() == 1000); // 1000 Hz default
    CHECK(rig.sensor().periodUs() == 10000); // 100 Hz default
    CHECK(rig.get(kSys, 0, sys::kControlRateHz) == 1000);

    std::array<uint32_t, 2> applied{};
    const std::array<core::SetEntry, 2> batch{entry(kSys, 0, sys::kControlRateHz, 500),
                                              entry(kSys, 0, sys::kSensorRateHz, 1000)}; // clamped to 500
    core::PendingSet p;
    REQUIRE(rig.params().stage(batch, applied, p).ok());
    CHECK_FALSE(p.active()); // done already
    CHECK(applied[0] == 500);
    CHECK(applied[1] == 500);
    CHECK(rig.control().periodUs() == 2000);
    CHECK(rig.sensor().periodUs() == 2000);
    CHECK(rig.get(kSys, 0, sys::kControlRateHz) == 500);
    CHECK(rig.get(kSys, 0, sys::kSensorRateHz) == 500);

    const core::SetEntry low = entry(kSys, 0, sys::kControlRateHz, 1);
    REQUIRE(rig.params().set({&low, 1}, applied).ok());
    CHECK(applied[0] == 100);
    CHECK(rig.control().periodUs() == 10000);

    // The system instance is not in any loop's list.
    CHECK(rig.core.instanceCount() == 3);
    CHECK(core::periodUsFromHz(3) == 333333);
    CHECK(core::periodUsFromHz(0) == 0);
}

TEST_CASE("Param get: params only, from the shadow")
{
    Rig rig;
    const std::array<core::Ref, 3> refs{ref(fx::kTypeId, 0, fx::kLevel), ref(fx::kTypeId, 1, fx::kMode),
                                        ref(kSys, 0, sys::kSensorRateHz)};
    std::array<uint32_t, 3> values{};
    REQUIRE(rig.params().get(refs, values).ok());
    CHECK(values[0] == 3);
    CHECK(values[1] == 1);
    CHECK(values[2] == 100);

    const std::array<core::Ref, 2> bad{ref(fx::kTypeId, 0, fx::kLevel), ref(fx::kTypeId, 0, fx::kPosition)};
    core::Status st = rig.params().get(bad, values);
    CHECK(st.detail == Detail::WrongKind);
    CHECK(st.index == 1);
    const core::Ref missing = ref(fx::kTypeId, 5, fx::kLevel);
    CHECK(rig.params().get({&missing, 1}, values).detail == Detail::UnknownRef);
    std::array<uint32_t, 1> small{};
    CHECK(rig.params().get(refs, small).detail == Detail::BufferTooSmall);
}
