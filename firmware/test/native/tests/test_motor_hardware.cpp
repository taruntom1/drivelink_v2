// actuator.pwm_dir and feedback.quadrature against the fake driver backend
// (fake_motor_hw.cpp). Ticks run through Loop::runTick, so the real-time
// guard covers every tick, every arm transition and every driver call in it.
#include "doctest.h"
#include "fake_motor_hw.hpp"
#include "core/core.hpp"
#include "core/persistence.hpp"
#include "core_os/clock.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/native_task.hpp"
#include "core_os/realtime.hpp"
#include "diag_table.hpp"
#include "mod_motor/modules.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/gen/type_actuator_pwm_dir.hpp"
#include "drivelink/protocol/gen/type_feedback_quadrature.hpp"
#include <climits>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
namespace defs = drivelink::protocol::defs;
namespace codes = defs::diag::codes;
namespace pwm = defs::actuator_pwm_dir;
namespace quad = defs::feedback_quadrature;
using fake_motor_hw::outputs;
using fake_motor_hw::counters;
constexpr uint8_t kPwm = pwm::kTypeId, kQuad = quad::kTypeId, kPid = 0x20;
constexpr float kTwoPi = 6.28318530717958647692F;

struct Manual {
    Manual() {
        core_os::native::setManualScheduling(true); core_os::native::resetRecordedDiag();
        core_os::native::resetBootInfo(); fake_motor_hw::reset();
    }
    ~Manual() { core_os::native::setManualScheduling(false); core_os::native::resetRecordedDiag(); }
};

struct Options {
    uint8_t pwm_pin = 4, dir_pin = 5, pin_a = 6, pin_b = 7;
    bool controller = true;
    core_os::Err pwm_open_error = 0, quadrature_open_error = 0;
};

/// loop 0 (1 kHz, real-time), actuator.pwm_dir 0, feedback.quadrature 0 and,
/// optionally, controller.cascade_pid 0 wired to both. The fake plant is a
/// first-order motor: 10 rad/s per unit duty, tau 0.1 s, 1440 counts/rev.
struct Hw {
    Manual manual;
    core_os::KvStore store;
    core::Core c{core::CoreConfig{}};
    const std::array<const core::ModuleType *, 3> types{
        mod_motor::kTypes[0], mod_motor::kHardwareTypes[0], mod_motor::kHardwareTypes[1]};
    double plant_velocity = 0, plant_position = 0;

