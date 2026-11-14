#include "doctest.h"
#include "core/core.hpp"
#include "core/loop_instance.hpp"
#include "core/command_service.hpp"
#include "core/persistence.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/native_task.hpp"
#include "core_os/realtime.hpp"
#include "core_os/native_realtime.hpp"
#include "core_os/mutex.hpp"
#include "core_os/native_diag.hpp"
#include "drivelink/protocol/gen/type_loop.hpp"
#include "drivelink/protocol/gen/type_actuator_mock.hpp"
#include "mod_mock/modules.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
namespace defs = drivelink::protocol::defs;
namespace lp = defs::loop;
struct ExpectedViolations {
    core_os::native::RealtimeViolationSink previous=core_os::native::setRealtimeViolationSink(nullptr);
    ~ExpectedViolations() { core_os::native::setRealtimeViolationSink(previous); }
};
core::persist::StoredInstance storedLoop(uint8_t id, bool realtime)
{
    core::persist::StoredInstance s{};
    s.type_id=defs::kLoopTypeId; s.instance=id; s.loop=core::kNoLoop;
    s.label[0]='l'; s.label[1]=static_cast<char>('a'+id); s.label[2]=0;
    s.pairs={{lp::kRealtime,realtime?1U:0U}};
    return s;
}
void boot(core::Core &c, core_os::KvStore &store)
{
    core_os::native::resetBootInfo();
    REQUIRE(c.init(mod_mock::kTypes).ok());
    REQUIRE(c.boot(store).ok());
}
core::Status set(core::Core &c, uint8_t type, uint8_t instance, uint16_t id, uint32_t value)
{
    const core::SetEntry e{{type,instance,id},value}; uint32_t applied=0;
    return c.params().set({&e,1},{&applied,1});
}

std::array<int8_t, 8> requested_affinities{};
void observeStart(const char *name, int8_t affinity)
{
    unsigned id = 0;
    if (std::sscanf(name, "core_loop_%u", &id) == 1 && id < requested_affinities.size()) {
        requested_affinities[id] = affinity;
    }
}
struct PlacementObservation {
    bool previous_manual = core_os::native::manualScheduling();
    core_os::native::PeriodicStartObserver previous_observer = core_os::native::setPeriodicStartObserver(observeStart);
    PlacementObservation() { core_os::native::setManualScheduling(true); requested_affinities.fill(99); }
    ~PlacementObservation() {
        core_os::native::setPeriodicStartObserver(previous_observer);
        core_os::native::setManualScheduling(previous_manual);
    }
};
uint32_t placement(core::Core &c, uint8_t id, bool pending = false)
{
    const core::Ref ref{defs::kLoopTypeId, id, lp::kCore}; uint32_t value = 99;
    REQUIRE((pending ? c.getPending({&ref, 1}, {&value, 1}) : c.params().get({&ref, 1}, {&value, 1})).ok());
    return value;
}
}

TEST_CASE("Configurable loops: factory defaults and a one-loop build")
{
    for (uint8_t limit : {uint8_t{1},uint8_t{4},uint8_t{8}}) {
        PlacementObservation observation;
        core_os::KvStore store; core::CoreConfig cfg; cfg.max_loops=limit;
        core::Core c(cfg); boot(c,store);
        CHECK(c.loopCount()==(limit==1?1:2));
        REQUIRE(c.findLoop(0)); CHECK(c.loop(0).realtime()); CHECK(c.loop(0).periodUs()==1000);
        if (limit>1) { REQUIRE(c.findLoop(1)); CHECK_FALSE(c.loop(1).realtime()); CHECK(c.loop(1).periodUs()==10000); }
        CHECK(c.findLoop(19)==nullptr);
        CHECK(c.findRecord(defs::kLoopTypeId,0)->loop==core::kNoLoop);
        CHECK(placement(c, 0) == 1); CHECK(requested_affinities[0] == 1);
        if (limit > 1) { CHECK(placement(c, 1) == 0); CHECK(requested_affinities[1] == 0); }
    }
}

