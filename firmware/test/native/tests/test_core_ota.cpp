#include "doctest.h"
#include "core/core.hpp"
#include "core/command_service.hpp"
#include "core/persistence.hpp"
#include "core_os/native_ota.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/native_task.hpp"
#include "core_os/native_diag.hpp"
#include "mod_mock/modules.hpp"
#include "../../../../protocol/test/ota_fixture.hpp"
#include <filesystem>
#include <fstream>
#include <cstring>

namespace p=drivelink::protocol;
namespace {
struct Manual {
    Manual() { core_os::native::setManualScheduling(true);core_os::native::resetBootInfo();core_os::native::resetRecordedDiag(); }
    ~Manual() { core_os::native::setManualScheduling(false);core_os::native::setOtaDirectory({});core_os::native::resetRecordedDiag(); }
};
struct Fixture {
    Manual manual;core_os::KvStore store;core::Core core{{}};core::CommandService service{core,{}};
    std::vector<uint8_t> image=otaFixture();
    Fixture() { REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(store).ok());REQUIRE(core.start().ok()); }
    void begin() { REQUIRE(core.ota().begin(static_cast<uint32_t>(image.size()),p::sha256(image)).ok()); }
    void upload() {
        begin();for (uint32_t i=0;i<image.size();) {
            const auto n=std::min<size_t>(512,image.size()-i);REQUIRE(core.ota().write(i,std::span(image).subspan(i,n)).ok());i+=static_cast<uint32_t>(n);
        }
    }
};
}
TEST_CASE("OTA offset retries do not advance or rehash duplicate bytes") {
    Fixture f;auto &ota=f.core.ota();f.begin();
    CHECK(f.core.arm().detail==core::Detail::OtaActive);CHECK(f.core.blocksArm());
    CHECK(ota.finish().detail==core::Detail::WrongState);
    CHECK(ota.write(1,std::span(f.image).first(512)).detail==core::Detail::OtaOffset);
    REQUIRE(ota.write(0,std::span(f.image).first(512)).ok());CHECK(ota.offset()==512);
    REQUIRE(ota.write(0,std::span(f.image).first(512)).ok());CHECK(ota.offset()==512);
    auto changed=std::vector<uint8_t>(f.image.begin(),f.image.begin()+512);changed[44]^=1;
    CHECK(ota.write(0,changed).detail==core::Detail::OtaOffset);
    CHECK(ota.write(256,std::span(f.image).first(512)).detail==core::Detail::OtaOffset);
    CHECK(ota.write(UINT32_MAX,std::span(f.image).first(1)).detail==core::Detail::OtaOffset);
    CHECK(ota.write(512,{}).detail==core::Detail::BadValue);
    REQUIRE(ota.begin(static_cast<uint32_t>(f.image.size()),p::sha256(f.image)).ok());CHECK(ota.offset()==512);
    CHECK(ota.begin(100,{}).detail==core::Detail::OtaActive);
    CHECK(f.core.save().detail==core::Detail::OtaActive);CHECK(f.core.revert().detail==core::Detail::OtaActive);
    CHECK(f.core.factoryReset().detail==core::Detail::OtaActive);CHECK(f.core.discardPending().detail==core::Detail::OtaActive);
    for (uint32_t i=512;i<f.image.size();) {
        const auto n=std::min<size_t>(512,f.image.size()-i);REQUIRE(ota.write(i,std::span(f.image).subspan(i,n)).ok());i+=static_cast<uint32_t>(n);
    }
    REQUIRE(ota.finish().ok());REQUIRE(ota.finish().ok());CHECK(ota.state()==p::defs::OtaState::Ready);
    REQUIRE(ota.abort().ok());CHECK_FALSE(f.core.blocksArm());CHECK(ota.bootInfo().selected==ota.bootInfo().running);
    CHECK(f.core.arm().detail==core::Detail::NoCommandGroup);REQUIRE(ota.abort().ok());
}
TEST_CASE("OTA malformed wire and short response buffers never mutate storage") {
    Fixture f;std::array<uint8_t,1024> request{},response{};
    p::wire::Writer w(request.data(),request.size());w.appendU8(p::core_wire::kVersion);w.appendU8(3);w.appendU8(9);w.appendU8(1);w.appendU32Le(static_cast<uint32_t>(f.image.size()));
    for (auto b:p::sha256(f.image)) { w.appendU8(b); }
    const auto n=w.size();
    CHECK(f.service.handle({request.data(),n},{response.data(),9})==6);CHECK(f.core.ota().state()==p::defs::OtaState::Idle);
    CHECK(f.service.handle({request.data(),n-1},response)==6);CHECK(f.core.ota().state()==p::defs::OtaState::Idle);
    REQUIRE(f.service.handle({request.data(),n},response)==10);CHECK(response[4]==0);
    w=p::wire::Writer(request.data(),request.size());w.appendU8(p::core_wire::kVersion);w.appendU8(4);w.appendU8(9);w.appendU8(2);w.appendU32Le(7);w.appendU16Le(1);w.appendU8(42);
    CHECK(f.service.handle({request.data(),w.size()},response)==10);CHECK(response[5]==static_cast<uint8_t>(core::Detail::OtaOffset));
    p::wire::Reader r(response.data()+6,4);CHECK(r.readU32Le()==0);CHECK(f.core.ota().offset()==0);
    request[8]=0;request[9]=0;CHECK(f.service.handle({request.data(),w.size()},response)==6);CHECK(f.core.ota().offset()==0);
}
TEST_CASE("OTA backend errors retain ARM gate until verified cleanup") {
    for (const auto *operation:{"begin","write","read","finish","select","abort","info"}) {
        CAPTURE(operation);Fixture f;auto &ota=f.core.ota();
        if (std::string(operation)=="begin") { core_os::native::failOtaOperation(ota.platform(),operation);CHECK_FALSE(ota.begin(1,{}).ok()); }
        else if (std::string(operation)=="info") { core_os::native::failOtaOperation(ota.platform(),operation);CHECK_FALSE(ota.refresh().ok()); }
        else {
            f.upload();core_os::native::failOtaOperation(ota.platform(),operation);
            if (std::string(operation)=="write") { REQUIRE(ota.abort().ok());f.begin();CHECK_FALSE(ota.write(0,std::span(f.image).first(1)).ok()); }
            else if (std::string(operation)=="read") { CHECK_FALSE(ota.write(0,std::span(f.image).first(1)).ok()); }
            else if (std::string(operation)=="abort") { CHECK_FALSE(ota.abort().ok()); }
            else { CHECK_FALSE(ota.finish().ok()); }
        }
        CHECK(ota.blocksArm());CHECK(ota.state()==p::defs::OtaState::Error);REQUIRE(ota.abort().ok());CHECK_FALSE(ota.blocksArm());
    }
}
TEST_CASE("OTA rejects wrong digest and invalid image without selecting boot") {
    Fixture f;auto &ota=f.core.ota();REQUIRE(ota.begin(static_cast<uint32_t>(f.image.size()),{}).ok());
    for (uint32_t i=0;i<f.image.size();) { const auto n=std::min<size_t>(512,f.image.size()-i);REQUIRE(ota.write(i,std::span(f.image).subspan(i,n)).ok());i+=static_cast<uint32_t>(n); }
    CHECK(ota.finish().detail==core::Detail::OtaHash);CHECK(ota.bootInfo().selected==0);REQUIRE(ota.abort().ok());
    f.image[0]=0;f.upload();CHECK_FALSE(ota.finish().ok());CHECK(ota.bootInfo().selected==0);CHECK(ota.blocksArm());
}
TEST_CASE("Native OTA models confirmed boot rollback and partial upload reset") {
    Manual manual;core_os::Ota ota;core_os::OtaBootInfo info;const auto image=otaFixture();
    auto stage=[&] { REQUIRE(ota.begin(static_cast<uint32_t>(image.size()))==0);REQUIRE(ota.write(image)==0);REQUIRE(ota.finish()==0);REQUIRE(ota.select()==0); };
    stage();core_os::native::rebootOta(ota);REQUIRE(ota.info(info)==0);CHECK(info.running==1);CHECK(info.pending);
    core_os::native::failOtaOperation(ota,"confirm");CHECK(ota.confirm()!=0);core_os::native::rebootOta(ota);
    REQUIRE(ota.info(info)==0);CHECK(info.running==0);CHECK_FALSE(info.pending);
    stage();core_os::native::rebootOta(ota);REQUIRE(ota.confirm()==0);core_os::native::rebootOta(ota);
    REQUIRE(ota.info(info)==0);CHECK(info.running==1);CHECK_FALSE(info.pending);
    REQUIRE(ota.begin(1000)==0);REQUIRE(ota.write(std::span(image).first(100))==0);core_os::native::rebootOta(ota);
    REQUIRE(ota.info(info)==0);CHECK(info.running==1);CHECK_FALSE(info.pending);
}
TEST_CASE("Native OTA persists selection and fails closed on torn metadata") {
    Manual manual;const auto directory=std::filesystem::temp_directory_path()/"drivelink-native-ota-test";
    std::filesystem::remove_all(directory);std::filesystem::create_directories(directory);core_os::native::setOtaDirectory(directory);
    const auto image=otaFixture();
    { core_os::Ota ota;REQUIRE(ota.begin(static_cast<uint32_t>(image.size()))==0);REQUIRE(ota.write(image)==0);REQUIRE(ota.finish()==0);REQUIRE(ota.select()==0); }
    { core_os::Ota ota;core_os::OtaBootInfo info;REQUIRE(ota.info(info)==0);CHECK(info.running==1);CHECK(info.pending); }
    { core_os::Ota ota;core_os::OtaBootInfo info;REQUIRE(ota.info(info)==0);CHECK(info.running==0);CHECK_FALSE(info.pending); }
    { std::ofstream corrupt(directory/"ota.bin",std::ios::binary|std::ios::trunc);corrupt<<"torn"; }
    { core_os::Ota ota;core_os::OtaBootInfo info;CHECK(ota.info(info)!=0);CHECK(ota.abort()!=0); }
    core_os::native::setOtaDirectory({});std::filesystem::remove_all(directory);
}
TEST_CASE("OTA mutations refuse armed devices and remain available in safe mode") {
    SUBCASE("armed") {
        Manual manual;core_os::KvStore store;
        std::array<core::persist::StoredInstance,2> layout{};
        layout[0].type_id=p::defs::kLoopTypeId;layout[0].instance=0;layout[0].loop=255;
        std::strcpy(layout[0].label,"control");layout[0].pairs={{1,1000},{2,1}};
        layout[1].type_id=112;layout[1].instance=0;layout[1].loop=0;std::strcpy(layout[1].label,"motor");
        REQUIRE(core::persist::saveConfig(store,1,layout,{}).ok());
        core::Core c({});REQUIRE(c.init(mod_mock::kTypes).ok());REQUIRE(c.boot(store).ok());
        const core::Ref input{112,0,512};REQUIRE(c.groups().stageConfig(core::GroupKind::Command,0,0,1,{&input,1}).ok());
        c.loop(0).runTick(1);REQUIRE(c.groups().complete(core::GroupKind::Command,0,0).ok());REQUIRE(c.start().ok());REQUIRE(c.arm().ok());
        auto &ota=c.ota();const auto image=otaFixture();
        CHECK(ota.begin(static_cast<uint32_t>(image.size()),p::sha256(image)).result==core::Result::Armed);
        CHECK(ota.write(0,std::span(image).first(1)).result==core::Result::Armed);
        CHECK(ota.finish().result==core::Result::Armed);CHECK(ota.abort().result==core::Result::Armed);CHECK(ota.confirm().result==core::Result::Armed);
        CHECK(ota.refresh().ok());CHECK_FALSE(ota.blocksArm());c.disarm();
    }
    SUBCASE("safe mode") {
        Manual manual;core_os::storeCrashCount(2);core_os::native::setResetWasCrash(true);
        core_os::KvStore store;core::Core c({});REQUIRE(c.init(mod_mock::kTypes).ok());REQUIRE(c.boot(store).ok());
        REQUIRE(c.safeMode());CHECK(c.loopCount()==0);
        const auto image=otaFixture();REQUIRE(c.ota().begin(static_cast<uint32_t>(image.size()),p::sha256(image)).ok());
        REQUIRE(c.ota().abort().ok());CHECK_FALSE(c.ota().blocksArm());
    }
}
