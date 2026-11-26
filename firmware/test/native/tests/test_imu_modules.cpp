#include "doctest.h"
#include "fake_imu_bus.hpp"
#include "mod_imu_bno055/driver.hpp"
#include "mod_imu_bno055/modules.hpp"
#include "mod_mock/modules.hpp"
#include "core/core.hpp"
#include "core/persistence.hpp"
#include "core_os/clock.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_task.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/native_kv.hpp"
#include <atomic>
#include <thread>
#include <chrono>

namespace {
using namespace mod_imu_bno055;
namespace defs = drivelink::protocol::defs;
struct Manual {
    Manual() { core_os::native::setManualScheduling(true); core_os::native::resetBootInfo(); core_os::native::resetRecordedDiag(); fake_imu_bus::reset(); }
    ~Manual() { core_os::native::setManualScheduling(false); }
};
struct DriverFixture {
    Manual manual;
    core_os::Err err = 0;
    std::unique_ptr<Bus> bus = openBus({.sda=8,.scl=9}, err);
    Driver driver{*bus};
    fake_imu_bus::Device &device = fake_imu_bus::devices[0];
};
struct Fixture {
    Manual manual;
    core_os::KvStore store;
    const std::array<const core::ModuleType *,2> types{mod_mock::kTypes[2], &mod_imu_bno055::kType};
    std::unique_ptr<core::Core> c;
    uint8_t type = 114;
    explicit Fixture(bool real = false, std::vector<core::ParamPair> extra = {}, uint8_t sys_error = 0) : type(real ? 48 : 114) {
        std::array<core::persist::StoredInstance,2> layout{};
        layout[0] = {5,1,255,"sensor",{{1,100},{2,0}}};
        layout[1] = {type,0,1,"imu",std::move(extra)};
        if (real) { layout[1].pairs.push_back({1,8}); layout[1].pairs.push_back({2,9}); }
        REQUIRE(core::persist::saveConfig(store,1,layout,{}).ok());
        fake_imu_bus::devices[0].regs[0x3A] = sys_error;
        if (sys_error) { fake_imu_bus::devices[0].regs[0x39] = 1; }
        reboot();
    }
    void reboot() {
        c.reset();
        c = std::make_unique<core::Core>(core::CoreConfig{});
        REQUIRE(c->init(types).ok()); REQUIRE(c->boot(core::prepareBoot(store)).ok()); REQUIRE(c->start().ok());
    }
    void tick() { c->loop(1).runTick(1); }
    uint32_t get(uint16_t id) {
        core::Ref ref{type,0,id}; uint32_t value=0;
        REQUIRE(c->params().get({&ref,1},{&value,1}).ok()); return value;
    }
    uint32_t signal(uint16_t id) {
        auto *r=c->findRecord(type,0); auto i=r->itemIndex(id);
        REQUIRE(r->object != nullptr); REQUIRE(i!=core::detail::kNoItem);
        return core::loadValue(r->desc->items[i].type,r->bindings[i].ptr);
    }
    template <typename Fn> core::Status configure(Fn fn) {
        std::atomic<bool> done{false}; core::Status status;
        std::thread config([&] { status=fn(); done.store(true,std::memory_order_release); });
        while (!done.load(std::memory_order_acquire)) { tick(); std::this_thread::sleep_for(std::chrono::microseconds(100)); }
        config.join(); return status;
    }
    core::Status action(uint16_t id, core::ActionOut &out, std::span<const uint8_t> blob={}, uint32_t value=0) {
        return configure([&] { core::ActionArgs in; in.blob=blob; in.values[0]=value; return c->runAction({type,0,id},in,out); });
    }
};
std::array<uint8_t,kCalibrationBlobBytes> blob(int value=123) {
    Calibration cal; cal.valid=true; cal.has_sic=true; cal.values.fill(value);
    std::array<uint8_t,kCalibrationBlobBytes> bytes{}; REQUIRE(encodeCalibration(cal,bytes)); return bytes;
}
}

