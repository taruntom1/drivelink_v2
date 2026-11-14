#include "doctest.h"
#include "core/core.hpp"
#include "core/persistence.hpp"
#include "core_os/clock.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/native_task.hpp"
#include "mod_motor/modules.hpp"
#include "mod_motor/interfaces.hpp"
#include "mod_mock/modules.hpp"
#include "pid_kernel.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/gen/type_actuator_mock.hpp"
#include "drivelink/protocol/gen/type_feedback_mock.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>

namespace {
namespace defs = drivelink::protocol::defs;
struct Manual {
    Manual() { core_os::native::setManualScheduling(true); core_os::native::resetRecordedDiag(); core_os::native::resetBootInfo(); }
    ~Manual() { core_os::native::setManualScheduling(false); core_os::native::resetRecordedDiag(); }
};
struct Motor {
    Manual manual;
    core_os::KvStore store;
    core::Core c{core::CoreConfig{}};
    const std::array<const core::ModuleType *,5> types{
        mod_mock::kTypes[0], mod_mock::kTypes[1], mod_mock::kTypes[2], mod_mock::kTypes[3], mod_motor::kTypes[0]};
    Motor(bool controller = true, bool duplicate = false, uint32_t actuator_ref = 112U<<8, uint32_t feedback_ref = 113U<<8,
          float noise = 0) {
        std::vector<core::persist::StoredInstance> layout(controller ? (duplicate ? 5 : 4) : 3);
        const uint8_t type_ids[]{5,112,113,32,32};
        for (size_t i=0; i<layout.size(); ++i) {
            layout[i].type_id = type_ids[i]; layout[i].instance = i==4 ? 1 : 0;
            layout[i].loop = i==0 ? 255 : 0;
            std::snprintf(layout[i].label, sizeof(layout[i].label), "m%u", static_cast<unsigned>(i));
        }
        layout[0].pairs = {{1,1000},{2,1}};
        layout[2].pairs = {{1,112U<<8},{5,defs::f32Bits(noise)}};
        if (controller) {
            layout[3].pairs = {{1,actuator_ref},{2,feedback_ref},{32,defs::f32Bits(.12F)},
                {33,defs::f32Bits(.8F)},{48,defs::f32Bits(3.F)},{49,defs::f32Bits(2.F)}};
            if (duplicate) { layout[4].pairs = layout[3].pairs; }
        }
        REQUIRE(core::persist::saveConfig(store,1,layout,{}).ok());
        REQUIRE(c.init(types).ok()); REQUIRE(c.boot(store).ok());
        if (!c.instancesFailed()) {
            std::array<core::Ref,3> inputs{{{32,0,514},{32,0,512},{32,0,513}}};
            const core::Ref direct{112,0,512};
            REQUIRE(c.groups().stageConfig(core::GroupKind::Command,0,0,1,
                controller ? std::span<const core::Ref>(inputs) : std::span<const core::Ref>(&direct,1)).ok());
            tick(); REQUIRE(c.groups().complete(core::GroupKind::Command,0,0).ok());
        }
        REQUIRE(c.start().ok());
    }
    void tick(unsigned n=1) { for (unsigned i=0;i<n;++i) { c.loop(0).runTick(1); } }
    float get(uint8_t type, uint16_t id) const {
        const auto *r=c.findRecord(type,0); const auto i=r->itemIndex(id);
        REQUIRE(i != core::detail::kNoItem);
        const auto bits=core::loadValue(r->desc->items[i].type, r->bindings[i].ptr);
        return r->desc->items[i].type==defs::ValueType::F32 ? defs::f32FromBits(bits) : static_cast<float>(bits);
    }
    void set(uint8_t type, uint16_t id, uint32_t bits) {
        const core::SetEntry entry{{type,0,id},bits}; uint32_t applied;
        core::PendingSet pending;
        REQUIRE(c.params().stage({&entry,1},{&applied,1},pending).ok());
        tick(); REQUIRE(c.params().complete(pending,0).ok());
    }
    void setFloat(uint8_t type,uint16_t id,float value) { set(type,id,defs::f32Bits(value)); }
    bool send(float duty,float velocity=0,float position=0,int64_t at=0,bool keepalive=false) {
        std::array<uint8_t,16> bytes{}; drivelink::protocol::wire::Writer w(bytes.data(),bytes.size());
        w.appendU8(drivelink::protocol::core_wire::kVersion); w.appendU8(1); w.appendU16Le(0);
        if (!keepalive) {
            w.appendU32Le(defs::f32Bits(duty)); w.appendU32Le(defs::f32Bits(velocity)); w.appendU32Le(defs::f32Bits(position));
        }
        return c.groups().receiveCommand(0,{bytes.data(),w.size()},at ? at : core_os::nowUs()+1);
    }
    void arm() { REQUIRE(c.arm().ok()); tick(); }
};
Motor *injected_motor = nullptr;
core::OnChange original_mode_hook = nullptr;
void commandAfterModeHook(void *self, uint16_t id) {
    original_mode_hook(self,id);
    REQUIRE(injected_motor->send(.2F,2));
}
}

