// core: the pending layout and layout validation. Layout ops, labels, the
// pin policy, wiring (category, loop, cycles), remove while referenced,
// pending SETs and pending_reboot.

#include "doctest.h"

#include <array>
#include <cstring>
#include <vector>

#include "core/core.hpp"
#include "core/layout_validation.hpp"
#include "core/wiring_order.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"

using namespace testmod;
using core::Detail;
using core::Result;

namespace {

namespace sys = drivelink::protocol::defs::system;
namespace lp = drivelink::protocol::defs::loop;
constexpr uint8_t kSys = drivelink::protocol::defs::kSystemTypeId;
constexpr uint8_t kCtl = core::kFactoryControlLoop;
constexpr uint8_t kSen = core::kFactorySensorLoop;

/// A core booted on an empty store.
struct Rig {
    Rig() : core(testConfig())
    {
        reset();
        REQUIRE(bootCore(core, store).ok());
    }

    uint8_t add(uint8_t type, uint8_t loop, const char *label)
    {
        uint8_t inst = 0xEE;
        REQUIRE(core.addInstance(type, loop, label, inst).ok());
        return inst;
    }

    core::Status set(std::vector<core::SetEntry> entries)
    {
        applied.assign(entries.size(), 0);
        return core.params().set(entries, applied);
    }

    uint32_t pending(uint8_t type, uint8_t inst, uint16_t id)
    {
        const core::Ref r = ref(type, inst, id);
        uint32_t v = 0xA5A5A5A5;
        REQUIRE(core.getPending({&r, 1}, {&v, 1}).ok());
        return v;
    }

    core_os::KvStore store;
    core::Core core;
    std::vector<uint32_t> applied;
};

} // namespace

TEST_CASE("Layout: add picks the lowest free id; remove, label and discard")
{
    Rig rig;
    CHECK_FALSE(rig.core.pendingReboot());
    CHECK_FALSE(rig.core.pendingLayout().active());
    CHECK(rig.add(fx::kTypeId, kCtl, "a") == 0);
    CHECK(rig.core.pendingReboot());
    CHECK(rig.core.nvsDirty());
    CHECK(rig.add(fx::kTypeId, kSen, "b") == 1);
    CHECK(rig.add(fxs::kTypeId, kSen, "s") == 0); // ids are per type
    REQUIRE(rig.core.removeInstance(fx::kTypeId, 0).ok());
    CHECK(rig.add(fx::kTypeId, kCtl, "c") == 0); // reused
    CHECK(rig.core.pendingLayout().entries().size() == 5);

    REQUIRE(rig.core.setLabel(fx::kTypeId, 1, "renamed").ok());
    CHECK(std::strcmp(rig.core.pendingLayout().entries()[2].label, "renamed") == 0);

    CHECK(rig.core.removeInstance(fx::kTypeId, 9).detail == Detail::UnknownRef);
    CHECK(rig.core.setLabel(fx::kTypeId, 9, "x").detail == Detail::UnknownRef);
    uint8_t inst = 0;
    core::Status st = rig.core.addInstance(0x66, kCtl, "x", inst);
    CHECK(st.result == Result::NotFound);
    CHECK(st.detail == Detail::UnknownType);
    st = rig.core.addInstance(fxs::kTypeId, kCtl, "x", inst); // sensor-only type
    CHECK(st.detail == Detail::LoopMismatch);
    CHECK(rig.core.addInstance(fx::kTypeId, 2, "x", inst).detail == Detail::LoopMismatch);

    // Nothing is running: the pending layout waits for SAVE and a reboot.
    CHECK(rig.core.instanceCount() == 2);
    REQUIRE(rig.core.discardPending().ok());
    CHECK_FALSE(rig.core.pendingLayout().active());
    CHECK(rig.core.pendingLayout().entries().empty());
    CHECK_FALSE(rig.core.pendingReboot());
}

TEST_CASE("Layout: the capacity bounds the layout")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig(2));
    REQUIRE(bootCore(c, store).ok());
    uint8_t inst = 0;
    REQUIRE(c.addInstance(fx::kTypeId, kCtl, "a", inst).ok());
    REQUIRE(c.addInstance(fx::kTypeId, kCtl, "b", inst).ok());
    const core::Status st = c.addInstance(fx::kTypeId, kCtl, "c", inst);
    CHECK(st.result == Result::NoMemory);
    CHECK(st.detail == Detail::Capacity);
    CHECK(c.pendingLayout().entries().size() == 4); // undone
}