TEST_CASE("IMU blob exact validation and atomic decoding") {
    auto bytes=blob(); Calibration cal; cal.values[0]=77;
    CHECK(decodeCalibration(bytes,cal)); CHECK(cal.valid); CHECK(cal.has_sic); CHECK(cal.values[19]==123);
    const auto saved=cal.values;
    for (size_t n=0;n<bytes.size();++n) { CHECK_FALSE(decodeCalibration({bytes.data(),n},cal)); CHECK(cal.values==saved); }
    bytes[0]=2; CHECK_FALSE(decodeCalibration(bytes,cal)); bytes[0]=1;
    bytes[1]=2; CHECK_FALSE(decodeCalibration(bytes,cal)); bytes[1]=0;
    bytes[8]=255; bytes[9]=255; CHECK_FALSE(decodeCalibration(bytes,cal)); // negative radius
}

TEST_CASE("IMU fake bus identity timing sample scales and complete publication") {
    DriverFixture f; REQUIRE(f.driver.init()==0); CHECK(f.device.regs[0x3B]==6);
    REQUIRE(f.driver.setMode(12)==0);
    fake_imu_bus::put16(f.device,0x08,-100); fake_imu_bus::put16(f.device,0x0E,160);
    fake_imu_bus::put16(f.device,0x14,-900); fake_imu_bus::put16(f.device,0x1A,900);
    fake_imu_bus::put16(f.device,0x1C,-1800); fake_imu_bus::put16(f.device,0x1E,450);
    fake_imu_bus::put16(f.device,0x20,16384); fake_imu_bus::put16(f.device,0x22,-8192);
    fake_imu_bus::put16(f.device,0x28,200); fake_imu_bus::put16(f.device,0x2E,981);
    f.device.regs[0x34]=251; f.device.regs[0x35]=0b11100100;
    Sample s; REQUIRE(f.driver.readSample(s)==0);
    CHECK(s.accel[0]==doctest::Approx(-1)); CHECK(s.mag[0]==doctest::Approx(10));
    CHECK(s.gyro[0]==doctest::Approx(-1)); CHECK(s.yaw==doctest::Approx(1));
    CHECK(s.roll==doctest::Approx(-2)); CHECK(s.pitch==doctest::Approx(.5));
    CHECK(s.quaternion[0]==doctest::Approx(1)); CHECK(s.quaternion[1]==doctest::Approx(-.5));
    CHECK(s.linear_accel[0]==doctest::Approx(2)); CHECK(s.gravity[0]==doctest::Approx(9.81));
    CHECK(s.temperature==-5); CHECK(s.calibration==std::array<uint8_t,4>{3,2,1,0});
    f.device.fail_read_reg=0x08; CHECK(f.driver.readSample(s)==f.device.error); CHECK(s.yaw==doctest::Approx(1));
    f.device.fail_read_reg=-1;
    for (unsigned m=0;m<=12;++m) { REQUIRE(f.driver.setMode(m)==0); CHECK(f.driver.mode()==m); CHECK((modeAvailability(m)!=0)==(m!=0)); }
    CHECK(f.device.bad_timing==0); CHECK(f.driver.setMode(13)!=0);
    CHECK((modeAvailability(2)&128)==0); CHECK((modeAvailability(3)&128)==0); CHECK((modeAvailability(12)&128)!=0);
    CHECK(f.device.trace[0].at_ms==400);
}

TEST_CASE("IMU bounded missing sensor wrong identity NACK and page errors") {
    DriverFixture f;
    SUBCASE("timeout") { f.device.fail_read_reg=0; }
    SUBCASE("wrong identity") { f.device.regs[0]=0; }
    SUBCASE("page NACK") { f.device.fail_write_reg=7; }
    CHECK(f.driver.init()!=0); CHECK(f.device.transfers<=14); CHECK(f.device.now_ms<=700);
}

