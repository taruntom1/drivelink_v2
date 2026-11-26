#include "mod_imu_bno055/driver.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <array>

namespace mod_imu_bno055 {
namespace {
constexpr core_os::Err kInvalid = -1;
int32_t signed16(const uint8_t *p) {
    drivelink::protocol::wire::Reader r(p, 2); return r.readI16Le();
}
}
core_os::Err Driver::writeByte(uint8_t reg, uint8_t value) { return m_bus.write(reg, {&value,1}); }

core_os::Err Driver::init()
{
    // Datasheet: startup to CONFIG 400 ms, reset to CONFIG 650 ms. Permit
    // reset/startup readiness with at most 700 ms of delays plus fourteen
    // bounded transfers. Select page zero even after a warm MCU reboot.
    m_bus.delayMs(400);
    uint8_t chip = 0;
    core_os::Err err = kInvalid;
    for (unsigned i = 0; i < 7; ++i) {
        err = writeByte(0x07, 0);
        if (!err) { err = m_bus.read(0, {&chip,1}); }
        if (err == 0 && chip == 0xA0) { break; }
        if (i == 6) { return err ? err : kInvalid; }
        m_bus.delayMs(50);
    }
    err = writeByte(0x07, 0); // PAGE_ID
    if (err) { return err; }
    uint8_t mode = 0;
    err = m_bus.read(0x3D, {&mode,1});
    if (err || mode > 12) { return err ? err : kInvalid; }
    m_mode = mode;
    if ((err = setMode(0))) { return err; }
    if ((err = writeByte(0x3E, 0))) { return err; } // normal power
    // m/s2, rad/s, radians, Celsius, Windows orientation convention.
    if ((err = writeByte(0x3B, 0x06))) { return err; }
    if ((err = writeByte(0x41, 0x24))) { return err; } // native X/Y/Z mapping
    if ((err = writeByte(0x42, 0))) { return err; } // native signs
    if ((err = writeByte(0x40, 0))) { return err; } // accelerometer temperature
    return 0;
}

core_os::Err Driver::setMode(uint8_t mode)
{
    if (mode > 12) { return kInvalid; }
    if (m_mode > 12) {
        // A failed write can still reach the sensor. An explicit later mode
        // action re-reads hardware and waits the longest settling interval.
        m_bus.delayMs(19);
        uint8_t actual = 255;
        const auto err = m_bus.read(0x3D, {&actual,1});
        if (err || actual > 12) { return err ? err : kInvalid; }
        m_mode = actual;
    }
    if (mode == m_mode) { return waitMode(mode); }
    if (m_mode != 0) {
        const auto err = writeByte(0x3D, 0);
        if (err) { m_mode = 255; return err; }
        m_bus.delayMs(19); m_mode = 0;
    }
    if (mode != 0) {
        const auto err = writeByte(0x3D, mode);
        if (err) { m_mode = 255; return err; }
        m_bus.delayMs(7); m_mode = mode;
    }
    return waitMode(mode);
}

core_os::Err Driver::waitMode(uint8_t expected)
{
    // Mode settling does not prove internal initialization succeeded.
    // Poll successful but initializing status for at most 500 ms of delays;
    // a bus error or SYS_ERR fails immediately. No such polling in a tick.
    for (unsigned attempt = 0; attempt <= 50; ++attempt) {
        std::array<uint8_t,5> health{}; // SYS_STATUS .. OPR_MODE
        const auto err = m_bus.read(0x39, health);
        if (err) { m_mode = 255; return err; }
        m_mode = health[4] & 15;
        // SYS_ERR is meaningful only while SYS_STATUS reports an error (1);
        // otherwise it can hold a stale code (seen: 0x09 while fusing).
        if (m_mode != expected || health[0] == 1 || health[0] > 6 ||
            (expected && (health[2] & 0x97) != 6)) { return kInvalid; }
        // In CONFIG, some firmware retains the last running status; mode 0
        // with no internal error is authoritative once init/self-test ends.
        if ((expected == 0 && (health[0] == 0 || health[0] >= 5)) ||
            (expected && health[0] == (expected >= 8 ? 5 : 6))) { return 0; }
        if (attempt == 50) { return kInvalid; }
        m_bus.delayMs(10);
    }
    return kInvalid;
}

core_os::Err Driver::readSample(Sample &out)
{
    if (m_mode == 0 || m_mode > 12) { return kInvalid; }
    std::array<uint8_t,54> bytes{}; // 0x08..0x3D: data plus status, units and mode
    const auto err = m_bus.read(0x08, bytes);
    if (err) { return err; }
    const auto expected = m_mode;
    m_mode = bytes[53] & 15;
    if (m_mode != expected || bytes[49] != (expected >= 8 ? 5 : 6) ||
        (bytes[51] & 0x97) != 6) { return kInvalid; }
    Sample next;
    for (size_t i = 0; i < 3; ++i) {
        next.accel[i] = static_cast<float>(signed16(&bytes[i*2])) / 100.F;
        next.mag[i] = static_cast<float>(signed16(&bytes[6+i*2])) / 16.F;
        next.gyro[i] = static_cast<float>(signed16(&bytes[12+i*2])) / 900.F;
        next.linear_accel[i] = static_cast<float>(signed16(&bytes[32+i*2])) / 100.F;
        next.gravity[i] = static_cast<float>(signed16(&bytes[38+i*2])) / 100.F;
    }
    next.yaw = static_cast<float>(signed16(&bytes[18])) / 900.F;
    next.roll = static_cast<float>(signed16(&bytes[20])) / 900.F;
    next.pitch = static_cast<float>(signed16(&bytes[22])) / 900.F;
    for (size_t i = 0; i < 4; ++i) { next.quaternion[i] = static_cast<float>(signed16(&bytes[24+i*2])) / 16384.F; }
    next.temperature = static_cast<int8_t>(bytes[44]);
    for (size_t i = 0; i < 4; ++i) { next.calibration[i] = (bytes[45] >> (6-2*i)) & 3; }
    out = next;
    return 0;
}

core_os::Err Driver::calibration(Calibration &cal, bool write)
{
    if (m_mode > 12) { return kInvalid; }
    const uint8_t previous = m_mode;
    auto err = setMode(0);
    if (err) { return err; }
    std::array<uint8_t,22> offsets{};
    std::array<uint8_t,18> sic{};
    // Register order is accel xyz, mag xyz, gyro xyz, accel radius, mag radius.
    constexpr size_t order[]{0,1,2,7,8,9,4,5,6,3,10};
    if (write) {
        drivelink::protocol::wire::Writer w(offsets.data(), offsets.size());
        for (const auto i : order) { w.appendI16Le(static_cast<int16_t>(cal.values[i])); }
        drivelink::protocol::wire::Writer s(sic.data(), sic.size());
        for (size_t i = 11; i < 20; ++i) { s.appendI16Le(static_cast<int16_t>(cal.values[i])); }
        err = m_bus.write(0x55, offsets);
        if (!err && cal.has_sic) { err = m_bus.write(0x43, sic); }
    } else {
        err = m_bus.read(0x55, offsets);
        if (!err) { err = m_bus.read(0x43, sic); }
        if (!err) {
            for (size_t i = 0; i < 11; ++i) { cal.values[order[i]] = signed16(&offsets[i*2]); }
            for (size_t i = 0; i < 9; ++i) { cal.values[11+i] = signed16(&sic[i*2]); }
            cal.valid = true; cal.has_sic = true;
            if (cal.values[3] < 0 || cal.values[10] < 0) { err = kInvalid; }
        }
    }
    const auto restore = setMode(previous);
    return err ? err : restore;
}

core_os::Err Driver::readCalibration(Calibration &out)
{
    Calibration next;
    const auto err = calibration(next, false);
    if (!err) { out = next; }
    return err;
}

core_os::Err Driver::applyCalibration(const Calibration &cal)
{
    std::array<uint8_t,kCalibrationBlobBytes> checked{};
    if (!cal.valid || !encodeCalibration(cal, checked)) { return kInvalid; }
    auto next = cal;
    return calibration(next, true);
}
} // namespace mod_imu_bno055