TEST_CASE("Layout: labels are non-empty, at most 16 characters and unique")
{
    Rig rig;
    uint8_t inst = 0;
    CHECK(rig.core.addInstance(fx::kTypeId, kCtl, "", inst).detail == Detail::BadLabel);
    CHECK(rig.core.addInstance(fx::kTypeId, kCtl, "seventeen_chars__", inst).detail == Detail::BadLabel);
    CHECK(rig.core.addInstance(fx::kTypeId, kCtl, nullptr, inst).detail == Detail::BadLabel);
    CHECK_FALSE(rig.core.pendingLayout().active()); // a refused first op leaves no copy
    CHECK_FALSE(rig.core.pendingReboot());
    rig.add(fx::kTypeId, kCtl, "sixteen_chars___");
    rig.add(fx::kTypeId, kCtl, "b");
    CHECK(rig.core.addInstance(fx::kTypeId, kCtl, "b", inst).detail == Detail::LabelInUse);
    CHECK(rig.core.setLabel(fx::kTypeId, 0, "b").detail == Detail::LabelInUse);
    CHECK(rig.core.setLabel(fx::kTypeId, 0, "").detail == Detail::BadLabel);
    CHECK(rig.core.setLabel(fx::kTypeId, 0, "b2").ok());
    CHECK(rig.core.pendingLayout().entries().size() == 4);
}

TEST_CASE("Layout: pins must be allowed and unique; 0xFF claims nothing")
{
    Rig rig;
    rig.add(act::kTypeId, kCtl, "a0");
    rig.add(act::kTypeId, kCtl, "a1"); // both on 0xFF: no conflict
    CHECK(rig.pending(act::kTypeId, 0, act::kPin) == defs::kNoGpio);

    core::Status st = rig.set({entry(act::kTypeId, 0, act::kPin, 5)});
    CHECK(st.result == Result::PendingReboot);
    CHECK(st.accepted());
    CHECK(rig.applied[0] == 5);
    CHECK(rig.pending(act::kTypeId, 0, act::kPin) == 5);

    st = rig.set({entry(act::kTypeId, 1, act::kPin, 5)});
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::GpioConflict);
    CHECK(rig.pending(act::kTypeId, 1, act::kPin) == defs::kNoGpio); // undone
    CHECK(rig.set({entry(act::kTypeId, 1, act::kPin, 13)}).detail == Detail::GpioNotAllowed);
    CHECK(rig.set({entry(act::kTypeId, 1, act::kPin, 45)}).detail == Detail::GpioNotAllowed);
    CHECK(rig.set({entry(act::kTypeId, 1, act::kPin, 6)}).accepted());

    // Two pins of one instance.
    const uint8_t w = rig.add(wd::kTypeId, kCtl, "w");
    st = rig.set({entry(wd::kTypeId, w, wd::kPinA, 7), entry(wd::kTypeId, w, wd::kPinB, 7)});
    CHECK(st.detail == Detail::GpioConflict);
    CHECK(rig.set({entry(wd::kTypeId, w, wd::kPinA, 7), entry(wd::kTypeId, w, wd::kPinB, 8)}).accepted());
    // Freeing a pin and taking it elsewhere in one batch is fine.
    st = rig.set({entry(act::kTypeId, 0, act::kPin, defs::kNoGpio), entry(act::kTypeId, 1, act::kPin, 5)});
    CHECK(st.detail == Detail::None);
    CHECK(st.accepted());

    // The batch index names the entry that hit the failing instance.
    st = rig.set({entry(act::kTypeId, 0, act::kPin, 9), entry(wd::kTypeId, w, wd::kPinA, 5)});
    CHECK(st.detail == Detail::GpioConflict);
    CHECK(st.index == 1);
}