TEST_CASE("IMU calibration register order restore errors and independent buses") {
    DriverFixture f; REQUIRE(f.driver.init()==0); REQUIRE(f.driver.setMode(12)==0);
    Calibration cal; REQUIRE(decodeCalibration(blob(),cal));
    cal.values[0]=-123; cal.values[7]=234; cal.values[4]=-345; cal.values[3]=456;
    REQUIRE(f.driver.applyCalibration(cal)==0); CHECK(f.driver.mode()==12);
    CHECK(f.device.regs[0x55]==static_cast<uint8_t>(-123)); CHECK(f.device.regs[0x5B]==static_cast<uint8_t>(234));
    Calibration read; REQUIRE(f.driver.readCalibration(read)==0); CHECK(read.values==cal.values); CHECK(read.has_sic);
    f.device.fail_read_reg=0x43; read.values[0]=999;
    CHECK(f.driver.readCalibration(read)==f.device.error); CHECK(read.values[0]==999); CHECK(f.driver.mode()==12);
    f.device.fail_read_reg=-1; f.device.fail_write_reg=0x55;
    CHECK(f.driver.applyCalibration(cal)==f.device.error); CHECK(f.driver.mode()==12); CHECK(f.device.bad_timing==0);
    core_os::Err err=0; CHECK_FALSE(openBus({.sda=10,.scl=11},err)); CHECK(err==-106);
    auto other=openBus({.port=1,.sda=10,.scl=11},err); REQUIRE(other); Driver second(*other);
    REQUIRE(second.init()==0); REQUIRE(second.setMode(8)==0); CHECK(f.driver.mode()==12);
    other.reset(); CHECK_FALSE(fake_imu_bus::devices[1].open);
}

TEST_CASE("IMU mock actions persist through SAVE and reboot and reject malformed data") {
    Fixture f; core::ActionOut out;
    CHECK_FALSE(f.action(kCalibrationApply,out).ok());
    auto bytes=blob(); REQUIRE(f.action(kCalibrationWrite,out,bytes).ok());
    CHECK(f.get(64)==1); CHECK(f.get(66)==123); CHECK(f.c->params().nvsDirty());
    bytes[0]=2; CHECK_FALSE(f.action(kCalibrationWrite,out,bytes).ok()); CHECK(f.get(66)==123);
    REQUIRE(f.c->save().ok()); CHECK_FALSE(f.c->params().nvsDirty());
    f.reboot(); CHECK(f.get(66)==123); REQUIRE(f.action(kCalibrationRead,out).ok());
    Calibration read; REQUIRE(decodeCalibration({out.blob.data(),out.blob_size},read)); CHECK(read.values[0]==123);
    bytes=blob(321); REQUIRE(f.action(kCalibrationWrite,out,bytes).ok());
    REQUIRE(f.action(kCalibrationApply,out).ok()); REQUIRE(f.action(kCalibrationRead,out).ok());
    REQUIRE(decodeCalibration({out.blob.data(),out.blob_size},read)); CHECK(read.values[0]==321);
    REQUIRE(f.c->save().ok());
    REQUIRE(f.c->setLabel(f.type,0,"renamed").ok()); // SAVE pending layout must retain live action writes
    bytes=blob(456); REQUIRE(f.action(kCalibrationWrite,out,bytes).ok()); REQUIRE(f.c->save().ok());
    f.reboot(); CHECK(f.get(66)==456);
}

TEST_CASE("IMU real module stale values recovery and restore failure") {
    Fixture f(true); REQUIRE(f.c->instancesFailed()==0); f.tick(); CHECK(f.signal(283)==1); CHECK(f.signal(285)>0);
    auto &device=fake_imu_bus::devices[0]; device.fail_read_reg=8; const auto count=f.signal(285);
    f.tick(); CHECK(f.signal(283)==0); CHECK(f.signal(285)==count); CHECK(f.signal(284)!=UINT32_MAX);
    device.fail_read_reg=-1; f.tick(); CHECK(f.signal(283)==1); CHECK(f.signal(285)==count+1);
    core::ActionOut out; auto bytes=blob(); REQUIRE(f.action(kCalibrationWrite,out,bytes).ok()); REQUIRE(f.c->save().ok());
    f.reboot(); CHECK(f.c->instancesFailed()==0); CHECK(f.get(66)==123); CHECK(device.regs[0x55]==123);
    device.fail_write_reg=0x55; f.reboot(); CHECK(f.c->instancesFailed()==1); CHECK_FALSE(device.open);
    CHECK(f.c->arm().detail==core::Detail::StartFailed);
}