    explicit Hw(Options o = {}) {
        std::vector<core::persist::StoredInstance> layout(o.controller ? 4 : 3);
        const uint8_t type_ids[]{defs::kLoopTypeId, kPwm, kQuad, kPid};
        for (size_t i = 0; i < layout.size(); ++i) {
            layout[i].type_id = type_ids[i]; layout[i].instance = 0; layout[i].loop = i == 0 ? 255 : 0;
            std::snprintf(layout[i].label, sizeof(layout[i].label), "h%u", static_cast<unsigned>(i));
        }
        layout[0].pairs = {{1, 1000}, {2, 1}};
        layout[1].pairs = {{pwm::kPwmPin, o.pwm_pin}, {pwm::kDirPin, o.dir_pin}};
        layout[2].pairs = {{quad::kPinA, o.pin_a}, {quad::kPinB, o.pin_b}};
        if (o.controller) {
            layout[3].pairs = {{1, uint32_t{kPwm} << 8}, {2, uint32_t{kQuad} << 8},
                {32, defs::f32Bits(.12F)}, {33, defs::f32Bits(.8F)}, {48, defs::f32Bits(3.F)}, {49, defs::f32Bits(2.F)}};
        }
        REQUIRE(core::persist::saveConfig(store, 1, layout, {}).ok());
        fake_motor_hw::fail_next_pwm_open = o.pwm_open_error;
        fake_motor_hw::fail_next_quadrature_open = o.quadrature_open_error;
        REQUIRE(c.init(types).ok()); REQUIRE(c.boot(store).ok());
        if (o.controller && !c.instancesFailed()) {
            std::array<core::Ref, 3> inputs{{{kPid, 0, 514}, {kPid, 0, 512}, {kPid, 0, 513}}};
            REQUIRE(c.groups().stageConfig(core::GroupKind::Command, 0, 0, 1, inputs).ok());
            tick(); REQUIRE(c.groups().complete(core::GroupKind::Command, 0, 0).ok());
        }
        REQUIRE(c.start().ok());
    }
    /// One loop tick, then the plant integrates the duty the pins drive.
    void tick(unsigned n = 1) {
        for (unsigned i = 0; i < n; ++i) {
            c.loop(0).runTick(1);
            const double dt = .001, target = 10.0 * (outputs[0].open ? outputs[0].effective() : 0.F);
            plant_velocity += (target - plant_velocity) * dt / .1;
            plant_position += plant_velocity * dt;
            counters[0].count = static_cast<int32_t>(std::lround(plant_position * 1440.0 / kTwoPi));
        }
    }
    float get(uint8_t type, uint16_t id) const {
        const auto *r = c.findRecord(type, 0); const auto i = r->itemIndex(id);
        REQUIRE(i != core::detail::kNoItem);
        const auto bits = core::loadValue(r->desc->items[i].type, r->bindings[i].ptr);
        switch (r->desc->items[i].type) {
        case defs::ValueType::F32: return defs::f32FromBits(bits);
        case defs::ValueType::I32: return static_cast<float>(static_cast<int32_t>(bits));
        default: return static_cast<float>(bits);
        }
    }
    int32_t count() const {
        const auto *r = c.findRecord(kQuad, 0);
        return static_cast<int32_t>(core::loadValue(defs::ValueType::I32, r->bindings[r->itemIndex(quad::kCount)].ptr));
    }
    void set(uint8_t type, uint16_t id, uint32_t bits) {
        const core::SetEntry entry{{type, 0, id}, bits}; uint32_t applied;
        core::PendingSet pending;
        REQUIRE(c.params().stage({&entry, 1}, {&applied, 1}, pending).ok());
        c.loop(0).runTick(1); REQUIRE(c.params().complete(pending, 0).ok());
    }
    bool send(float duty, float velocity = 0, float position = 0, int64_t at = 0) {
        std::array<uint8_t, 16> bytes{}; drivelink::protocol::wire::Writer w(bytes.data(), bytes.size());
        w.appendU8(drivelink::protocol::core_wire::kVersion); w.appendU8(1); w.appendU16Le(0);
        w.appendU32Le(defs::f32Bits(duty)); w.appendU32Le(defs::f32Bits(velocity)); w.appendU32Le(defs::f32Bits(position));
        return c.groups().receiveCommand(0, {bytes.data(), w.size()}, at ? at : core_os::nowUs() + 1);
    }
    void arm() { REQUIRE(c.arm().ok()); tick(); }
    core::Instance &actuator() { return *c.findRecord(kPwm, 0)->object; }
};

/// Whether `code` is active for `instance`. Sparse slots are stable for the
/// whole test binary and can run out; a code then lives in the safety
/// overflow bits, which only the aggregate query reads (each test starts
/// with no active safety code).
bool active(defs::diag::Code code, uint8_t instance = 0) {
    auto &table = core_os::native::diagTable();
    const auto slot = table.resolve(code.slot, instance);
    if (slot >= diag::detail::kRuntimeSlots) { return core_os::diagDisarmsActive(); }
    return (table.read(slot).state & diag::protocol::kStateActive) != 0;
}

/// The type's own InitFailed records, which the core sends in place of
/// core.InstanceStartFailed: arg0 is the stage, the instance is explicit.
std::vector<core_os::native::RecordedReport> startFailures(defs::diag::Code code) {
    std::vector<core_os::native::RecordedReport> out;
    for (const auto &r : core_os::native::recordedReports()) {
        if (r.code.source == code.source && r.code.code == code.code) { out.push_back(r); }
    }
    return out;
}
} // namespace

TEST_CASE("PWM+DIR output is forced from creation, gated while disarmed and released only by ARM") {
    Hw f; auto &o = outputs[0];
    REQUIRE(fake_motor_hw::output_count == 1);
    CHECK(o.open); CHECK(o.forced); CHECK(o.pwm_pin == 4); CHECK(o.dir_pin == 5);
    REQUIRE(f.send(.3F)); f.tick(3);
    CHECK(o.forced); CHECK(o.duty == 0); CHECK(o.releases == 0);
    CHECK(f.get(kPwm, pwm::kForced) == 1); CHECK(f.get(kPwm, pwm::kAppliedDuty) == 0);
    f.arm();
    CHECK_FALSE(o.forced); CHECK(o.releases == 1); CHECK(f.get(kPwm, pwm::kForced) == 0);
    REQUIRE(f.send(.3F)); f.tick();
    CHECK(o.duty == doctest::Approx(.3F)); CHECK(f.get(kPwm, pwm::kAppliedDuty) == doctest::Approx(.3F));
    f.c.disarm(); f.tick();
    CHECK(o.forced); CHECK(o.duty == 0); CHECK(f.get(kPwm, pwm::kAppliedDuty) == 0); CHECK(f.get(kPwm, pwm::kForced) == 1);
}

