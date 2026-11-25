#include "doctest.h"
#include "core/command_service.hpp"
#include "core/cpu_load.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/boot_info.hpp"
#include "mod_mock/modules.hpp"
#include "drivelink/protocol/gen/types.hpp"
#include <vector>
#include <thread>
#include <chrono>

namespace {
struct Fixture {
    core_os::KvStore store;
    core::Core core{core::CoreConfig{}};
    core::CommandService service{core,{}};
    std::array<uint8_t,1024> out{};
    Fixture() { core_os::storeCrashCount(0); REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok()); }
    size_t call(std::initializer_list<uint8_t> bytes,size_t cap=1024) {
        const std::vector<uint8_t> p(bytes);return service.handle(p,{out.data(),cap});
    }
};
}
TEST_CASE("Core wire rejects malformed commands before mutation") {
    Fixture f;
    CHECK(f.call({})==6);CHECK(f.out[2]==255);CHECK(f.out[3]==255);
    CHECK(f.call({1,7,2,2,112,0,1,'a'})==6);CHECK(f.core.pendingLayout().entries().empty());
    CHECK(f.call({drivelink::protocol::core_wire::kVersion,7,2,2,112,0,1,'a',0})==6);CHECK(f.out[4]!=0);
    CHECK(f.call({drivelink::protocol::core_wire::kVersion,7,2,2,112,0,1,'a'},6)==6);CHECK(f.core.pendingLayout().entries().empty());
    CHECK(f.call({drivelink::protocol::core_wire::kVersion,7,2,2,112,255,1,'a'})==7);CHECK(f.out[4]==0);CHECK(f.out[6]==0);
    CHECK(f.call({drivelink::protocol::core_wire::kVersion,7,4,3,1,112,0,2,0})==11);
    CHECK(f.out[4]==0);CHECK(f.out[9]==0x20);CHECK(f.out[10]==0x41);
    CHECK(f.call({drivelink::protocol::core_wire::kVersion,7,2,1,0,0,0})>11);CHECK(f.out[6]==2); // factory loop records
    CHECK(f.call({drivelink::protocol::core_wire::kVersion,7,2,1,1,0,0})>11);CHECK(f.out[6]==3); // factory loops and pending mock
    CHECK(f.call({drivelink::protocol::core_wire::kVersion,7,2,1,1,4,0})==6);CHECK(f.out[4]!=0); // invalid cursor
}
TEST_CASE("Every wire reboot path is refused while armed") {
    Fixture f;
    f.core.params().setArmed(true);
    for(uint8_t op : {uint8_t{7},uint8_t{8},uint8_t{9}}) {
        CHECK(f.call({drivelink::protocol::core_wire::kVersion,1,1,op})==6);
        CHECK(f.out[4]==static_cast<uint8_t>(core::Result::Armed));
        CHECK(f.service.takeAfterReply()==core::AfterReply::None);
    }
    CHECK(f.call({drivelink::protocol::core_wire::kVersion,1,1,254})==6);
    CHECK(f.out[5]==static_cast<uint8_t>(core::Detail::Unsupported));
    CHECK(f.service.takeAfterReply()==core::AfterReply::None);
}
TEST_CASE("Core describe fits whole descriptors and makes paging progress") {
    Fixture f;
    for(const auto *type:drivelink::protocol::defs::kTypeTable) {
        size_t cursor=0;
        do {
            const uint8_t lo=static_cast<uint8_t>(cursor),hi=static_cast<uint8_t>(cursor>>8);
            const auto n=f.call({drivelink::protocol::core_wire::kVersion,9,3,2,type->type_id,lo,hi},drivelink::protocol::defs::kDescriptorMinPayload);
            REQUIRE(f.out[4]==0);REQUIRE(n>11);
            const size_t next=f.out[8]|(f.out[9]<<8);
            REQUIRE((next==65535 || next>cursor));
            cursor=next;
        } while(cursor!=65535);
        for(const auto &item:type->items) {
            std::array<uint8_t,1024> bytes{};drivelink::protocol::wire::Writer w(bytes.data(),bytes.size());
            drivelink::protocol::core_wire::item(w,item);
            CHECK(w.ok());CHECK(w.size()==drivelink::protocol::core_wire::itemSize(item));
        }
    }
}
TEST_CASE("Mock actions run through loop and paused handoffs after save and reboot") {
    Fixture f;
    uint8_t instance=255;REQUIRE(f.core.addInstance(112,0,"motor",instance).ok());REQUIRE(f.core.save().ok());
    core::Core rebooted{core::CoreConfig{}};
    REQUIRE(rebooted.init(mod_mock::kTypes).ok());REQUIRE(rebooted.boot(core::prepareBoot(f.store)).ok());const core::SetEntry rate{{drivelink::protocol::defs::kLoopTypeId,0,1},100};uint32_t applied=0;
    REQUIRE(rebooted.params().set({&rate,1},{&applied,1}).ok());REQUIRE(rebooted.start().ok());
    core::ActionArgs args;args.values[0]=drivelink::protocol::defs::f32Bits(.5F);core::ActionOut out;
    REQUIRE(rebooted.runAction({112,0,768},args,out).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    REQUIRE(rebooted.runAction({112,0,768},args,out).ok());
    CHECK(drivelink::protocol::defs::f32FromBits(out.values[0])==0); // disarmed actions cannot drive a mock
    args={};const uint8_t data[]={1,2,3};args.blob=data;
    REQUIRE(rebooted.runAction({112,0,769},args,out).ok());CHECK(out.blob_size==3);CHECK(out.blob[2]==3);
    CHECK(rebooted.loop(0).actionSlot().state.load()==core::ActionState::Empty);
    args={};args.values[0]=0x7FC00000;
    CHECK_FALSE(rebooted.runAction({112,0,768},args,out).accepted());
    rebooted.params().setArmed(true);args={};
    CHECK(rebooted.runAction({112,0,769},args,out).result==core::Result::Armed);
}

TEST_CASE("Action completion events are wake hints, not timeout evidence") {
    Fixture f;uint8_t instance=0;
    REQUIRE(f.core.addInstance(114,1,"imu",instance).ok());REQUIRE(f.core.save().ok());
    core::Core c{core::CoreConfig{}};REQUIRE(c.init(mod_mock::kTypes).ok());REQUIRE(c.boot(core::prepareBoot(f.store)).ok());
    const core::SetEntry rate{{drivelink::protocol::defs::kLoopTypeId,1,1},10};uint32_t applied=0;
    REQUIRE(c.params().set({&rate,1},{&applied,1}).ok());REQUIRE(c.start().ok());
    auto &slot=c.loop(1).actionSlot();
    std::atomic<bool> waiting{false},finished{false},injected{false};
    std::thread stale([&] {
        waiting.store(true);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!finished.load() && std::chrono::steady_clock::now()<deadline) {
            if(slot.state.load(std::memory_order_acquire)==core::ActionState::Ready) {
                slot.completed.signal();injected.store(true);return;
            }
            std::this_thread::yield();
        }
    });
    while(!waiting.load()) { std::this_thread::yield(); }
    core::ActionOut out;const auto st=c.runAction({114,0,768},{},out);
    finished.store(true);stale.join();
    CHECK(st.ok());CHECK(injected.load());
}

