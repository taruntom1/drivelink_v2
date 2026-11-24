#include "doctest.h"
#include "core/core.hpp"
#include "core/command_service.hpp"
#include "core/persistence.hpp"
#include "core_os/clock.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/native_task.hpp"
#include "core_os/realtime.hpp"
#include "diag_observer.hpp"
#include "diag_table.hpp"
#include "mod_mock/modules.hpp"
#include "drivelink/protocol/gen/type_system.hpp"
#include <thread>

namespace {
namespace defs = drivelink::protocol::defs;
namespace cw = drivelink::protocol::core_wire;
namespace codes = defs::diag::codes;
struct Manual {
    Manual() { core_os::native::setManualScheduling(true); core_os::native::resetRecordedDiag(); core_os::native::resetBootInfo(); }
    ~Manual() { core_os::native::setManualScheduling(false); core_os::native::resetRecordedDiag(); }
};
struct Safety {
    Manual manual;
    core_os::KvStore store;
    core::Core c;
    Safety(bool command = true, core::CoreConfig config = {}) : c(config) {
        std::array<core::persist::StoredInstance, 3> layout{};
        layout[0].type_id = defs::kLoopTypeId; layout[0].instance = 0; layout[0].loop = 255;
        std::strcpy(layout[0].label, "control"); layout[0].pairs = {{1,1000}, {2,1}};
        layout[1].type_id = defs::kLoopTypeId; layout[1].instance = 19; layout[1].loop = 255;
        std::strcpy(layout[1].label, "background"); layout[1].pairs = {{1,100}, {2,0}};
        layout[2].type_id = 112; layout[2].instance = 0; layout[2].loop = 0; std::strcpy(layout[2].label, "motor");
        REQUIRE(core::persist::saveConfig(store,1,layout,{}).ok());
        REQUIRE(c.init(mod_mock::kTypes).ok()); REQUIRE(c.boot(core::prepareBoot(store)).ok());
        if (command) {
            const core::Ref input{112,0,512};
            REQUIRE(c.groups().stageConfig(core::GroupKind::Command,0,0,1,{&input,1}).ok());
            c.loop(0).runTick(1); REQUIRE(c.groups().complete(core::GroupKind::Command,0,0).ok());
        }
        REQUIRE(c.start().ok());
    }
    void ticks() { c.loop(0).runTick(1); c.loop(19).runTick(1); }
    bool send(int64_t at, float value, bool keepalive = false, uint8_t epoch = 1) {
        std::array<uint8_t,8> bytes{};
        drivelink::protocol::wire::Writer w(bytes.data(),bytes.size());
        w.appendU8(cw::kVersion); w.appendU8(epoch); w.appendU16Le(0);
        if (!keepalive) { w.appendU32Le(defs::f32Bits(value)); }
        return c.groups().receiveCommand(0,{bytes.data(),w.size()},at);
    }
    float value(uint16_t item) {
        const auto *record = c.findRecord(112,0);
        const auto index = record->itemIndex(item);
        return defs::f32FromBits(core::loadValue(defs::ValueType::F32, record->bindings[index].ptr));
    }
};
}

