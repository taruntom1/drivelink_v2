#include "doctest.h"
#include "core/group_service.hpp"
#include "core/command_service.hpp"
#include "core_os/realtime.hpp"
#include "core_test_modules.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include <atomic>
#include <thread>

using namespace testmod;
namespace {
using core::GroupKind;
constexpr auto sub = GroupKind::Subscription;
constexpr auto cmd = GroupKind::Command;
struct Groups {
    core_os::KvStore store;
    core::Core c{testConfig()};
    Groups() {
        reset();
        bootWith(c,store,{{fx::kTypeId,0,0,"a"},{fx::kTypeId,1,1,"b"}});
    }
    core::GroupService &g() { return c.groups(); }
    core::Status install(GroupKind kind, uint8_t id, uint8_t loop, std::span<const core::Ref> refs, uint16_t divider=1) {
        auto st=g().stageConfig(kind,id,loop,divider,refs);
        if (!st.ok()) { return st; }
        // Rebindings are detached by the old owning loop first.
        c.loop(0).runTick(1); c.loop(1).runTick(1);
        return g().complete(kind,id,0);
    }
};
std::vector<uint8_t> frame(uint8_t epoch, std::initializer_list<uint32_t> values={}) {
    std::vector<uint8_t> bytes(4+4*values.size());
    drivelink::protocol::wire::Writer w(bytes.data(),bytes.size());
    w.appendU8(drivelink::protocol::core_wire::kVersion);w.appendU8(epoch);w.appendU16Le(65535);
    for (auto value:values) { w.appendU32Le(value); } return bytes;
}
}
TEST_CASE("Groups validate before publishing, enforce loop ownership and command exclusivity") {
    Groups f;
    const core::Ref input{fx::kTypeId,0,fx::kVelocitySp}, signal{fx::kTypeId,0,fx::kPosition};
    REQUIRE(f.install(cmd,0,0,{&input,1}).ok());
    CHECK(f.g().info(cmd,0)->epoch==1);
    CHECK(f.g().stageConfig(cmd,1,0,1,{&input,1}).detail==core::Detail::AlreadyBound);
    CHECK(f.g().stageConfig(cmd,0,0,1,{&signal,1}).detail==core::Detail::WrongKind);
    CHECK(f.g().stageConfig(sub,0,1,1,{&signal,1}).detail==core::Detail::LoopMismatch);
    const core::Ref param{fx::kTypeId,0,fx::kGain}, unknown{255,0,0};
    CHECK(f.g().stageConfig(sub,0,0,1,{&param,1}).detail==core::Detail::WrongKind);
    CHECK(f.g().stageConfig(sub,0,0,1,{&unknown,1}).detail==core::Detail::UnknownRef);
    CHECK(f.g().stageConfig(sub,0,8,1,{}).detail==core::Detail::LoopMismatch);
    CHECK(f.g().stageConfig(sub,0,0,0,{}).detail==core::Detail::BadValue);
    CHECK(f.g().stageConfig(sub,255,0,1,{}).detail==core::Detail::Capacity);
    std::array<core::Ref,2> duplicates{signal,signal};
    CHECK(f.g().stageConfig(sub,0,0,1,duplicates).detail==core::Detail::DuplicateRef);
    CHECK(f.g().stageConfig(cmd,0,0,1,{&input,1},true).result==core::Result::Armed);
    CHECK(f.g().clear(cmd,0,true).result==core::Result::Armed);
    CHECK(f.g().info(cmd,0)->epoch==1); // no failed mutation changes the resource
}
TEST_CASE("Groups swap at boundaries, withdraw definitely and migrate across loops") {
    Groups f;
    const core::Ref a{fx::kTypeId,0,fx::kPosition},b{fx::kTypeId,1,fx::kPosition};
    REQUIRE(f.g().stageConfig(sub,0,0,1,{&a,1}).ok());
    CHECK_FALSE(f.g().info(sub,0)->configured);
    CHECK(f.g().stageConfig(sub,0,0,1,{&a,1}).result==core::Result::Busy);
    CHECK(f.g().complete(sub,0,0).detail==core::Detail::Withdrawn);
    f.c.loop(0).runTick(1);
    CHECK_FALSE(f.g().info(sub,0)->configured);
    REQUIRE(f.install(sub,0,0,{&a,1}).ok());
    REQUIRE(f.g().stageConfig(sub,0,1,1,{&b,1}).ok());
    f.c.loop(1).runTick(1); // new loop cannot steal a still-owned group
    CHECK(f.g().info(sub,0)->loop==0);
    f.c.loop(0).runTick(1);
    REQUIRE(f.g().complete(sub,0,0).ok());
    CHECK(f.g().info(sub,0)->loop==1);
    std::array<uint8_t,core::kGroupStreamBytes> bytes{};
    CHECK(f.g().takeStream(0,bytes)==0); // old latched frame invalidated
    f.c.loop(1).runTick(1);
    REQUIRE(f.g().takeStream(0,bytes)==16);CHECK(bytes[1]==2);
    // No loop is running autonomously: clearing withdraws and changes nothing.
    CHECK(f.g().clear(sub,0).detail==core::Detail::Withdrawn);
    CHECK(f.g().info(sub,0)->configured);
}
TEST_CASE("Subscriptions latch ordered typed storage on owning loops with dividers and latest wins") {
    Groups f;
    auto &module=*world().created[0];module.position=3.5F;module.ready=true;
    const std::array<core::Ref,3> refs{{{fx::kTypeId,0,fx::kPosition},{fx::kTypeId,0,fx::kReady},
        {fx::kTypeId,0,fx::kRaw}}};
    REQUIRE(f.install(sub,0,0,refs,2).ok());
    std::array<uint8_t,core::kGroupStreamBytes> bytes{};
    CHECK(f.g().takeStream(0,bytes)==0);
    f.c.loop(0).runTick(1);
    REQUIRE(f.g().takeStream(0,bytes)==24);
    drivelink::protocol::wire::Reader r(bytes.data(),24);
    CHECK(r.readU8()==3);CHECK(r.readU8()==1);CHECK(r.readU16Le()==0);CHECK(r.readU64Le()>0);
    CHECK(r.readU32Le()==testmod::f(3.5F));CHECK(r.readU32Le()==1);CHECK(r.readU32Le()==0);CHECK(r.done());
    for(int i=0;i<6;++i) { f.c.loop(0).runTick(1); }
    CHECK(f.g().takeStream(0,{bytes.data(),4})==0); // too small retains the sample
    REQUIRE(f.g().takeStream(0,bytes)==24);
    CHECK(bytes[2]==3); // two intervening samples overwritten
    CHECK(f.g().takeStream(0,bytes)==0);
    const core::Ref stats{defs::kLoopTypeId,1,defs::loop::kTicks};
    CHECK(f.g().stageConfig(sub,1,0,1,{&stats,1}).detail==core::Detail::LoopMismatch);
    REQUIRE(f.install(sub,1,1,{&stats,1}).ok());
    REQUIRE(f.g().takeStream(1,bytes)==16);
    CHECK(bytes[12]>0);
}
TEST_CASE("Command frames are atomic, typed, epoch checked and disarmed by default") {
    Groups f;
    const std::array<core::Ref,2> refs{{{fx::kTypeId,0,fx::kVelocitySp},{fx::kTypeId,0,fx::kRaw}}};
    REQUIRE(f.install(cmd,0,0,refs).ok());
    auto &module=*world().created[0];
    const auto good=frame(1,{testmod::f(7.F),99});
    int64_t receipt=0;
    REQUIRE(f.g().receiveCommand(0,good,100));
    f.c.loop(0).runTick(1);CHECK(module.velocity_sp==0);CHECK(module.raw==0);
    REQUIRE(f.g().receiveCommand(0,good,100));
    f.g().beginTick(0,true,100);CHECK(module.raw==0); // must be strictly after ARM
    REQUIRE(f.g().receiveCommand(0,good,101));
    auto nan=frame(1,{0x7fc00000,77});
    CHECK_FALSE(f.g().receiveCommand(0,nan,102));
    REQUIRE(f.g().lastCommandUs(0,receipt));CHECK(receipt==101);
    f.g().beginTick(0,true,100);CHECK(module.velocity_sp==7.F);CHECK(module.raw==99);
    const auto stale=frame(0,{testmod::f(1.F),1});CHECK_FALSE(f.g().receiveCommand(0,stale,103));
    const auto inf=frame(1,{0x7f800000,1});CHECK_FALSE(f.g().receiveCommand(0,inf,104));
    auto short_frame=good;short_frame.pop_back();CHECK_FALSE(f.g().receiveCommand(0,short_frame,105));
    auto long_frame=good;long_frame.push_back(0);CHECK_FALSE(f.g().receiveCommand(0,long_frame,106));
    for(size_t i=0;i<4;++i) { CHECK_FALSE(f.g().receiveCommand(0,{good.data(),i},106)); }
    auto wrong_version=good;wrong_version[0]=2;CHECK_FALSE(f.g().receiveCommand(0,wrong_version,107));
    CHECK_FALSE(f.g().receiveCommand(7,good,108));
    const auto keepalive=frame(1);REQUIRE(f.g().receiveCommand(0,keepalive,110));
    REQUIRE(f.g().lastCommandUs(0,receipt));CHECK(receipt==110);f.g().beginTick(0,true,100);CHECK(module.raw==99);
    CHECK(core_os::native::counterTotal(defs::diag::codes::core::kCommandFrameRejected)>=10);
    REQUIRE(f.install(cmd,0,0,refs).ok());REQUIRE(f.g().lastCommandUs(0,receipt));CHECK(receipt==0);
    CHECK_FALSE(f.g().receiveCommand(0,good,111));
}
TEST_CASE("Group swaps, enables and clears work with autonomous loops and wrap epochs") {
    Groups f;REQUIRE(f.c.start().ok());
    const core::Ref signal{fx::kTypeId,0,fx::kPosition};
    // Looser loop periods make desktop scheduling independent of a 1 ms tick.
    const core::SetEntry rate{{defs::kLoopTypeId,0,defs::loop::kRateHz},100};uint32_t applied=0;
    REQUIRE(f.c.params().set({&rate,1},{&applied,1}).ok());
    REQUIRE(f.g().configure(sub,0,0,1,{&signal,1}).ok());
    REQUIRE(f.g().enable(0,false).ok());CHECK_FALSE(f.g().info(sub,0)->enabled);
    REQUIRE(f.g().enable(0,true).ok());
    REQUIRE(f.g().clear(sub,0).ok());CHECK_FALSE(f.g().info(sub,0)->configured);
    // Empty clears complete without a loop; epoch is deliberately a wrapping u8.
    for(int i=0;i<252;++i) { REQUIRE(f.g().clear(sub,0).ok()); }
    CHECK(f.g().info(sub,0)->epoch==0);
    f.c.stop();
}
TEST_CASE("Group RX and telemetry slots remain coherent under concurrent use") {
    Groups f;
    const std::array<core::Ref,2> refs{{{fx::kTypeId,0,fx::kVelocitySp},{fx::kTypeId,0,fx::kRaw}}};
    REQUIRE(f.install(cmd,0,0,refs).ok());REQUIRE(f.install(sub,0,0,refs).ok());
    std::atomic<bool> done{false};
    std::thread rx([&] {
        const auto payload=frame(1,{testmod::f(6.F),123});
        while(!done.load()) { f.g().receiveCommand(0,payload,101); }
    });
    std::array<uint8_t,core::kGroupStreamBytes> bytes{};
    for(int i=0;i<1000;++i) {
        { core_os::RealtimeScope guard(true);f.g().beginTick(0,true,100);f.g().endTick(0,200); }
        const auto n=f.g().takeStream(0,bytes);
        if(n) {
            drivelink::protocol::wire::Reader r(bytes.data()+12,n-12);
            const auto first=r.readU32Le(),second=r.readU32Le();
            CHECK(((first==0 && second==0) || (first==testmod::f(6.F) && second==123)));
        }
    }
    done.store(true);rx.join();
}
TEST_CASE("Group wire rejects malformed requests before any mutation and pages resources") {
    Groups f; core::CommandService service(f.c,{});std::array<uint8_t,1024> out{};
    const std::array<uint8_t,6> list{3,1,5,6,0,0};
    const auto n=service.handle(list,out);REQUIRE(n>14);CHECK(out[4]==0);CHECK(out[6]==8);CHECK(out[7]==8);
    const auto too_small=service.handle(list,{out.data(),14});CHECK(too_small==6);CHECK(out[5]==static_cast<uint8_t>(core::Detail::BufferTooSmall));
    const std::array<uint8_t,13> config{3,1,5,1,0,0,1,0,1,fx::kTypeId,0,0,1};
    for(size_t length=4;length<config.size();++length) {
        CHECK(service.handle({config.data(),length},out)==6);CHECK(out[4]!=0);
    }
    CHECK_FALSE(f.g().info(sub,0)->configured);
}