TEST_CASE("IMU paused parameter updates validate all entries before mutation") {
    Fixture f; auto &r=*f.c->findRecord(f.type,0); auto &slot=f.c->loop(1).actionSlot();
    std::array<core::ActionParamUpdate,2> updates{{{66,10},{67,20}}};
    CHECK_FALSE(f.c->params().applyPaused(r,updates).ok()); CHECK(f.get(66)==0);
    slot.target=r.object.get(); slot.state.store(core::ActionState::Paused);
    SUBCASE("success updates bindings shadows and dirty together") {
        REQUIRE(f.c->params().applyPaused(r,updates).ok());
        CHECK(f.get(66)==10); CHECK(f.get(67)==20);
        CHECK(core::loadValue(defs::ValueType::I32,r.bindings[r.itemIndex(66)].ptr)==10);
        CHECK(f.c->params().nvsDirty());
    }
    SUBCASE("unknown id") { updates[1].id=65535; CHECK_FALSE(f.c->params().applyPaused(r,updates).ok()); CHECK(f.get(66)==0); }
    SUBCASE("range rejection is not clamping") { updates[1].bits=32768; CHECK_FALSE(f.c->params().applyPaused(r,updates).ok()); CHECK(f.get(66)==0); }
    SUBCASE("duplicate") { updates[1].id=66; CHECK_FALSE(f.c->params().applyPaused(r,updates).ok()); CHECK(f.get(66)==0); }
    SUBCASE("structural") { updates[1]={6,8}; CHECK_FALSE(f.c->params().applyPaused(r,updates).ok()); CHECK(f.get(66)==0); }
    SUBCASE("signal") { updates[1]={256,0}; CHECK_FALSE(f.c->params().applyPaused(r,updates).ok()); CHECK(f.get(66)==0); }
    SUBCASE("hook remains loop-only") {
        auto &b=r.bindings[r.itemIndex(67)]; b.hook=[](void *,uint16_t) {};
        CHECK_FALSE(f.c->params().applyPaused(r,updates).ok()); CHECK(f.get(66)==0); b.hook=nullptr;
    }
    SUBCASE("wrong instance ownership") { slot.target=nullptr; CHECK_FALSE(f.c->params().applyPaused(r,updates).ok()); CHECK(f.get(66)==0); }
    SUBCASE("bounded update count") {
        std::array<core::ActionParamUpdate,25> too_many{};
        CHECK_FALSE(f.c->params().applyPaused(r,too_many).ok()); CHECK(f.get(66)==0);
    }
    slot.state.store(core::ActionState::Empty);
}

TEST_CASE("IMU calibration torn and failed SAVE retains a complete generation") {
    Fixture f; core::ActionOut out; auto first=blob(123), next=blob(234);
    REQUIRE(f.action(kCalibrationWrite,out,first).ok()); REQUIRE(f.c->save().ok());
    REQUIRE(f.action(kCalibrationWrite,out,next).ok());
    SUBCASE("power loss before layout commit") {
        core_os::native::failWritesAfter(f.store,2);
        CHECK_FALSE(f.c->save().ok()); CHECK(f.c->params().nvsDirty());
    }
    SUBCASE("torn calibration blob") { core_os::native::tearWriteAfter(f.store,1); REQUIRE(f.c->save().ok()); }
    SUBCASE("corrupt calibration blob") { core_os::native::tearWriteAfter(f.store,1,core_os::native::Tear::Corrupt); REQUIRE(f.c->save().ok()); }
    core_os::native::clearFaults(f.store); f.reboot(); CHECK(f.get(66)==123);
    REQUIRE(f.action(kCalibrationRead,out).ok()); Calibration read;
    REQUIRE(decodeCalibration({out.blob.data(),out.blob_size},read)); CHECK(read.values[0]==123);
    REQUIRE(f.action(kCalibrationWrite,out,next).ok()); REQUIRE(f.c->save().ok()); f.reboot(); CHECK(f.get(66)==234);
}

TEST_CASE("IMU REVERT restores staged calibration without applying it") {
    Fixture f; core::ActionOut out; auto first=blob(123), next=blob(234);
    REQUIRE(f.action(kCalibrationWrite,out,first).ok()); REQUIRE(f.action(kCalibrationApply,out).ok()); REQUIRE(f.c->save().ok());
    REQUIRE(f.action(kCalibrationWrite,out,next).ok()); REQUIRE(f.action(kCalibrationApply,out).ok());
    REQUIRE(f.configure([&] { return f.c->revert(); }).ok()); CHECK(f.get(66)==123);
    REQUIRE(f.action(kCalibrationRead,out).ok()); Calibration read;
    REQUIRE(decodeCalibration({out.blob.data(),out.blob_size},read)); CHECK(read.values[0]==234); // sensor still has 234
    REQUIRE(f.configure([&] { return f.c->revert(); }).ok());
    REQUIRE(f.action(kCalibrationApply,out).ok()); REQUIRE(f.action(kCalibrationRead,out).ok());
    REQUIRE(decodeCalibration({out.blob.data(),out.blob_size},read)); CHECK(read.values[0]==123);
}