TEST_CASE("Layout: wiring must name an instance of its category, in the same loop")
{
    Rig rig;
    const uint8_t w = rig.add(wd::kTypeId, kCtl, "w"); // unwired (0) is fine
    const uint8_t f0 = rig.add(fb::kTypeId, kCtl, "f0");
    const uint8_t f1 = rig.add(fb::kTypeId, kSen, "f1");
    const uint8_t full = rig.add(fx::kTypeId, kCtl, "full");

    CHECK(rig.set({entry(wd::kTypeId, w, wd::kFeedback, wire(fx::kTypeId, full))}).detail == Detail::BadRef);
    CHECK(rig.set({entry(wd::kTypeId, w, wd::kFeedback, wire(fb::kTypeId, 7))}).detail == Detail::BadRef);
    CHECK(rig.set({entry(wd::kTypeId, w, wd::kFeedback, 0x10000U | wire(fb::kTypeId, f0))}).detail ==
          Detail::BadRef);
    CHECK(rig.set({entry(wd::kTypeId, w, wd::kFeedback, wire(fb::kTypeId, f1))}).detail == Detail::LoopMismatch);
    CHECK(rig.set({entry(wd::kTypeId, w, wd::kFeedback, wire(fb::kTypeId, f0))}).accepted());
    // The `fixture` category holds fixture.full, .sensor and .wired.
    CHECK(rig.set({entry(wd::kTypeId, w, wd::kPeer, wire(fx::kTypeId, full))}).accepted());
    CHECK(rig.pending(wd::kTypeId, w, wd::kFeedback) == wire(fb::kTypeId, f0));
}

TEST_CASE("Layout: no cycle in the ref graph or the tick graph")
{
    Rig rig;
    const uint8_t w0 = rig.add(wd::kTypeId, kCtl, "w0");
    const uint8_t w1 = rig.add(wd::kTypeId, kCtl, "w1");
    const uint8_t w2 = rig.add(wd::kTypeId, kCtl, "w2");

    SUBCASE("self")
    {
        CHECK(rig.set({entry(wd::kTypeId, w0, wd::kPeer, wire(wd::kTypeId, w0))}).detail == Detail::WiringCycle);
    }
    SUBCASE("ref graph")
    {
        REQUIRE(rig.set({entry(wd::kTypeId, w0, wd::kPeer, wire(wd::kTypeId, w1))}).accepted());
        CHECK(rig.set({entry(wd::kTypeId, w1, wd::kPeer, wire(wd::kTypeId, w0))}).detail == Detail::WiringCycle);
        CHECK(rig.set({entry(wd::kTypeId, w1, wd::kFollower, wire(wd::kTypeId, w0))}).detail ==
              Detail::WiringCycle);
    }
    SUBCASE("tick graph only")
    {
        // Refs w0→w1, w0→w2, w2→w1 have no cycle, but the ticks do:
        // w1 before w0 (peer), w0 before w2 (follower), w2 before w1 (follower).
        REQUIRE(rig.set({entry(wd::kTypeId, w0, wd::kPeer, wire(wd::kTypeId, w1)),
                         entry(wd::kTypeId, w0, wd::kFollower, wire(wd::kTypeId, w2))})
                    .accepted());
        const core::Status st = rig.set({entry(wd::kTypeId, w2, wd::kFollower, wire(wd::kTypeId, w1))});
        CHECK(st.detail == Detail::WiringCycle);
        CHECK(rig.pending(wd::kTypeId, w2, wd::kFollower) == 0);
    }
}

TEST_CASE("Layout: an instance is not removed while another is wired to it")
{
    Rig rig;
    const uint8_t a = rig.add(act::kTypeId, kCtl, "a");
    const uint8_t w = rig.add(wd::kTypeId, kCtl, "w");
    REQUIRE(rig.set({entry(wd::kTypeId, w, wd::kActuator, wire(act::kTypeId, a))}).accepted());
    const core::Status st = rig.core.removeInstance(act::kTypeId, a);
    CHECK(st.result == Result::InvalidArgument);
    CHECK(st.detail == Detail::Referenced);
    REQUIRE(rig.set({entry(wd::kTypeId, w, wd::kActuator, 0)}).accepted());
    CHECK(rig.core.removeInstance(act::kTypeId, a).ok());
}