TEST_CASE("Groups support full 64-entry payloads, sparse loop ids and boot-only lifetime") {
    reset();core_os::KvStore store;core::Core c(testConfig(32));
    std::vector<Inst> layout{{defs::kLoopTypeId,19,core::kNoLoop,"sparse",{{defs::loop::kRealtime,0}}}};
    std::vector<core::Ref> refs;
    for(uint8_t i=0;i<32;++i) {
        layout.push_back({fx::kTypeId,i,19,"m"+std::to_string(i)});
        refs.push_back({fx::kTypeId,i,fx::kVelocitySp});refs.push_back({fx::kTypeId,i,fx::kRaw});
    }
    bootWith(c,store,layout);
    REQUIRE(c.groups().stageConfig(cmd,7,19,1,refs).ok());
    c.loop(19).runTick(1);REQUIRE(c.groups().complete(cmd,7,0).ok());
    REQUIRE(c.groups().stageConfig(sub,7,19,1,refs).ok());
    c.loop(19).runTick(1);REQUIRE(c.groups().complete(sub,7,0).ok());
    std::array<uint8_t,core::kGroupStreamBytes> bytes{};
    CHECK(c.groups().takeStream(7,bytes)==268);
    refs.push_back(refs[0]);CHECK(c.groups().stageConfig(sub,7,19,1,refs).detail==core::Detail::BatchTooLarge);
    std::vector<uint8_t> command(260);command[0]=3;command[1]=1;
    REQUIRE(c.groups().receiveCommand(7,command,101));
    { core_os::RealtimeScope guard(true);c.groups().beginTick(19,true,100);c.groups().endTick(19,102); }
    REQUIRE(c.save().ok());
    core::Core reboot(testConfig(32));REQUIRE(bootCore(reboot,store).ok());
    CHECK_FALSE(reboot.groups().info(sub,7)->configured);CHECK_FALSE(reboot.groups().info(cmd,7)->configured);
    CHECK(reboot.groups().info(sub,7)->epoch==0);
}