TEST_CASE("ARM tickets lose to every intervening disarm, even while disarmed") {
    core::ArmState arm;
    const auto ticket = arm.ticket(); arm.disarm();
    CHECK_FALSE(arm.commit(ticket,123));
    REQUIRE(arm.commit(arm.ticket(),456));
    uint32_t word; int64_t at;
    REQUIRE(arm.snapshot(word,at)); CHECK(at==456);
    for (unsigned i=0;i<100;++i) {
        core::ArmState racing; const auto before=racing.ticket();
        std::atomic<bool> go{false};
        std::thread fault([&] { while (!go.load()) { std::this_thread::yield(); } racing.disarm(core::DisarmReason::Fault,true); });
        go.store(true); (void)racing.commit(before,100); fault.join();
        CHECK(racing.status()==core::ArmStatus::Fault);
    }
}
TEST_CASE("An expired monitor observation cannot stop a freshly re-armed epoch") {
    core::ArmState arm; REQUIRE(arm.commit(arm.ticket(),100));
    const auto evaluated = arm.ticket(); // monitor observed this epoch at its deadline
    arm.disarm(); REQUIRE(arm.commit(arm.ticket(),500));
    CHECK_FALSE(arm.claimStop(evaluated)); CHECK(arm.status()==core::ArmStatus::Armed);
    REQUIRE(arm.claimStop(arm.ticket())); // current observations can claim a stop
    CHECK_FALSE(arm.claimStop(arm.ticket())); // a second owner cannot clear its gate
    arm.disarm(core::DisarmReason::Watchdog);
    CHECK_FALSE(arm.commit(arm.ticket(),600)); // force application still owns the gate
    arm.word.fetch_and(~core::ArmState::kStopping);
    REQUIRE(arm.commit(arm.ticket(),700));
}
TEST_CASE("ARM requires a current command group and all interlocks") {
    Safety f(false);
    CHECK(f.c.arm().detail==core::Detail::NoCommandGroup);
    REQUIRE(f.c.groups().stageConfig(core::GroupKind::Command,0,0,1,{}).ok());
    f.ticks(); REQUIRE(f.c.groups().complete(core::GroupKind::Command,0,0).ok());
    REQUIRE(f.c.arm().ok()); CHECK(f.c.params().armed());
    f.c.disarm();
    core_os::raiseDiag(codes::actuator_mock::kArmBlocked,0);
    CHECK(f.c.arm().detail==core::Detail::BlocksArm);
    core_os::clearDiag(codes::actuator_mock::kArmBlocked,0);
    REQUIRE(f.c.arm().ok()); f.c.disarm();
    uint8_t instance = 255; REQUIRE(f.c.addInstance(114,19,"new",instance).ok());
    CHECK(f.c.arm().detail==core::Detail::RebootPending);
}
TEST_CASE("Arming resets inputs and never replays pre-arm values through keepalives") {
    Safety f;
    REQUIRE(f.send(core_os::nowUs(),.8F)); REQUIRE(f.c.arm().ok());
    REQUIRE(f.send(core_os::nowUs()+1,0,true)); f.ticks(); CHECK(f.value(512)==0);
    REQUIRE(f.send(core_os::nowUs()+1,.8F)); f.ticks(); CHECK(f.value(512)==.8F);
    // Disarm and re-arm entirely between two loop ticks.
    REQUIRE(f.send(core_os::nowUs(),.5F)); f.c.disarm(); REQUIRE(f.c.arm().ok());
    REQUIRE(f.send(core_os::nowUs()+1,0,true)); f.ticks(); CHECK(f.value(512)==0);
    REQUIRE(f.send(core_os::nowUs()+1,.4F)); f.ticks(); CHECK(f.value(512)==.4F);
    f.c.disarm(); f.ticks(); CHECK(f.value(512)==0);
}
TEST_CASE("Disarming faults latch synchronously and ACK requires recovery") {
    Safety f; REQUIRE(f.c.arm().ok());
    { core_os::RealtimeScope rt(true); core_os::raiseDiag(codes::actuator_mock::kFaultInjected,0); }
    CHECK(f.c.armStatus()==core::ArmStatus::Fault); CHECK_FALSE(f.c.params().armed());
    CHECK(f.c.arm().detail==core::Detail::FaultLatched);
    CHECK(f.c.acknowledgeFault().detail==core::Detail::FaultActive);
    f.c.disarm(); CHECK(f.c.armStatus()==core::ArmStatus::Fault);
    { core_os::RealtimeScope rt(true); core_os::clearDiag(codes::actuator_mock::kFaultInjected,0); }
    CHECK(f.c.armStatus()==core::ArmStatus::Fault);
    REQUIRE(f.c.acknowledgeFault().ok()); REQUIRE(f.c.arm().ok());
    const core::SetEntry watchdog{{0,0,defs::system::kWatchdogMs},50}; uint32_t applied;
    CHECK(f.c.params().set({&watchdog,1},{&applied,1}).result==core::Result::Armed);
}
TEST_CASE("Record and counter occurrences also drive the synchronous disarm interlock") {
    Safety f;
    auto code = codes::core::kCommandDropped; code.disarms = true;
    REQUIRE(f.c.arm().ok());
    core_os::countDiag(code,0); CHECK(f.c.params().armed());
    { core_os::RealtimeScope rt(true); core_os::countDiag(code,1); }
    CHECK(f.c.armStatus()==core::ArmStatus::Fault);
    REQUIRE(f.c.acknowledgeFault().ok()); REQUIRE(f.c.arm().ok());
    code = codes::core::kInstanceStartFailed; code.disarms = true;
    core_os::reportDiag(code,0,0,0);
    CHECK(f.c.armStatus()==core::ArmStatus::Fault);
}
TEST_CASE("Active startup diagnostics seed the fault latch before loop gates open") {
    Manual manual; core_os::KvStore store; core::Core c(core::CoreConfig{});
    REQUIRE(c.init(mod_mock::kTypes).ok()); REQUIRE(c.boot(core::prepareBoot(store)).ok());
    core_os::raiseDiag(codes::actuator_mock::kFaultInjected,0);
    REQUIRE(c.start().ok());
    core_os::clearDiag(codes::actuator_mock::kFaultInjected,0);
    CHECK(c.armStatus()==core::ArmStatus::Fault); REQUIRE(c.acknowledgeFault().ok());
}
TEST_CASE("The monitor stops an actuator when its loop cannot process a disarm") {
    Safety f; REQUIRE(f.c.arm().ok()); REQUIRE(f.send(core_os::nowUs()+1,1)); f.ticks(); f.ticks();
    const float velocity = f.value(256); REQUIRE(velocity>0);
    const auto base=core_os::nowUs(); f.c.monitor(base);
    { core_os::RealtimeScope rt(true); f.c.monitor(base+9000); }
    CHECK(f.c.armStatus()==core::ArmStatus::Fault);
    CHECK(f.c.lastDisarmReason()==core::DisarmReason::LoopStalled);
    // Bypass Loop::runTick: no arm transition and the old duty is still 1.
    // The monitor's independent force must make the actuator decay instead.
    REQUIRE(f.value(512)==1);
    f.c.findRecord(112,0)->object->tick({base+10000,.01F,1});
    CHECK(f.value(256)<velocity);
    CHECK(f.c.acknowledgeFault().detail==core::Detail::FaultActive);
    f.ticks(); f.c.monitor(base+10000);
    REQUIRE(f.c.acknowledgeFault().ok()); CHECK(f.value(512)==0);
}
TEST_CASE("The monitor covers sparse non-real-time loops and recovers while disarmed") {
    Safety f; REQUIRE(f.c.arm().ok()); f.ticks();
    const auto base=core_os::nowUs(); f.c.monitor(base);
    f.c.loop(0).runTick(1); f.c.monitor(base+40000);
    CHECK(f.c.armStatus()==core::ArmStatus::Fault);
    auto &table=core_os::native::diagTable();
    CHECK((table.read(table.resolve(codes::core::kLoopStalled.slot,19)).state & diag::protocol::kStateActive) != 0);
    f.ticks(); f.c.monitor(base+41000); REQUIRE(f.c.acknowledgeFault().ok());
}
TEST_CASE("Watchdog uses only valid post-arm receipts, with an initial grace period") {
    Safety f; REQUIRE(f.send(core_os::nowUs(),.5F)); REQUIRE(f.c.arm().ok());
    const auto base=core_os::nowUs(); f.ticks(); f.c.monitor(base);
    SUBCASE("pre-arm and rejected frames do not feed") {
        REQUIRE_FALSE(f.send(base+199000,.5F,false,99));
        f.ticks(); f.c.monitor(base+200001);
        CHECK(f.c.armStatus()==core::ArmStatus::Disarmed);
        CHECK(f.c.lastDisarmReason()==core::DisarmReason::Watchdog);
        CHECK_FALSE(f.c.blocksArm()); REQUIRE(f.c.arm().ok());
    }
    SUBCASE("a prefix-only keepalive feeds without applying values") {
        REQUIRE(f.send(base+199000,0,true)); f.ticks(); f.c.monitor(base+200001);
        CHECK(f.c.armStatus()==core::ArmStatus::Armed); CHECK(f.value(512)==0);
    }
}
TEST_CASE("Fast DISARM validates exact bodies, bypasses service state, and preserves Fault") {
    Safety f; core::CommandService service(f.c,{}); REQUIRE(f.c.arm().ok());
    std::array<uint8_t,6> out{}; std::array<uint8_t,5> request{cw::kVersion,77,1,3,0};
    REQUIRE(core::fastDisarm(f.c,request,out)==6); CHECK(out[4]!=0); CHECK(f.c.params().armed());
    REQUIRE(core::fastDisarm(f.c,{request.data(),4},out)==6); CHECK(out[1]==77); CHECK(out[4]==0); CHECK_FALSE(f.c.params().armed());
    core_os::raiseDiag(codes::actuator_mock::kFaultInjected,0);
    REQUIRE(core::fastDisarm(f.c,{request.data(),4},out)==6); CHECK(f.c.armStatus()==core::ArmStatus::Fault);
}
TEST_CASE("The limits in effect override the saved ones in GET; GET_PENDING keeps those") {
    core::CoreConfig config; config.max_loops=3; config.max_instances=23;
    config.subscription_groups=3; config.command_groups=2; config.group_entries=12;
    config.configure_system=nullptr; // fixed by this test
    Safety f(true,config);
    const std::array<core::Ref,5> refs{{{0,0,16},{0,0,17},{0,0,18},{0,0,19},{0,0,20}}};
    std::array<uint32_t,5> values{}; REQUIRE(f.c.params().get(refs,values).ok());
    CHECK(values==std::array<uint32_t,5>{3,23,3,2,12});
    REQUIRE(f.c.getPending(refs,values).ok());
    CHECK(values==std::array<uint32_t,5>{4,16,8,8,64});
    REQUIRE(f.c.save().ok());
    f.c.stop(); config.group_entries=7;
    core::Core rebooted(config); REQUIRE(rebooted.init(mod_mock::kTypes).ok()); REQUIRE(rebooted.boot(core::prepareBoot(f.store)).ok());
    REQUIRE(rebooted.params().get(refs,values).ok()); CHECK(values[4]==7);
}
TEST_CASE("Sparse diagnostic exhaustion preserves safety identities through recovery") {
    Safety f; REQUIRE(f.c.arm().ok()); auto &table=core_os::native::diagTable();
    for (unsigned i=0;i<255;++i) { (void)table.resolve(codes::core::kCommandDropped.slot,static_cast<uint8_t>(i)); }
    core_os::raiseDiag(codes::actuator_mock::kFaultInjected,254);
    core_os::raiseDiag(codes::actuator_mock::kFaultInjected,253);
    CHECK(f.c.armStatus()==core::ArmStatus::Fault);
    core_os::clearDiag(codes::actuator_mock::kFaultInjected,254);
    CHECK(f.c.acknowledgeFault().detail==core::Detail::FaultActive);
    core_os::clearDiag(codes::actuator_mock::kFaultInjected,253);
    REQUIRE(f.c.acknowledgeFault().ok()); REQUIRE(f.c.arm().ok());
}