TEST_CASE("Layout: pending SETs, mixed batches and GET_PENDING")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    bootWith(c, store, {{fx::kTypeId, 0, kCtl, "a"}});
    std::array<uint32_t, 2> applied{};

    // A reboot_required write on a running instance goes to the pending copy.
    const core::SetEntry wiring = entry(fx::kTypeId, 0, fx::kActuator, 0);
    core::Status st = c.params().set({&wiring, 1}, applied);
    CHECK(st.result == Result::PendingReboot);
    CHECK(c.pendingReboot());

    // Mixed with a live param: refused.
    const std::array<core::SetEntry, 2> mixed{wiring, entry(fx::kTypeId, 0, fx::kGain, f(2.0f))};
    st = c.params().set(mixed, applied);
    CHECK(st.detail == Detail::CrossLoopBatch);
    CHECK(st.index == 1);

    // An added instance takes live writes into the pending layout too.
    uint8_t inst = 0;
    REQUIRE(c.addInstance(fx::kTypeId, kSen, "new", inst).ok());
    CHECK(inst == 1);
    const core::SetEntry gain = entry(fx::kTypeId, 1, fx::kGain, f(5000.0f));
    st = c.params().set({&gain, 1}, applied);
    CHECK(st.result == Result::PendingReboot);
    CHECK(applied[0] == f(1000.0f)); // clamped
    const core::Ref r = ref(fx::kTypeId, 1, fx::kGain);
    uint32_t v = 0;
    CHECK(c.params().get({&r, 1}, {&v, 1}).detail == Detail::UnknownRef); // not configured yet
    REQUIRE(c.getPending({&r, 1}, {&v, 1}).ok());
    CHECK(v == f(1000.0f));
    // Mixing a pending-only instance with a running one is refused too.
    const std::array<core::SetEntry, 2> mixed2{gain, entry(fx::kTypeId, 0, fx::kGain, f(2.0f))};
    CHECK(c.params().set(mixed2, applied).detail == Detail::CrossLoopBatch);

    // GET_PENDING: a copied instance's live params come from its record.
    const core::SetEntry live = entry(fx::kTypeId, 0, fx::kLevel, 7);
    core::PendingSet p;
    REQUIRE(c.params().stage({&live, 1}, applied, p).ok());
    p.loop->runTick(1);
    REQUIRE(c.params().complete(p, 0).ok());
    const std::array<core::Ref, 3> refs{ref(fx::kTypeId, 0, fx::kLevel), ref(defs::kLoopTypeId, 0, lp::kRateHz),
                                        ref(fx::kTypeId, 0, fx::kPosition)};
    std::array<uint32_t, 3> vals{};
    st = c.getPending(refs, vals);
    CHECK(st.detail == Detail::WrongKind);
    CHECK(st.index == 2);
    REQUIRE(c.getPending(std::span(refs).first(2), vals).ok());
    CHECK(vals[0] == 7);
    CHECK(vals[1] == 1000);

    REQUIRE(c.discardPending().ok());
    const core::Ref gone = ref(fx::kTypeId, 1, fx::kGain);
    CHECK(c.getPending({&gone, 1}, {&v, 1}).detail == Detail::UnknownRef);
}

TEST_CASE("Layout: pending_reboot follows the pending copy and the last SAVE")
{
    Rig rig;
    CHECK_FALSE(rig.core.pendingReboot());
    rig.add(fx::kTypeId, kCtl, "a");
    CHECK(rig.core.pendingReboot());
    REQUIRE(rig.core.save().ok());
    CHECK(rig.core.pendingReboot()); // saved, not running yet
    CHECK_FALSE(rig.core.nvsDirty());
    REQUIRE(rig.core.discardPending().ok());
    CHECK(rig.core.pendingReboot()); // NVS still holds what was saved
    REQUIRE(rig.core.save().ok());   // the running layout again
    CHECK_FALSE(rig.core.pendingReboot());

    // After the reboot that applies a saved pending layout, it is clear.
    rig.add(fx::kTypeId, kCtl, "a");
    REQUIRE(rig.core.save().ok());
    reset();
    core::Core next(testConfig());
    REQUIRE(bootCore(next, rig.store).ok());
    CHECK_FALSE(next.pendingReboot());
    CHECK(next.instanceCount() == 3);
    CHECK(stateOf(next, fx::kTypeId, 0) == core::InstanceState::Running);
    CHECK(getParam(next, fx::kTypeId, 0, fx::kGain) == f(1.5f)); // the defaults
}

