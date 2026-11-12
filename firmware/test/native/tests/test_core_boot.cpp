// core: the boot. Creation and tick order from the wiring, ctx.ref(), failed
// and invalid instances and what is wired to them, offline param writes.

#include "doctest.h"

#include <array>
#include <vector>

#include "core/core.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"

using namespace testmod;
using core::Detail;
using core::InstanceState;
using core::Result;

namespace {

namespace sys = drivelink::protocol::defs::system;
namespace lp = drivelink::protocol::defs::loop;
namespace codes = drivelink::protocol::defs::diag::codes;
constexpr uint8_t kSys = drivelink::protocol::defs::kSystemTypeId;
constexpr uint8_t kCtl = core::kFactoryControlLoop;
constexpr uint8_t kSen = core::kFactorySensorLoop;

/// Reports of `code`, in order.
std::vector<core_os::native::RecordedReport> reportsOf(const drivelink::protocol::defs::diag::Code &code)
{
    std::vector<core_os::native::RecordedReport> out;
    for (const auto &r : core_os::native::recordedReports()) {
        if (r.code.code == code.code && r.code.source == code.source) {
            out.push_back(r);
        }
    }
    return out;
}

/// An actuator, a controller and a feedback in one loop, the controller
/// listed between them, plus one more feedback and actuator pair in the other
/// order in the sensor loop.
std::vector<Inst> controllerLayout()
{
    return {
        {act::kTypeId, 0, kCtl, "act0", {{act::kPin, 4}}},
        {wd::kTypeId, 0, kCtl, "ctl0",
         {{wd::kFeedback, wire(fb::kTypeId, 0)}, {wd::kActuator, wire(act::kTypeId, 0)}, {wd::kGain, f(3.0f)}}},
        {fb::kTypeId, 0, kCtl, "fb0", {{fb::kPin, 5}}},
        {wd::kTypeId, 1, kSen, "ctl1", {{wd::kFeedback, wire(fb::kTypeId, 1)}, {wd::kActuator, wire(act::kTypeId, 1)}}},
        {fb::kTypeId, 1, kSen, "fb1"},
        {act::kTypeId, 1, kSen, "act1"},
    };
}

} // namespace

TEST_CASE("Boot: creation is dependencies first, the tick order feedback, controller, actuator")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    bootWith(c, store, controllerLayout());
    CHECK(c.instanceCount() == 8);
    CHECK(c.instancesFailed() == 0);
    CHECK_FALSE(c.layoutInvalid());
    CHECK(core_os::native::recordedReports().empty());

    const int a0 = key(act::kTypeId, 0), w0 = key(wd::kTypeId, 0), f0 = key(fb::kTypeId, 0);
    const int a1 = key(act::kTypeId, 1), w1 = key(wd::kTypeId, 1), f1 = key(fb::kTypeId, 1);
    // Stable: the lowest ready index first.
    CHECK(world().create_log == std::vector<int>{a0, f0, w0, f1, a1, w1});

    c.loop(kCtl).runTick(1);
    CHECK(world().tick_log == std::vector<int>{f0, w0, a0});
    world().tick_log.clear();
    c.loop(kSen).runTick(1);
    CHECK(world().tick_log == std::vector<int>{f1, w1, a1});
}

TEST_CASE("Boot: ctx.ref() gives the wired instance")
{
    reset();
    core_os::KvStore store;
    core::Core c(testConfig());
    bootWith(c, store, controllerLayout());
    auto *w0 = static_cast<WiredModule *>(world().objects.at(key(wd::kTypeId, 0)));
    CHECK(w0->feedback_obj == world().objects.at(key(fb::kTypeId, 0)));
    CHECK(w0->actuator_obj == world().objects.at(key(act::kTypeId, 0)));
    CHECK(w0->peer_obj == nullptr);   // unwired
    CHECK(w0->not_a_ref == nullptr);  // gain is no ref
    CHECK(w0->gain == 3.0f);
    CHECK(w0->feedback == wire(fb::kTypeId, 0));
}