TEST_CASE("The actuator caps duty, applies invert and passes a reversal through one zero tick") {
    Hw f; auto &o = outputs[0];
    f.set(kPwm, pwm::kDutyLimit, defs::f32Bits(.2F));
    f.set(kPwm, pwm::kInvert, 1);
    f.arm();
    REQUIRE(f.send(.5F)); f.tick();
    CHECK(f.get(kPwm, pwm::kAppliedDuty) == doctest::Approx(.2F)); CHECK(o.duty == doctest::Approx(-.2F));
    REQUIRE(f.send(-.5F)); f.tick();
    CHECK(f.get(kPwm, pwm::kAppliedDuty) == 0); CHECK(o.duty == 0);
    REQUIRE(f.send(-.5F)); f.tick();
    CHECK(f.get(kPwm, pwm::kAppliedDuty) == doctest::Approx(-.2F)); CHECK(o.duty == doctest::Approx(.2F));
}

TEST_CASE("A request alternating sign every tick writes zero instead of a one-sided bias") {
    Hw f; auto &o = outputs[0];
    f.arm();
    REQUIRE(f.send(.3F)); f.tick(); CHECK(o.duty == doctest::Approx(.3F));
    for (int i = 0; i < 6; ++i) {
        REQUIRE(f.send(i % 2 ? .3F : -.3F)); f.tick();
        CHECK(o.duty == 0);
    }
    REQUIRE(f.send(.3F)); f.tick(); CHECK(o.duty == doctest::Approx(.3F)); // a steady request passes
}

TEST_CASE("The monitor's force stops the output without a loop transition and holds until re-ARM") {
    Hw f; auto &o = outputs[0];
    f.arm(); REQUIRE(f.send(.3F)); f.tick();
    REQUIRE(o.effective() == doctest::Approx(.3F));
    SUBCASE("a direct emergencyStop") {
        { core_os::RealtimeScope rt(true); f.actuator().emergencyStop(); }
        CHECK(o.forced); CHECK(o.effective() == 0);
        // The core is still armed and commands still arrive: the gate holds.
        REQUIRE(f.send(.3F)); f.tick();
        CHECK(o.duty == 0); CHECK(f.get(kPwm, pwm::kForced) == 1);
        f.c.disarm(); f.tick(); f.arm();
        REQUIRE(f.send(.3F)); f.tick();
        CHECK_FALSE(o.forced); CHECK(o.duty == doctest::Approx(.3F));
    }
    SUBCASE("a watchdog trip forces before any loop tick") {
        const auto base = core_os::nowUs(); f.c.monitor(base);
        f.c.loop(0).runTick(1); // the heartbeat advances: no stall, only the watchdog
        { core_os::RealtimeScope rt(true); f.c.monitor(base + 200001); }
        CHECK(f.c.armStatus() == core::ArmStatus::Disarmed);
        CHECK(f.c.lastDisarmReason() == core::DisarmReason::Watchdog);
        CHECK(o.forced); CHECK(o.effective() == 0);
        f.tick(); CHECK(o.duty == 0);
    }
}

TEST_CASE("A monitor stop racing the ARM release is not undone by it") {
    Hw f; auto &o = outputs[0];
    // The monitor forces after the loop read the stop epoch, and its force
    // lands before the release does: the loop must stop again.
    o.during_release = [](void *a) {
        core_os::RealtimeScope rt(true);
        static_cast<core::Instance *>(a)->emergencyStop();
    };
    o.during_release_ctx = &f.actuator();
    f.arm();
    CHECK(o.releases == 1);
    CHECK(o.forced); CHECK(f.get(kPwm, pwm::kForced) == 1);
    REQUIRE(f.send(.3F)); f.tick();
    CHECK(o.duty == 0); CHECK(o.effective() == 0);
    // A later ARM without the race releases normally.
    f.c.disarm(); f.tick(); f.arm();
    REQUIRE(f.send(.3F)); f.tick();
    CHECK_FALSE(o.forced); CHECK(o.duty == doctest::Approx(.3F));
}