TEST_CASE("SET and ACTION envelopes reject wrong counts and trailing bytes") {
    Fixture f;
    const auto before=f.core.nvsDirty();
    f.call({drivelink::protocol::core_wire::kVersion,1,4,2,1,0,0,1,0});CHECK(f.out[4]!=0);
    f.call({drivelink::protocol::core_wire::kVersion,1,4,2,0,42});CHECK(f.out[4]!=0);
    f.call({drivelink::protocol::core_wire::kVersion,1,4,1,65});CHECK(f.out[4]!=0);
    CHECK(f.core.nvsDirty()==before);
    f.call({drivelink::protocol::core_wire::kVersion,1,8,1,112,0,0,3});CHECK(f.out[4]!=0);
    std::array<uint8_t,27> action{drivelink::protocol::core_wire::kVersion,1,8,1,112,0,0,3};
    action[24]=2; // declares two blob bytes, but only one follows
    f.service.handle(action,f.out);
    CHECK(f.out[4]==static_cast<uint8_t>(core::Result::InvalidArgument));
    CHECK(f.out[5]==static_cast<uint8_t>(core::Detail::BadValue));
    f.call({drivelink::protocol::core_wire::kVersion,1,4,3,1,0,0,3,0});REQUIRE(f.out[4]==0);CHECK(f.out[6]==1);
}

TEST_CASE("CPU load samples idle deltas with wrap and clamps delayed accounting") {
    core::CpuLoadSampler cpu;
    CHECK(cpu.sample(0xFFFFFFF0,{0xFFFFFFF0,0xFFFFFFF0})[0]==255);
    const auto load=cpu.sample(84,{34,64}); // elapsed 100; idle deltas 50,80
    CHECK(load[0]==50);CHECK(load[1]==20);
    CHECK(cpu.sample(184,{234,64})[0]==0); // delayed accounting exceeds interval
    CHECK(cpu.sample(184,{234,64})[0]==255); // no elapsed interval
}
TEST_CASE("Pure diagnostic counter bridge keeps instance identity") {
    core_os::native::resetRecordedDiag();
    const auto code=drivelink::protocol::defs::diag::codes::uart_comm::kCrcError;
    core_os::countDiag(code,3,2);core_os::countDiag(code,4,3);
    CHECK(core_os::native::counterTotal(code,2)==3);
    CHECK(core_os::native::counterTotal(code,3)==4);
    CHECK(core_os::native::counterTotal(code)==0);
}


TEST_CASE("SET_NAME validates the entire request before applying and GET_INFO exposes identity")
{
    Fixture f;
    constexpr auto v = drivelink::protocol::core_wire::kVersion;
    CHECK(f.call({v,1,1,10,3,'b','o','t'}) == 6); REQUIRE(f.out[4] == 0);
    CHECK(std::string(f.core.name()) == "bot"); CHECK(f.core.nvsDirty());
    for (const auto &request : {std::vector<uint8_t>{v,1,1,10},
            std::vector<uint8_t>{v,1,1,10,2,'x'},
            std::vector<uint8_t>{v,1,1,10,1,'x',0},
            std::vector<uint8_t>{v,1,1,10,1,0},
            std::vector<uint8_t>{v,1,1,10,2,0xC0,0xAF}}) {
        CHECK(f.service.handle(request, f.out) == 6);
        CHECK(f.out[4] == static_cast<uint8_t>(core::Result::InvalidArgument));
        CHECK(std::string(f.core.name()) == "bot");
    }
    f.core.params().setArmed(true);
    CHECK(f.call({v,1,1,10,0}) == 6); CHECK(f.out[4] == 0);
    CHECK(std::string(f.core.name()).empty());
    core::DeviceInfo info; info.device_id = {2,4,6,8,10,12};
    core::CommandService service(f.core, info);
    const std::array<uint8_t,4> request{v,1,1,1};
    const auto n = service.handle(request, f.out);
    REQUIRE(n >= 13); CHECK(f.out[4] == 0);
    CHECK(std::equal(info.device_id.begin(), info.device_id.end(), f.out.begin() + n - 11));
    CHECK(f.out[n-1] == 0);
}