TEST_CASE("Layout: ops are refused while armed and before boot")
{
    Rig rig;
    rig.core.params().setArmed(true);
    uint8_t inst = 0;
    CHECK(rig.core.addInstance(fx::kTypeId, kCtl, "a", inst).result == Result::Armed);
    CHECK(rig.core.removeInstance(fx::kTypeId, 0).result == Result::Armed);
    CHECK(rig.core.setLabel(fx::kTypeId, 0, "a").result == Result::Armed);
    CHECK(rig.core.discardPending().result == Result::Armed);
    rig.core.params().setArmed(false);

    reset();
    core::Core c(testConfig());
    REQUIRE(c.init(kTypes).ok());
    CHECK(c.addInstance(fx::kTypeId, kCtl, "a", inst).detail == Detail::NotAtBoot);
}

TEST_CASE("Layout: an entry that was already invalid does not block other ops")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    // Pin 13 is not allowed (the policy changed since it was saved).
    bootWith(c, store, {{act::kTypeId, 0, kCtl, "a", {{act::kPin, 13}}}});
    CHECK(stateOf(c, act::kTypeId, 0) == core::InstanceState::Invalid);
    uint8_t inst = 0;
    REQUIRE(c.addInstance(fx::kTypeId, kCtl, "b", inst).ok());
    // Fixing it is a pending write, and a write that leaves it broken is refused.
    std::array<uint32_t, 1> applied{};
    const core::SetEntry still_bad = entry(act::kTypeId, 0, act::kPin, 45);
    CHECK(c.params().set({&still_bad, 1}, applied).detail == Detail::GpioNotAllowed);
    const core::SetEntry fix = entry(act::kTypeId, 0, act::kPin, 12);
    CHECK(c.params().set({&fix, 1}, applied).accepted());
}

TEST_CASE("Layout validation and wiring order, directly")
{
    uint32_t wired_vals[16] = {};
    // fixture.wired: feedback (item 0) → entry 2, actuator (item 1) → entry 0.
    wired_vals[0] = wire(fb::kTypeId, 0);
    wired_vals[1] = wire(act::kTypeId, 0);
    wired_vals[4] = defs::kNoGpio;
    wired_vals[5] = defs::kNoGpio;
    uint32_t act_vals[4] = {defs::kNoGpio};
    uint32_t fb_vals[4] = {defs::kNoGpio};
    uint32_t loop_values[9] = {1000, 1, 1, 20, 4096};
    const std::vector<core::LayoutEntry> entries{
        {&act::kDesc, act::kTypeId, 0, kCtl, "a", act_vals},
        {&wd::kDesc, wd::kTypeId, 0, kCtl, "w", wired_vals},
        {&fb::kDesc, fb::kTypeId, 0, kCtl, "f", fb_vals},
        {&lp::kDesc, defs::kLoopTypeId, 0, core::kNoLoop, "control", loop_values},
    };
    std::vector<core::Status> st(4);
    CHECK(core::validateLayout(entries, {kTestGpioMask, 16}, st) == 0);
    const std::vector<bool> all{true, true, true, false};
    CHECK(core::creationOrder(entries, all) == std::vector<size_t>{0, 2, 1});
    CHECK(core::tickOrder(entries, all) == std::vector<size_t>{2, 1, 0});
    // Leaving the feedback out drops its link.
    const std::vector<bool> some{true, true, false, false};
    CHECK(core::tickOrder(entries, some) == std::vector<size_t>{1, 0});

    // An unknown type in the middle: the entry wired to it misses a dependency.
    std::vector<core::LayoutEntry> broken = entries;
    broken[2].desc = nullptr;
    CHECK(core::validateLayout(broken, {kTestGpioMask, 16}, st) == 2);
    CHECK(st[2].detail == Detail::UnknownType);
    CHECK(st[1].detail == Detail::BadRef); // its target is of no known category
    CHECK(st[0].ok());
}