TEST_CASE("Concurrent monitor stops and loop ticks leave no effort after the stop") {
    Hw f; auto &o = outputs[0]; auto &a = f.actuator();
    f.arm();
    std::atomic<bool> go{false};
    std::thread monitor([&] {
        while (!go.load()) {}
        core_os::RealtimeScope rt(true);
        for (int i = 0; i < 2000; ++i) { a.emergencyStop(); }
    });
    go = true;
    for (int i = 0; i < 2000; ++i) { REQUIRE(f.send(.3F)); f.tick(); }
    monitor.join();
    REQUIRE(f.send(.3F)); f.tick();
    CHECK(o.forced); CHECK(o.duty == 0); CHECK(o.effective() == 0);
}

TEST_CASE("Driver errors raise a latched disarming DriverFault and force the output") {
    Hw f; auto &o = outputs[0];
    f.arm(); REQUIRE(f.send(.3F)); f.tick();
    SUBCASE("a failed write") {
        o.fail_write = true; REQUIRE(f.send(.3F)); f.tick();
        CHECK(active(codes::actuator_pwm_dir::kDriverFault)); CHECK(o.forced);
        CHECK(f.c.armStatus() == core::ArmStatus::Fault);
        CHECK(f.c.acknowledgeFault().detail == core::Detail::FaultActive);
        o.fail_write = false; f.tick();
        CHECK_FALSE(active(codes::actuator_pwm_dir::kDriverFault));
        REQUIRE(f.c.acknowledgeFault().ok());
        f.arm(); REQUIRE(f.send(.3F)); f.tick(); CHECK(o.effective() == doctest::Approx(.3F));
    }
    SUBCASE("a failed release on ARM keeps the output forced") {
        f.c.disarm(); f.tick();
        o.fail_release = true; REQUIRE(f.c.arm().ok()); f.tick();
        // Raised by the transition, cleared by the same tick's successful
        // zero write: the latched Fault remains, and ARM can be retried
        // after ACK_FAULT instead of being blocked until a reboot.
        CHECK(f.c.armStatus() == core::ArmStatus::Fault); CHECK(o.forced);
        CHECK(f.get(kPwm, pwm::kForced) == 1);
        CHECK_FALSE(active(codes::actuator_pwm_dir::kDriverFault));
        REQUIRE(f.send(.3F)); f.tick(); CHECK(o.effective() == 0);
        o.fail_release = false; REQUIRE(f.c.acknowledgeFault().ok()); f.arm();
        CHECK_FALSE(o.forced);
    }
    SUBCASE("a single failed monitor force clears once a later force succeeds") {
        o.fail_force = true;
        { core_os::RealtimeScope rt(true); f.actuator().emergencyStop(); }
        o.fail_force = false;
        CHECK(active(codes::actuator_pwm_dir::kDriverFault));
        CHECK(f.c.armStatus() == core::ArmStatus::Fault);
        f.tick(); // the disarm transition forces again, successfully
        f.tick();
        CHECK_FALSE(active(codes::actuator_pwm_dir::kDriverFault));
        REQUIRE(f.c.acknowledgeFault().ok());
        f.arm(); REQUIRE(f.send(.3F)); f.tick(); CHECK(o.effective() == doctest::Approx(.3F));
    }
    SUBCASE("a failed monitor force stays active until a force succeeds") {
        o.fail_force = true;
        { core_os::RealtimeScope rt(true); f.actuator().emergencyStop(); }
        CHECK(active(codes::actuator_pwm_dir::kDriverFault));
        f.tick(); CHECK(active(codes::actuator_pwm_dir::kDriverFault));
        CHECK(o.duty == 0); // the gate holds even though the hardware force failed
        o.fail_force = false;
        { core_os::RealtimeScope rt(true); f.actuator().emergencyStop(); }
        f.tick(); CHECK_FALSE(active(codes::actuator_pwm_dir::kDriverFault)); CHECK(o.forced);
    }
}