TEST_CASE("Loop placement: only declared choices map to an OS affinity")
{
    for (unsigned value = 0; value <= 255; ++value) {
        int8_t affinity = 99;
        const bool ok = core::loopCoreAffinity(static_cast<uint8_t>(value), affinity);
        CHECK(ok == (value <= 2));
        CHECK(affinity == (value < 2 ? static_cast<int8_t>(value) : (value == 2 ? core_os::kNoAffinity : 99)));
    }
}

TEST_CASE("Loop placement: new default, strict enum validation and pending lifecycle")
{
    PlacementObservation observation;
    core_os::KvStore store;
    uint8_t id = 255;
    {
        core::Core c(core::CoreConfig{}); boot(c, store); REQUIRE(c.save().ok());
        REQUIRE(c.addInstance(defs::kLoopTypeId, core::kNoLoop, "automatic", id).ok());
        CHECK(placement(c, id, true) == 2);
        CHECK(requested_affinities[id] == 99); // no task until SAVE/reboot
        for (uint32_t invalid : {3U, 255U, 256U, 0xFFFFFFFFU}) {
            const auto st = set(c, defs::kLoopTypeId, id, lp::kCore, invalid);
            CHECK(st.result == core::Result::InvalidArgument); CHECK(st.detail == core::Detail::BadEnum);
            CHECK(placement(c, id, true) == 2);
        }
        REQUIRE(set(c, defs::kLoopTypeId, id, lp::kCore, 1).accepted());
        CHECK(placement(c, id, true) == 1);
        REQUIRE(set(c, defs::kLoopTypeId, id, lp::kCore, 2).accepted());
        REQUIRE(c.save().ok());
    }
    {
        core::Core c(core::CoreConfig{}); boot(c, store);
        REQUIRE(c.findLoop(id)); CHECK(placement(c, id) == 2);
        CHECK(requested_affinities[id] == core_os::kNoAffinity);
        REQUIRE(set(c, defs::kLoopTypeId, id, lp::kCore, 0).accepted());
        CHECK(c.pendingReboot()); CHECK(placement(c, id) == 2); CHECK(placement(c, id, true) == 0);
        CHECK(requested_affinities[id] == core_os::kNoAffinity);
        REQUIRE(c.revert().ok()); // REVERT changes live params, not reboot-required edits.
        CHECK(placement(c, id, true) == 0);
        REQUIRE(c.discardPending().ok()); CHECK_FALSE(c.pendingReboot());
        CHECK(placement(c, id) == 2);
        REQUIRE(c.save().ok());
    }
    core::Core reloaded(core::CoreConfig{}); boot(reloaded, store);
    CHECK(placement(reloaded, id) == 2); CHECK(requested_affinities[id] == core_os::kNoAffinity);
}

TEST_CASE("Loop placement: saved byte values retain meaning in both realtime modes")
{
    PlacementObservation observation;
    for (bool realtime : {false, true}) {
        core_os::KvStore store;
        auto a = storedLoop(0, realtime), b = storedLoop(1, realtime), missing = storedLoop(7, realtime);
        // The pre-change instance blob is id/u32 pairs, with no scalar type tag.
        a.pairs.push_back({lp::kCore, 0}); b.pairs.push_back({lp::kCore, 1});
        const std::array entries{a, b, missing};
        REQUIRE(core::persist::saveConfig(store, 1, entries, {}).ok());
        for (int generation = 0; generation < 2; ++generation) {
            core::Core c(core::CoreConfig{}); boot(c, store);
            CHECK(placement(c, 0) == 0); CHECK(requested_affinities[0] == 0);
            CHECK(placement(c, 1) == 1); CHECK(requested_affinities[1] == 1);
            CHECK(placement(c, 7) == 2); CHECK(requested_affinities[7] == core_os::kNoAffinity);
            REQUIRE(c.start().ok());
            REQUIRE(c.save().ok()); // writes explicit core=any for the previously missing pair
        }
    }
}