TEST_CASE("IMU calibration is erased with removed instance and id reuse") {
    Fixture f; core::ActionOut out; auto bytes=blob();
    REQUIRE(f.action(kCalibrationWrite,out,bytes).ok()); REQUIRE(f.c->save().ok());
    REQUIRE(f.c->removeInstance(f.type,0).ok()); REQUIRE(f.c->save().ok()); f.reboot();
    uint8_t fresh=255;
    REQUIRE(f.c->addInstance(f.type,1,"fresh",fresh).ok()); CHECK(fresh==0); REQUIRE(f.c->save().ok()); f.reboot();
    CHECK(f.get(64)==0); CHECK(f.get(66)==0); CHECK_FALSE(f.action(kCalibrationApply,out).ok());
    REQUIRE(core::persist::eraseAll(f.store).ok()); f.reboot(); CHECK(f.c->instancesFailed()==0);
    CHECK(f.c->findRecord(f.type,0)==nullptr);
}

TEST_CASE("IMU mode write uncertainty can be recovered by explicit mode action") {
    DriverFixture f; REQUIRE(f.driver.init()==0); REQUIRE(f.driver.setMode(12)==0);
    f.device.fail_write_reg=0x3D;
    CHECK(f.driver.setMode(8)==f.device.error); CHECK(f.driver.mode()==255);
    f.device.fail_write_reg=-1; REQUIRE(f.driver.setMode(8)==0); CHECK(f.driver.mode()==8);
    CHECK(f.device.bad_timing==0);
}

TEST_CASE("IMU warm startup restores page units axes and CONFIG timing") {
    DriverFixture f; f.device.regs[7]=1; f.device.regs[0x3D]=12;
    f.device.regs[0x3B]=128; f.device.regs[0x41]=0; f.device.regs[0x42]=7;
    f.device.regs[0x40]=1;
    REQUIRE(f.driver.init()==0);
    CHECK(f.device.regs[7]==0); CHECK(f.device.regs[0x3B]==6);
    CHECK(f.device.regs[0x41]==0x24); CHECK(f.device.regs[0x42]==0);
    CHECK(f.device.regs[0x40]==0);
    CHECK(f.driver.mode()==0); CHECK(f.device.bad_timing==0);
}

TEST_CASE("IMU calibration SIC and mode restoration errors leave output unchanged") {
    DriverFixture f; REQUIRE(f.driver.init()==0); REQUIRE(f.driver.setMode(12)==0);
    Calibration cal; REQUIRE(decodeCalibration(blob(),cal));
    f.device.fail_write_reg=0x43;
    CHECK(f.driver.applyCalibration(cal)==f.device.error); CHECK(f.driver.mode()==12);
    f.device.fail_write_reg=-1;
    // Enter CONFIG, verify health, offsets, SIC, then restore-mode write.
    f.device.fail_transfer=f.device.transfers+5;
    cal.values[0]=999;
    CHECK(f.driver.readCalibration(cal)==f.device.error); CHECK(cal.values[0]==999);
    CHECK(f.driver.mode()==255);
    f.device.fail_transfer=0; REQUIRE(f.driver.setMode(12)==0);
    REQUIRE(f.driver.readCalibration(cal)==0); CHECK(cal.values[0]==123);
}

TEST_CASE("IMU readable but unhealthy sensor never publishes a fresh sample") {
    DriverFixture f; REQUIRE(f.driver.init()==0); REQUIRE(f.driver.setMode(12)==0);
    Sample sample; sample.yaw=123;
    SUBCASE("internal error") { f.device.regs[0x39]=1; f.device.regs[0x3A]=7; }
    SUBCASE("still initializing") { f.device.regs[0x39]=3; }
    SUBCASE("sensor-only reset") { f.device.regs[0x39]=0; f.device.regs[0x3D]=0; f.device.regs[0x3B]=128; }
    SUBCASE("units changed") { f.device.regs[0x3B]=128; }
    SUBCASE("unexpected operating mode") { f.device.regs[0x3D]=8; }
    CHECK(f.driver.readSample(sample)!=0); CHECK(sample.yaw==123);
}