TEST_CASE("Boot: a failed instance leaves what is wired to it out, transitively, and its config survives SAVE")
{
    reset();
    core_os::KvStore store;
    std::vector<Inst> layout = controllerLayout();
    // ctl2 is wired (peer) to ctl0, which is wired to fb0.
    layout.push_back({wd::kTypeId, 2, kCtl, "ctl2", {{wd::kPeer, wire(wd::kTypeId, 0)}, {wd::kGain, f(7.0f)}}});
    writeLayout(store, layout);
    {
        core::Core c(testConfig());
        world().fail_start.push_back(key(fb::kTypeId, 0));
        REQUIRE(bootCore(c, store).ok());
        CHECK(stateOf(c, fb::kTypeId, 0) == InstanceState::Failed);
        CHECK(stateOf(c, wd::kTypeId, 0) == InstanceState::DependencyMissing);
        CHECK(stateOf(c, wd::kTypeId, 2) == InstanceState::DependencyMissing);
        CHECK(stateOf(c, act::kTypeId, 0) == InstanceState::Running); // a dependency, not a dependent
        CHECK(c.instanceCount() == 6);
        CHECK(c.instancesFailed() == 3);
        CHECK_FALSE(c.layoutInvalid());

        CHECK(reportsOf(codes::core::kInstanceStartFailed).size() == 1);
        const auto missing = reportsOf(codes::core::kDependencyMissing);
        REQUIRE(missing.size() == 2);
        CHECK(missing[0].arg0 == static_cast<uint32_t>(key(wd::kTypeId, 0)));
        CHECK(missing[0].arg1 == wire(fb::kTypeId, 0));
        CHECK(missing[1].arg0 == static_cast<uint32_t>(key(wd::kTypeId, 2)));
        CHECK(missing[1].arg1 == wire(wd::kTypeId, 0));

        // The control loop ticks only what runs.
        c.loop(kCtl).runTick(1);
        CHECK(world().tick_log == std::vector<int>{key(act::kTypeId, 0)});

        // The config of what didn't start is kept, and SAVE writes it.
        CHECK(getParam(c, wd::kTypeId, 2, wd::kGain) == f(7.0f));
        REQUIRE(c.save().ok());
    }
    reset();
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK(c.instanceCount() == 9);
    CHECK(getParam(c, wd::kTypeId, 2, wd::kGain) == f(7.0f));
    CHECK(getParam(c, fb::kTypeId, 0, fb::kPin) == 5);
    auto *w2 = static_cast<WiredModule *>(world().objects.at(key(wd::kTypeId, 2)));
    CHECK(w2->peer_obj == world().objects.at(key(wd::kTypeId, 0)));
}

TEST_CASE("Boot: an invalid layout entry is left out and sets layout_invalid")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {
                           {0x66, 0, kCtl, "gone"},                          // a type this firmware lacks
                           {act::kTypeId, 0, kCtl, "act0", {{act::kPin, 13}}}, // a pin the policy no longer allows
                           {wd::kTypeId, 0, kCtl, "ctl0", {{wd::kActuator, wire(act::kTypeId, 0)}}},
                           {fb::kTypeId, 0, kCtl, "fb0"},
                       });
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK(c.layoutInvalid());
    CHECK(c.findRecord(0x66, 0) == nullptr);
    CHECK(stateOf(c, act::kTypeId, 0) == InstanceState::Invalid);
    CHECK(stateOf(c, wd::kTypeId, 0) == InstanceState::DependencyMissing);
    CHECK(stateOf(c, fb::kTypeId, 0) == InstanceState::Running);
    CHECK(c.instanceCount() == 3);
    CHECK(c.instancesFailed() == 3);
    CHECK(world().create_log == std::vector<int>{key(fb::kTypeId, 0)});

    const auto invalid = reportsOf(codes::core::kLayoutInvalid);
    REQUIRE(invalid.size() == 2);
    CHECK(invalid[0].arg0 == 0x6600);
    CHECK(invalid[0].arg1 == static_cast<uint32_t>(Detail::UnknownType));
    CHECK(invalid[1].arg0 == static_cast<uint32_t>(key(act::kTypeId, 0)));
    CHECK(invalid[1].arg1 == static_cast<uint32_t>(Detail::GpioNotAllowed));
    const auto missing = reportsOf(codes::core::kDependencyMissing);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].arg1 == wire(act::kTypeId, 0));
    // The invalid entry's config is there to fix.
    CHECK(getParam(c, act::kTypeId, 0, act::kPin) == 13);
}