TEST_CASE("Loop placement: safe-mode edits are applied only on the next normal boot")
{
    PlacementObservation observation;
    core_os::KvStore store;
    { core::Core c(core::CoreConfig{}); boot(c, store); REQUIRE(c.save().ok()); }
    requested_affinities.fill(99);
    core_os::native::setResetWasCrash(true); core_os::storeCrashCount(2);
    {
        core::Core c(core::CoreConfig{});
        REQUIRE(c.init(mod_mock::kTypes).ok()); REQUIRE(c.boot(store).ok()); REQUIRE(c.safeMode());
        REQUIRE(set(c, defs::kLoopTypeId, 0, lp::kCore, 2).accepted());
        CHECK(placement(c, 0, true) == 2); CHECK(requested_affinities[0] == 99);
        REQUIRE(c.save().ok());
    }
    core_os::native::resetBootInfo();
    core::Core c(core::CoreConfig{}); boot(c, store);
    CHECK(placement(c, 0) == 2); CHECK(requested_affinities[0] == core_os::kNoAffinity);
    CHECK(placement(c, 1) == 0); CHECK(requested_affinities[1] == 0);
}

TEST_CASE("Configurable loops: old layout format falls back to the factory layout")
{
    core_os::KvStore store;
    auto blob=core::encodeLayoutBlob(1,{});blob[0]=1;
    drivelink::protocol::wire::Writer crc(blob.data()+blob.size()-4,4);
    crc.appendU32Le(core::crc32(blob.data(),blob.size()-4));
    REQUIRE(store.set("core","layout_b",blob.data(),blob.size())==core_os::kOk);
    core::Core c(core::CoreConfig{});boot(c,store);
    CHECK_FALSE(c.configLoadedFromNvs());CHECK(c.loopCount()==2);
    REQUIRE(c.start().ok());c.stop();CHECK_FALSE(c.isRunning());
    CHECK(c.start().detail==core::Detail::NotAtBoot);
}

TEST_CASE("Configurable loops: safe mode edits and REVERT use offline loop shadows")
{
    core_os::KvStore store;
    { core::Core c(core::CoreConfig{});boot(c,store);REQUIRE(c.save().ok()); }
    core_os::native::setResetWasCrash(true);core_os::storeCrashCount(2);
    core::Core c(core::CoreConfig{});
    REQUIRE(c.init(mod_mock::kTypes).ok());REQUIRE(c.boot(store).ok());REQUIRE(c.safeMode());
    CHECK(c.loopCount()==0);
    REQUIRE(set(c,defs::kLoopTypeId,0,lp::kRateHz,500).ok());
    REQUIRE(c.revert().ok());
    const core::Ref rate{defs::kLoopTypeId,0,lp::kRateHz};uint32_t hz=0;
    REQUIRE(c.params().get({&rate,1},{&hz,1}).ok());CHECK(hz==1000);
    REQUIRE(c.save().ok());CHECK(c.loopCount()==0);
    core_os::native::resetBootInfo();
}

TEST_CASE("Configurable loops: limits, sparse ids and empty saved layouts")
{
    for (uint8_t count=0; count<=8; ++count) {
        std::vector<core::persist::StoredInstance> loops;
        for (uint8_t id=0; id<count; ++id) { loops.push_back(storedLoop(static_cast<uint8_t>(2*id+2),true)); }
        core_os::KvStore store; REQUIRE(core::persist::saveConfig(store,1,loops,{}).ok());
        core::CoreConfig cfg; cfg.max_loops=8;
        core::Core c(cfg); boot(c,store);
        CHECK(c.loopCount()==count); CHECK(c.configuredCount()==count);
        CHECK(c.findLoop(0)==nullptr);
        uint8_t module=255;
        const auto st=c.addInstance(112,255,"motor",module);
        if (count==0) { CHECK(st.detail==core::Detail::LoopMismatch); }
        else { REQUIRE(st.ok()); CHECK(c.pendingLayout().entries().back().loop==2); }
    }
    core_os::KvStore store; core::CoreConfig cfg; cfg.max_loops=1;
    core::Core c(cfg); boot(c,store); uint8_t id=255;
    CHECK(c.addInstance(defs::kLoopTypeId,255,"extra",id).detail==core::Detail::Capacity);
    CHECK(c.removeInstance(defs::kLoopTypeId,0).ok());
    CHECK(c.addInstance(defs::kLoopTypeId,1,"bad",id).detail==core::Detail::LoopMismatch);
}