TEST_CASE("Start failures report their stage and take no hardware") {
    const auto actuatorInit = codes::actuator_pwm_dir::kInitFailed;
    const auto encoderInit = codes::feedback_quadrature::kInitFailed;
    SUBCASE("an actuator pin left unset") {
        Hw f(Options{.pwm_pin = defs::kNoGpio});
        CHECK(f.c.instancesFailed()); CHECK(fake_motor_hw::output_count == 0);
        const auto failures = startFailures(actuatorInit); REQUIRE(failures.size() == 1);
        CHECK(failures[0].arg0 == 1); CHECK(failures[0].err == -1); CHECK(failures[0].instance == 0);
        CHECK_FALSE(f.c.arm().ok());
    }
    SUBCASE("an actuator driver failure carries its error") {
        Hw f(Options{.pwm_open_error = 0x105});
        CHECK(fake_motor_hw::output_count == 0);
        const auto failures = startFailures(actuatorInit); REQUIRE(failures.size() == 1);
        CHECK(failures[0].arg0 == 2); CHECK(failures[0].err == 0x105);
    }
    SUBCASE("an encoder pin left unset") {
        Hw f(Options{.pin_b = defs::kNoGpio});
        CHECK(fake_motor_hw::counter_count == 0);
        const auto failures = startFailures(encoderInit); REQUIRE(failures.size() == 1);
        CHECK(failures[0].arg0 == 1); CHECK(failures[0].err == -1);
    }
    SUBCASE("an encoder driver failure carries its error") {
        Hw f(Options{.quadrature_open_error = 0x105});
        const auto failures = startFailures(encoderInit); REQUIRE(failures.size() == 1);
        CHECK(failures[0].arg0 == 2); CHECK(failures[0].err == 0x105);
        CHECK_FALSE(f.c.arm().ok());
    }
}

TEST_CASE("Destroying the core leaves the output forced and released") {
    {
        Hw f; f.arm(); REQUIRE(f.send(.3F)); f.tick();
        REQUIRE(outputs[0].effective() == doctest::Approx(.3F));
    }
    CHECK_FALSE(outputs[0].open); CHECK(outputs[0].forced);
}

TEST_CASE("Quadrature feedback scales, inverts across the int32 range and keeps its count across arm transitions") {
    Hw f(Options{.controller = false});
    REQUIRE(fake_motor_hw::counter_count == 1);
    CHECK(counters[0].pin_a == 6); CHECK(counters[0].pin_b == 7); CHECK(counters[0].pullup);
    f.tick(); // the plant (no controller, zero duty) keeps the count at 0
    counters[0].count = 1440; f.c.loop(0).runTick(1);
    CHECK(f.count() == 1440); CHECK(f.get(kQuad, quad::kPosition) == doctest::Approx(kTwoPi));
    f.set(kQuad, quad::kInvert, 1);
    counters[0].count = 5; f.c.loop(0).runTick(1); CHECK(f.count() == -5);
    counters[0].count = INT32_MIN; f.c.loop(0).runTick(1); CHECK(f.count() == INT32_MIN);
    counters[0].count = INT32_MAX; f.c.loop(0).runTick(1); CHECK(f.count() == -INT32_MAX);
    CHECK(std::isfinite(f.get(kQuad, quad::kVelocity)));
    f.c.disarm(); f.c.loop(0).runTick(1); CHECK(f.count() == -INT32_MAX);
}

TEST_CASE("A failed encoder read reports NaN, raises ReadFailed and the controller drives zero") {
    Hw f; auto &o = outputs[0];
    f.arm(); REQUIRE(f.send(.3F)); f.tick();
    REQUIRE(o.duty == doctest::Approx(.3F));
    counters[0].fail_read = true; REQUIRE(f.send(.3F)); f.c.loop(0).runTick(1);
    CHECK(std::isnan(f.get(kQuad, quad::kPosition))); CHECK(std::isnan(f.get(kQuad, quad::kVelocity)));
    CHECK(o.duty == 0);
    CHECK(active(codes::feedback_quadrature::kReadFailed));
    CHECK(f.c.armStatus() == core::ArmStatus::Fault);
    counters[0].fail_read = false; f.c.loop(0).runTick(1);
    CHECK_FALSE(active(codes::feedback_quadrature::kReadFailed));
    CHECK(std::isfinite(f.get(kQuad, quad::kPosition)));
    REQUIRE(f.c.acknowledgeFault().ok());
}

TEST_CASE("Real module types close velocity and position loops on a fake plant") {
    Hw f;
    f.set(kPid, 16, 1); // velocity mode, set while disarmed
    f.arm();
    for (int i = 0; i < 3000; ++i) { REQUIRE(f.send(0, 2)); f.tick(); }
    CHECK(f.get(kQuad, quad::kVelocity) == doctest::Approx(2).epsilon(.1));
    CHECK(f.plant_velocity == doctest::Approx(2).epsilon(.1));
    f.c.disarm(); f.tick(); CHECK(outputs[0].forced);
    f.set(kPid, 16, 2); // position mode
    f.arm();
    const float target = f.get(kQuad, quad::kPosition) + 1.F;
    for (int i = 0; i < 4000; ++i) { REQUIRE(f.send(0, 0, target)); f.tick(); }
    CHECK(f.get(kQuad, quad::kPosition) == doctest::Approx(target).epsilon(.02));
}