TEST_CASE("Boot: labels are unique, and the system params load before the loops start")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {{fx::kTypeId, 0, kCtl, "same"}, {fx::kTypeId, 1, kCtl, "same"}, {fx::kTypeId, 2, kCtl, ""},
                        {defs::kLoopTypeId, 1, core::kNoLoop, "sensor", {{lp::kRateHz, 50}}}});
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK(stateOf(c, fx::kTypeId, 1) == InstanceState::Invalid);
    CHECK(stateOf(c, fx::kTypeId, 2) == InstanceState::Invalid);
    const auto invalid = reportsOf(codes::core::kLayoutInvalid);
    REQUIRE(invalid.size() == 2);
    CHECK(invalid[0].arg1 == static_cast<uint32_t>(Detail::LabelInUse));
    CHECK(invalid[1].arg1 == static_cast<uint32_t>(Detail::BadLabel));
    CHECK(c.loop(kSen).periodUs() == 20000);
    REQUIRE(c.start().ok());
    CHECK(c.isRunning());
    c.stop();
}

TEST_CASE("Boot: a param set on an instance that isn't running goes to its shadow")
{
    reset();
    core_os::KvStore store;
    writeLayout(store, {{fx::kTypeId, 0, kCtl, "a"}, {fx::kTypeId, 1, kCtl, "b"}});
    world().mode = StartMode::FailStage; // both fail
    {
        core::Core c(testConfig());
        REQUIRE(bootCore(c, store).ok());
        CHECK(stateOf(c, fx::kTypeId, 0) == InstanceState::Failed);
        CHECK_FALSE(c.nvsDirty());

        std::array<uint32_t, 2> applied{};
        const std::array<core::SetEntry, 2> batch{entry(fx::kTypeId, 0, fx::kGain, f(5000.0f)),
                                                  entry(fx::kTypeId, 1, fx::kLevel, 9)};
        core::PendingSet p;
        REQUIRE(c.params().stage(batch, applied, p).ok());
        CHECK_FALSE(p.active()); // done on the caller
        CHECK(applied[0] == f(1000.0f));
        CHECK(getParam(c, fx::kTypeId, 0, fx::kGain) == f(1000.0f));
        CHECK(getParam(c, fx::kTypeId, 1, fx::kLevel) == 9);
        CHECK(c.nvsDirty());

        // Not with a system param, and the rules still apply.
        const std::array<core::SetEntry, 2> mixed{entry(fx::kTypeId, 0, fx::kGain, f(1.0f)),
                                                  entry(defs::kLoopTypeId, 1, lp::kRateHz, 50)};
        CHECK(c.params().set(mixed, applied).detail == Detail::CrossLoopBatch);
        const core::SetEntry ro = entry(fx::kTypeId, 0, fx::kCount, 1);
        CHECK(c.params().set({&ro, 1}, applied).detail == Detail::ReadOnly);
        c.params().setArmed(true);
        const core::SetEntry locked = entry(fx::kTypeId, 0, fx::kMode, 2);
        CHECK(c.params().set({&locked, 1}, applied).result == Result::Armed);
        c.params().setArmed(false);
        REQUIRE(c.save().ok());
    }
    reset();
    core::Core c(testConfig());
    REQUIRE(bootCore(c, store).ok());
    CHECK(c.instanceCount() == 4);
    CHECK(world().created[0]->gain == 1000.0f);
    CHECK(world().created[1]->level == 9);
}