TEST_CASE("Configurable loops: pending compatibility, references, SAVE and reload")
{
    core_os::KvStore store;
    {
        core::Core c(core::CoreConfig{}); boot(c,store); uint8_t loop=255, motor=255;
        REQUIRE(c.addInstance(defs::kLoopTypeId,255,"third",loop).ok()); CHECK(loop==2);
        REQUIRE(set(c,defs::kLoopTypeId,loop,lp::kRealtime,1).accepted());
        REQUIRE(set(c,defs::kLoopTypeId,loop,lp::kRateHz,250).accepted());
        REQUIRE(c.addInstance(112,loop,"motor",motor).ok());
        CHECK(c.removeInstance(defs::kLoopTypeId,loop).detail==core::Detail::Referenced);
        CHECK(set(c,defs::kLoopTypeId,loop,lp::kRealtime,0).detail==core::Detail::LoopMismatch);
        REQUIRE(c.save().ok());
    }
    core::Core c(core::CoreConfig{}); boot(c,store);
    REQUIRE(c.findLoop(2)); CHECK(c.loop(2).realtime()); CHECK(c.loop(2).periodUs()==4000);
    CHECK(c.findRecord(112,0)->state==core::InstanceState::Running);
    CHECK(set(c,defs::kLoopTypeId,2,lp::kRateHz,500).ok()); CHECK(c.loop(2).periodUs()==2000);
    REQUIRE(c.start().ok());
    REQUIRE(c.revert().ok()); CHECK(c.loop(2).periodUs()==4000);
    c.params().setArmed(true);
    CHECK(set(c,defs::kLoopTypeId,2,lp::kRateHz,100).result==core::Result::Armed);
    core::ActionOut out;
    CHECK(c.runAction({112,0,769},{},out).result==core::Result::Armed);
    c.params().setArmed(false);
    core_os::native::failNextPeriodChange();
    CHECK(set(c,defs::kLoopTypeId,2,lp::kRateHz,100).detail==core::Detail::TimerFailed);
    CHECK(c.loop(2).periodUs()==4000);
}

TEST_CASE("Configurable loops: one task failure leaves other loops and the link usable")
{
    core_os::KvStore store;
    auto a=storedLoop(0,true), b=storedLoop(7,false);
    core::persist::StoredInstance motor{};motor.type_id=112;motor.loop=0;std::strcpy(motor.label,"motor");
    core::persist::StoredInstance imu{};imu.type_id=114;imu.loop=7;std::strcpy(imu.label,"imu");
    const std::array entries{a,b,motor,imu};
    REQUIRE(core::persist::saveConfig(store,1,entries,{}).ok());
    core_os::native::failPeriodicStartAfter(0);
    core::Core c(core::CoreConfig{}); boot(c,store); REQUIRE(c.start().ok());
    CHECK(c.findLoop(0)==nullptr); REQUIRE(c.findLoop(7));
    CHECK(c.findRecord(defs::kLoopTypeId,0)->state==core::InstanceState::Failed);
    CHECK(c.findRecord(112,0)->state==core::InstanceState::DependencyMissing);
    CHECK(c.findRecord(114,0)->state==core::InstanceState::Running);
    core::CommandService service(c,{}); const uint8_t query[]={drivelink::protocol::core_wire::kVersion,1,1,1};std::array<uint8_t,1024> reply{};
    CHECK(service.handle(query,reply)>6);CHECK(reply[4]==0);
    c.stop();
}