TEST_CASE("IMU ignores a stale SYS_ERR unless SYS_STATUS reports a system error") {
    // A real BNO055 kept SYS_ERR 0x09 while running fusion (SYS_STATUS 5).
    DriverFixture f; f.device.regs[0x3A]=9;
    REQUIRE(f.driver.init()==0); REQUIRE(f.driver.setMode(12)==0);
    Sample sample; CHECK(f.driver.readSample(sample)==0);
    f.device.regs[0x39]=1;
    CHECK(f.driver.readSample(sample)!=0);
}

TEST_CASE("IMU ignores reserved OPR_MODE bits") {
    // A real BNO055 read OPR_MODE 0x10 in CONFIG.
    DriverFixture f; f.device.regs[0x3D]=0x10;
    REQUIRE(f.driver.init()==0); CHECK(f.driver.mode()==0);
    REQUIRE(f.driver.setMode(12)==0);
    f.device.regs[0x3D]|=0x10;
    Sample sample; CHECK(f.driver.readSample(sample)==0); CHECK(f.driver.mode()==12);
}

TEST_CASE("IMU initialization health errors gate ARM and readiness waits are bounded") {
    SUBCASE("core start failure") {
        Fixture f(true,{},7); CHECK(f.c->instancesFailed()==1);
        CHECK(f.c->arm().detail==core::Detail::StartFailed); CHECK_FALSE(fake_imu_bus::devices[0].open);
    }
    SUBCASE("initializing status cannot wait indefinitely") {
        DriverFixture f; f.device.regs[0x39]=3;
        CHECK(f.driver.init()!=0); CHECK(f.device.now_ms==900); CHECK(f.device.transfers<60);
    }
}

TEST_CASE("IMU supported bus settings fit the bounded transfer policy") {
    BusConfig c{.sda=8,.scl=9}; CHECK(validBusConfig(c));
    for (auto hz : {100000U,400000U}) {
        for (auto timeout : {10U,20U}) { c.hz=hz; c.timeout_ms=timeout; CHECK(validBusConfig(c)); }
    }
    c.hz=10000; CHECK_FALSE(validBusConfig(c)); c.hz=400001; CHECK_FALSE(validBusConfig(c)); c.hz=400000;
    for (auto timeout : {0U,1U,9U,21U}) { c.timeout_ms=timeout; CHECK_FALSE(validBusConfig(c)); }
}

TEST_CASE("IMU reset and internal errors retain sample count and recover on healthy data") {
    Fixture f(true); f.tick(); const auto count=f.signal(285);
    auto &d=fake_imu_bus::devices[0]; d.regs[0x39]=1; d.regs[0x3A]=7;
    f.tick(); CHECK(f.signal(283)==0); CHECK(f.signal(285)==count);
    d.regs[0x39]=5; d.regs[0x3A]=0;
    f.tick(); CHECK(f.signal(283)==1); CHECK(f.signal(285)==count+1);
    d.regs[0x39]=0; d.regs[0x3D]=0; d.regs[0x3B]=128;
    f.tick(); CHECK(f.signal(283)==0); CHECK(f.signal(287)==0); CHECK(f.signal(285)==count+1);
    f.tick(); CHECK(f.signal(283)==0); CHECK(f.signal(285)==count+1);
}

TEST_CASE("IMU unit validation checks all defined bits and ignores reserved ones") {
    for (uint8_t bit : {0,1,2,4,7}) {
        DriverFixture f; REQUIRE(f.driver.init()==0); REQUIRE(f.driver.setMode(12)==0);
        f.device.regs[0x3B] ^= uint8_t{1} << bit;
        Sample sample; sample.temperature=77;
        CHECK(f.driver.readSample(sample)!=0); CHECK(sample.temperature==77);
        CHECK(f.driver.setMode(12)!=0); // also rejected before a mode action succeeds
    }
    DriverFixture f; REQUIRE(f.driver.init()==0); REQUIRE(f.driver.setMode(12)==0);
    f.device.regs[0x3B] |= 0x68; // reserved bits 3,5,6
    Sample sample; CHECK(f.driver.readSample(sample)==0); CHECK(f.driver.setMode(12)==0);
}
