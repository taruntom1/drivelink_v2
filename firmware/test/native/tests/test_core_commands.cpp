#include "doctest.h"
#include "core/command_service.hpp"
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
    Fixture() { core_os::storeCrashCount(0); REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(store).ok()); }
    size_t call(std::initializer_list<uint8_t> bytes,size_t cap=1024) {
        const std::vector<uint8_t> p(bytes);return service.handle(p,{out.data(),cap});
    }
};
}
TEST_CASE("Core wire rejects malformed commands before mutation") {
    Fixture f;
    CHECK(f.call({})==6);CHECK(f.out[2]==255);CHECK(f.out[3]==255);
    CHECK(f.call({2,7,2,2,112,0,1,'a'})==6);CHECK(f.core.pendingLayout().entries().empty());
    CHECK(f.call({1,7,2,2,112,0,1,'a',0})==6);CHECK(f.out[4]!=0);
    CHECK(f.call({1,7,2,2,112,0,1,'a'},6)==6);CHECK(f.core.pendingLayout().entries().empty());
    CHECK(f.call({1,7,2,2,112,255,1,'a'})==7);CHECK(f.out[4]==0);CHECK(f.out[6]==0);
    CHECK(f.call({1,7,2,1,0,0,0})==11);CHECK(f.out[6]==0); // configured list empty
    CHECK(f.call({1,7,2,1,1,0,0})>11);CHECK(f.out[6]==1); // pending has mock
    CHECK(f.call({1,7,2,1,1,2,0})==6);CHECK(f.out[4]!=0); // invalid cursor
}
TEST_CASE("Core describe fits whole descriptors and makes paging progress") {
    Fixture f;
    for(const auto *type:drivelink::protocol::defs::kTypeTable) {
        size_t cursor=0;
        do {
            const uint8_t lo=static_cast<uint8_t>(cursor),hi=static_cast<uint8_t>(cursor>>8);
            const auto n=f.call({1,9,3,2,type->type_id,lo,hi},drivelink::protocol::defs::kDescriptorMinPayload);
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
    REQUIRE(rebooted.init(mod_mock::kTypes).ok());REQUIRE(rebooted.boot(f.store).ok());const core::SetEntry rate{{0,0,1},100};uint32_t applied=0;
    REQUIRE(rebooted.params().set({&rate,1},{&applied,1}).ok());REQUIRE(rebooted.start().ok());
    core::ActionArgs args;args.values[0]=drivelink::protocol::defs::f32Bits(.5F);core::ActionOut out;
    REQUIRE(rebooted.runAction({112,0,768},args,out).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    REQUIRE(rebooted.runAction({112,0,768},args,out).ok());
    CHECK(drivelink::protocol::defs::f32FromBits(out.values[0])>0);
    args={};const uint8_t data[]={1,2,3};args.blob=data;
    REQUIRE(rebooted.runAction({112,0,769},args,out).ok());CHECK(out.blob_size==3);CHECK(out.blob[2]==3);
    CHECK(rebooted.loop(0).actionSlot().state.load()==core::ActionState::Empty);
    args={};args.values[0]=0x7FC00000;
    CHECK_FALSE(rebooted.runAction({112,0,768},args,out).accepted());
    rebooted.params().setArmed(true);args={};
    CHECK(rebooted.runAction({112,0,769},args,out).result==core::Result::Armed);
}