TEST_CASE("Real-time guard: C++ and C allocations, blocking calls and thread isolation")
{
    ExpectedViolations expected;
    const auto before=core_os::realtimeViolations();
    core_os::Mutex mutex;core_os::Event event;core_os::KvStore store;
    {
        core_os::RealtimeScope scope(true);
        auto *p=::operator new(37);::operator delete(p);
        void *(*volatile alloc)(size_t)=&std::malloc;std::free(alloc(17));
        mutex.lock();mutex.unlock();event.wait(0);
        size_t len=0;store.get("test","value",nullptr,len);
    }
    CHECK(core_os::realtimeViolations()>=before+5);
    const auto clean=core_os::realtimeViolations();
    std::thread other([] { auto *p=::operator new(37);::operator delete(p); });
    { core_os::RealtimeScope scope(true);CHECK(core_os::realtimeActive()); }
    other.join();CHECK(core_os::realtimeViolations()==clean);CHECK_FALSE(core_os::realtimeActive());
}

TEST_CASE("Real-time guard: only realtime ticks enable it, including nested scopes")
{
    ExpectedViolations expected;
    struct Allocator final : core::Instance {
        bool start(core::InstanceContext &) override { return true; }
        void tick(const core::TickContext &) override { auto *p=::operator new(31);::operator delete(p); }
    } instance;
    core::Loop rt(9,"rt",true,1,1000000), background(3,"background",false,1,1000000);
    REQUIRE(rt.addInstance(&instance));REQUIRE(background.addInstance(&instance));
    auto before=core_os::realtimeViolations();
    background.runTick(1);CHECK(core_os::realtimeViolations()==before);
    rt.runTick(1);CHECK(core_os::realtimeViolations()>before);CHECK_FALSE(core_os::realtimeActive());
    { core_os::RealtimeScope outer(true);core_os::RealtimeScope inner(false);CHECK(core_os::realtimeActive()); }
}

TEST_CASE("Loop rate batch: a later restart failure rolls back earlier loops and shadows")
{
    core_os::KvStore store;core::Core c(core::CoreConfig{});boot(c,store);
    const std::array<core::SetEntry,2> entries{{{{defs::kLoopTypeId,0,lp::kRateHz},500},
                                              {{defs::kLoopTypeId,1,lp::kRateHz},50}}};
    std::array<uint32_t,2> values{};
    core_os::native::failPeriodChangeAfter(1);
    const auto st=c.params().set(entries,values);
    CHECK(st.detail==core::Detail::TimerFailed);CHECK(st.index==1);
    CHECK(c.loop(0).periodUs()==1000);CHECK(c.loop(1).periodUs()==10000);
    const std::array<core::Ref,2> refs{entries[0].ref,entries[1].ref};
    REQUIRE(c.params().get(refs,values).ok());CHECK(values[0]==1000);CHECK(values[1]==100);
}

TEST_CASE("Real-time guard: every realtime-capable mock ticks with staged params and an action")
{
    core_os::KvStore store;
    {
        core::Core c(core::CoreConfig{});boot(c,store);uint8_t motor=0,feedback=0;
        REQUIRE(c.addInstance(112,0,"motor",motor).ok());
        REQUIRE(c.addInstance(113,0,"feedback",feedback).ok());
        REQUIRE(set(c,113,feedback,1,(112U<<8U)|motor).accepted());
        REQUIRE(c.save().ok());
    }
    core::Core c(core::CoreConfig{});boot(c,store);
    core_os::native::resetRecordedDiag();
    uint32_t applied=0;core::PendingSet pending;
    const auto *record=c.findRecord(112,0);REQUIRE(record);
    const defs::ItemDesc *param=nullptr;
    for(const auto &item:record->desc->items) {
        if(item.kind==defs::ItemKind::Param && !defs::hasFlag(item,defs::ItemFlag::RebootRequired)) { param=&item;break; }
    }
    REQUIRE(param);
    const core::SetEntry live{{112,0,param->id},param->default_bits};
    REQUIRE(c.params().stage({&live,1},{&applied,1},pending).ok());
    auto &slot=c.loop(0).actionSlot();slot.target=record->object.get();slot.id=768;slot.values={};
    slot.state.store(core::ActionState::Ready);
    const auto violations=core_os::realtimeViolations();
    c.loop(0).runTick(1);
    CHECK(core_os::realtimeViolations()==violations);
    REQUIRE(c.params().complete(pending,0).ok());
    slot.state.store(core::ActionState::Empty);
}