TEST_CASE("Cascade PID kernel tracks output without kick and recovers from saturation") {
    pid::Kernel p; p.kp=1; p.ki=1; p.kd=.1F;
    p.track(2,2,.4F,1); CHECK(p.compute(2,2,.001F,1)==doctest::Approx(.4F));
    for (int i=0;i<1000;++i) { CHECK(p.compute(0,100,.001F,1)==1); }
    CHECK(p.i==doctest::Approx(.4F));
    CHECK(p.compute(0,-1,.001F,1) < 0);
    CHECK(p.compute(0,0,0,1)==0);
    p.reset(); CHECK(p.i==0); CHECK(p.d==0);
    p.kp=p.ki=p.kd=0; CHECK(p.compute(0,100,.001F,1)==0);
}
TEST_CASE("Testgen receipts activate switched controller modes without feeding watchdog") {
    Motor f; f.arm();
    core::TestgenConfig cfg; cfg.loop=0; cfg.target={32,0,512}; cfg.amplitude=2; cfg.repeat=0;
    uint32_t run;
    REQUIRE(f.c.testgen().stageStart(cfg,run).ok()); f.tick(); REQUIRE(f.c.testgen().complete(0,0).ok());
    f.set(32,16,1); // velocity mode: first tick is the bumpless switch
    CHECK(f.get(32,260)==1);
    f.tick(3); CHECK(f.get(32,260)==0); CHECK(f.get(32,259)==doctest::Approx(2));
    int64_t receipt=123;
    REQUIRE(f.c.groups().lastCommandUs(0,receipt)); CHECK(receipt==0); // generator never feeds it
    f.c.monitor(core_os::nowUs());
    f.tick(); f.c.monitor(core_os::nowUs()+250000); // fresh heartbeat, expired command grace
    CHECK(f.c.armStatus()==core::ArmStatus::Disarmed);
    CHECK(f.c.lastDisarmReason()==core::DisarmReason::Watchdog);
    CHECK_FALSE(f.c.testgen().running()); f.tick(); CHECK(f.get(32,258)==0);
}
TEST_CASE("Motor controller shares quantized feedback and controls velocity and position") {
    Motor f; f.arm();
    f.set(32,16,1); REQUIRE(f.send(0,2)); f.tick(5000);
    CHECK(f.get(32,257)==doctest::Approx(2).epsilon(.08));
    CHECK(f.get(32,256)==f.get(113,257));
    CHECK(f.get(32,257)==f.get(113,256));
    CHECK(f.get(32,260)==0);
    f.set(32,16,2); const float target=f.get(32,256)+1;
    REQUIRE(f.send(0,0,target)); f.tick(6000);
    CHECK(f.get(32,256)==doctest::Approx(target).epsilon(.025));
    CHECK(std::fabs(f.get(32,257)) < .12F);
    const float step=6.283185307F/1440.F;
    CHECK(f.get(113,257)/step==doctest::Approx(std::round(f.get(113,257)/step)).epsilon(.00001));
}
TEST_CASE("Cascade limits constrain outer velocity and inner duty") {
    Motor f; f.setFloat(32,17,.2F); f.setFloat(32,18,.5F); f.arm(); f.set(32,16,2);
    REQUIRE(f.send(0,0,100)); f.tick(500);
    CHECK(std::fabs(f.get(32,259))<=.5F); CHECK(std::fabs(f.get(32,258))<=.2F);
    f.setFloat(32,17,0); f.tick(); CHECK(f.get(32,258)==0);
}
TEST_CASE("All six mode transfers preserve duty on the transition tick with plant imperfections") {
    for (uint32_t old_mode=0; old_mode<3; ++old_mode) {
        for (uint32_t next=0; next<3; ++next) {
            if (old_mode==next) { continue; }
            Motor f(true,false,112U<<8,113U<<8,.25F); f.arm();
            if (old_mode) { f.set(32,16,old_mode); }
            REQUIRE(f.send(.3F,3,3)); f.tick(200);
            const float before=f.get(32,258); REQUIRE(std::fabs(before)>.01F);
            f.set(32,16,next);
            CHECK(f.get(32,258)==doctest::Approx(before)); CHECK(f.get(32,260)==1);
            // Inactive targets arrived before the mode boundary; they stay stale.
            f.tick(20); CHECK(f.get(32,260)==1);
            REQUIRE(f.send(.3F,3,3)); f.tick(); CHECK(f.get(32,260)==0);
        }
    }
}
TEST_CASE("A mode boundary excludes queued values keepalives and repeated mode writes do not create a new boundary") {
    Motor f; f.arm(); REQUIRE(f.send(.3F,5,5)); f.tick(100);
    // Queue an old command before applying the new mode; group copy is after hooks.
    REQUIRE(f.send(.3F,5,5,core_os::nowUs()-1)); f.set(32,16,1); f.tick();
    CHECK(f.get(32,260)==1);
    REQUIRE(f.send(0,0,0,0,true)); f.tick(); CHECK(f.get(32,260)==1);
    CHECK_FALSE(f.send(0,std::nanf(""))); f.tick(); CHECK(f.get(32,260)==1);
    REQUIRE(f.send(.3F,5,5)); f.tick(); CHECK(f.get(32,260)==0);
    f.set(32,16,1); CHECK(f.get(32,260)==0);
    f.c.disarm(); REQUIRE(f.c.arm().ok());
    REQUIRE(f.send(0,0,0,0,true)); f.tick(); CHECK(f.get(32,258)==0); CHECK(f.get(32,260)==1);
    REQUIRE(f.send(0,5)); f.tick(); CHECK(f.get(32,260)==0);
}
TEST_CASE("Disarmed mode changes hold position at the measurement and cannot restore old duty") {
    Motor f; f.arm(); REQUIRE(f.send(.5F)); f.tick(300); f.c.disarm(); f.tick();
    f.set(32,16,2); f.tick(200); f.arm(); f.tick();
    CHECK(f.get(32,258)==0); CHECK(f.get(32,260)==1);
}
TEST_CASE("A value received during the transition tick is used only on the following tick") {
    Motor f; f.arm(); REQUIRE(f.send(.2F)); f.tick(100);
    const auto *r=f.c.findRecord(32,0); auto &binding=r->bindings[r->itemIndex(16)];
    original_mode_hook=binding.hook; binding.hook=&commandAfterModeHook; injected_motor=&f;
    const float before=f.get(32,258);
    f.set(32,16,1); CHECK(f.get(32,258)==doctest::Approx(before)); CHECK(f.get(32,260)==1);
    f.tick(); CHECK(f.get(32,260)==0);
    binding.hook=original_mode_hook; injected_motor=nullptr;
}
TEST_CASE("Commands for unrelated inputs do not satisfy new mode freshness") {
    Motor f;
    const core::Ref input{32,0,514};
    REQUIRE(f.c.groups().stageConfig(core::GroupKind::Command,0,0,1,{&input,1}).ok());
    f.tick(); REQUIRE(f.c.groups().complete(core::GroupKind::Command,0,0).ok());
    f.arm(); f.set(32,16,1);
    std::array<uint8_t,8> bytes{}; drivelink::protocol::wire::Writer w(bytes.data(),bytes.size());
    w.appendU8(drivelink::protocol::core_wire::kVersion); w.appendU8(2); w.appendU16Le(0); w.appendU32Le(defs::f32Bits(.5F));
    REQUIRE(f.c.groups().receiveCommand(0,bytes,core_os::nowUs()+1)); f.tick();
    CHECK(f.get(32,260)==1); CHECK(f.get(32,258)==0);
    const auto *r=f.c.findRecord(32,0);
    CHECK(*r->bindings[r->itemIndex(512)].receipt_us==0);
    CHECK(*r->bindings[r->itemIndex(514)].receipt_us>0);
}
TEST_CASE("Actuator ownership rejects duplicate controllers unwired refs and direct drive overrides") {
    SUBCASE("duplicate") { Motor f(true,true); CHECK(f.c.instancesFailed()==1); CHECK(f.c.arm().detail==core::Detail::StartFailed); }
    SUBCASE("unwired") { Motor f(true,false,0); CHECK(f.c.instancesFailed()==1); }
    SUBCASE("missing feedback") { Motor f(true,false,112U<<8,0); CHECK(f.c.instancesFailed()==1); }
    SUBCASE("owned drive") {
        Motor f; f.arm(); REQUIRE(f.send(.25F)); f.tick(5);
        core::ActionArgs in; in.values[0]=defs::f32Bits(-1); core::ActionOut out;
        // Run on the owning loop here, avoiding config-task rendezvous in manual scheduling.
        CHECK(f.c.findRecord(112,0)->object->runAction(768,in,out).detail==core::Detail::AlreadyBound);
        const core::Ref direct{112,0,512};
        f.c.disarm(); f.tick();
        REQUIRE(f.c.groups().stageConfig(core::GroupKind::Command,1,0,1,{&direct,1}).ok());
        f.tick(); REQUIRE(f.c.groups().complete(core::GroupKind::Command,1,0).ok()); f.arm();
        std::array<uint8_t,8> b{}; drivelink::protocol::wire::Writer w(b.data(),b.size());
        w.appendU8(drivelink::protocol::core_wire::kVersion); w.appendU8(1); w.appendU16Le(0); w.appendU32Le(defs::f32Bits(1));
        REQUIRE(f.c.groups().receiveCommand(1,b,core_os::nowUs()+1)); f.tick(20);
        CHECK(f.get(112,258)==0); // standalone input cannot bypass owner
    }
}
TEST_CASE("Mock deadband delay cap and emergency force govern physical effort") {
    Motor f; f.arm(); REQUIRE(f.send(.01F)); f.tick(20); CHECK(f.get(112,258)==0);
    f.setFloat(112,4,.25F); REQUIRE(f.send(.8F)); f.tick(); CHECK(f.get(112,258)==0);
    f.tick(); CHECK(f.get(112,258)==.25F); f.tick(100);
    const float moving=f.get(112,256); REQUIRE(moving>0);
    auto *a=f.c.findRecord(112,0)->object.get(); a->emergencyStop();
    a->tick({core_os::nowUs(),.001F,1}); CHECK(f.get(112,258)==0); CHECK(f.get(112,256)<moving);
    f.c.disarm(); f.tick(); f.arm(); f.tick(5); CHECK(f.get(112,258)==0);
}
TEST_CASE("Shared actuator claim lifetime survives provider destruction") {
    struct A : mod_motor::Actuator {
        bool start(core::InstanceContext &) override { return true; }
        void tick(const core::TickContext &) override {}
    };
    auto a=std::make_unique<A>(); auto claim=a->claim(); REQUIRE(claim); CHECK_FALSE(a->claim());
    a.reset(); CHECK(claim->claimed); claim->claimed=false; claim.reset();
}
TEST_CASE("Mock feedback consumes an optional plant interface with reproducible noise reset and count wrap") {
    struct Plant : mod_motor::Actuator {
        mod_motor::PlantObservation state;
        bool start(core::InstanceContext &) override { return true; }
        void tick(const core::TickContext &) override {}
        bool plantObservation(mod_motor::PlantObservation &out) const override { out=state; return true; }
    };
    core::detail::InstanceTable table(2);
    auto provider=std::make_unique<core::detail::InstanceRecord>();
    provider->type_id=112; provider->instance=0;
    auto plant=std::make_unique<Plant>(); auto *raw=plant.get(); provider->object=std::move(plant);
    REQUIRE(table.add(std::move(provider)));
    core::detail::InstanceRecord record;
    record.desc=&defs::feedback_mock::kDesc; record.type_id=113;
    record.bindings=std::make_unique<core::detail::Binding[]>(record.desc->items.size());
    record.shadow=std::make_unique<uint32_t[]>(record.desc->items.size());
    for(size_t i=0;i<record.desc->items.size();++i) { record.shadow[i]=record.desc->items[i].default_bits; }
    record.shadow[record.itemIndex(1)]=112U<<8;
    const core::InstanceConfig cfg{record.desc,113,0,0,"feedback"};
    record.object.reset(mod_mock::kTypes[1]->create(cfg));
    core::InstanceContext ctx(record,&table); REQUIRE(record.object->start(ctx));
    auto *feedback=static_cast<mod_motor::Feedback *>(record.object->queryInterface(mod_motor::kFeedbackInterface));
    REQUIRE(feedback);
    *static_cast<float *>(record.bindings[record.itemIndex(4)].ptr)=0; // unfiltered wrap delta
    raw->state.position=2147483647.0*6.28318530717958647692/1440.0;
    record.object->tick({0,.001F,1}); CHECK(feedback->measurement().count==2147483647);
    raw->state.position=2147483649.0*6.28318530717958647692/1440.0;
    record.object->tick({1000,.001F,1}); CHECK(feedback->measurement().count==-2147483647);
    CHECK(feedback->measurement().velocity==doctest::Approx(2*6.283185307179586F/1.44F));
    raw->state.position=0; ++raw->state.reset_generation;
    *static_cast<float *>(record.bindings[record.itemIndex(5)].ptr)=10;
    std::array<int32_t,30> counts{};
    for(auto &count:counts) { record.object->tick({0,.001F,1}); count=feedback->measurement().count; CHECK(std::abs(count)<=10); }
    CHECK(*std::min_element(counts.begin(),counts.end()) != *std::max_element(counts.begin(),counts.end()));
    ++raw->state.reset_generation;
    for(auto count:counts) { record.object->tick({0,.001F,1}); CHECK(feedback->measurement().count==count); }
    *static_cast<float *>(record.bindings[record.itemIndex(5)].ptr)=0;
    ++raw->state.reset_generation; record.object->tick({0,.001F,1}); CHECK(feedback->measurement().velocity==0);
}
