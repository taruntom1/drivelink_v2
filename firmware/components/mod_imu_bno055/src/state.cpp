#include "mod_imu_bno055/state.hpp"
#include "drivelink/protocol/wire_codec.hpp"
#include <algorithm>
#include <limits>

namespace mod_imu_bno055 {
namespace wire = drivelink::protocol::wire;

core::Status badCalibration() { return core::Status::fail(core::Result::InvalidArgument, core::Detail::BadValue); }

bool decodeCalibration(std::span<const uint8_t> blob, Calibration &out)
{
    if (blob.size() != kCalibrationBlobBytes) { return false; }
    wire::Reader r(blob.data(), blob.size());
    if (r.readU8() != 1) { return false; }
    const auto has_sic = r.readU8();
    if (has_sic > 1) { return false; }
    Calibration next; next.valid = true; next.has_sic = has_sic != 0;
    for (auto &v : next.values) { v = r.readI16Le(); }
    if (!r.done() || next.values[3] < 0 || next.values[10] < 0) { return false; }
    out = next;
    return true;
}

bool encodeCalibration(const Calibration &cal, std::span<uint8_t> out)
{
    if (out.size() != kCalibrationBlobBytes) { return false; }
    for (const auto v : cal.values) { if (v < -32768 || v > 32767) { return false; } }
    if (cal.values[3] < 0 || cal.values[10] < 0) { return false; }
    wire::Writer w(out.data(), out.size());
    w.appendU8(1); w.appendU8(cal.has_sic ? 1 : 0);
    for (const auto v : cal.values) { w.appendI16Le(static_cast<int16_t>(v)); }
    return w.ok();
}

void calibrationUpdates(const Calibration &cal, core::ActionOut &out)
{
    out.param_count = 22;
    out.params[0] = {kCalibrationValid, cal.valid ? 1U : 0U};
    out.params[1] = {kCalibrationHasSic, cal.has_sic ? 1U : 0U};
    for (size_t i = 0; i < cal.values.size(); ++i) {
        out.params[i+2] = {static_cast<uint16_t>(kCalibrationFirst+i), static_cast<uint32_t>(cal.values[i])};
    }
}

bool bindCalibration(core::InstanceContext &ctx, Calibration &cal)
{
    bool ok = ctx.bindParam(kCalibrationValid, &cal.valid) && ctx.bindParam(kCalibrationHasSic, &cal.has_sic);
    for (size_t i = 0; i < cal.values.size(); ++i) {
        ok = ctx.bindParam(static_cast<uint16_t>(kCalibrationFirst+i), &cal.values[i]) && ok;
    }
    return ok;
}

bool State::bind(core::InstanceContext &ctx)
{
    bool ok = bindCalibration(ctx, staged) && ctx.bindParam(6, &startup_mode);
    uint16_t id = kSignalFirst;
    ok = ctx.bindSignal(id++, &sample.yaw) && ok;
    ok = ctx.bindSignal(id++, &sample.roll) && ok;
    ok = ctx.bindSignal(id++, &sample.pitch) && ok;
    for (const auto *group : {&sample.quaternion}) {
        for (const auto &v : *group) { ok = ctx.bindSignal(id++, &v) && ok; }
    }
    for (const auto *group : {&sample.linear_accel, &sample.gravity, &sample.accel, &sample.gyro, &sample.mag}) {
        for (const auto &v : *group) { ok = ctx.bindSignal(id++, &v) && ok; }
    }
    ok = ctx.bindSignal(id++, &sample.temperature) && ok;
    for (const auto &v : sample.calibration) { ok = ctx.bindSignal(id++, &v) && ok; }
    ok = ctx.bindSignal(id++, &valid) && ok;
    ok = ctx.bindSignal(id++, &age_us) && ok;
    ok = ctx.bindSignal(id++, &count) && ok;
    ok = ctx.bindSignal(id++, &available) && ok;
    return ctx.bindSignal(id, &actual_mode) && ok;
}

void State::age(int64_t now_us)
{
    age_us = last_sample_us < 0 ? UINT32_MAX : static_cast<uint32_t>(std::min<int64_t>(UINT32_MAX, std::max<int64_t>(0, now_us-last_sample_us)));
}

void State::publish(const Sample &value, int64_t now_us)
{
    sample = value; valid = actual_mode != 0 && actual_mode <= 12;
    available = modeAvailability(actual_mode); ++count; last_sample_us = now_us; age_us = 0;
}

uint16_t modeAvailability(uint8_t mode)
{
    // Datasheet table 3-3: enabled sensors/fusion outputs per OPR_MODE.
    constexpr uint16_t accel = 16, gyro = 32, mag = 64, fusion = 15, status = 256;
    constexpr uint16_t sensors[]{0,accel,mag,gyro,accel|mag,accel|gyro,gyro|mag,
        accel|gyro|mag,accel|gyro,accel|mag,accel|mag,accel|gyro|mag,accel|gyro|mag};
    if (mode == 0 || mode > 12) { return 0; }
    // TEMP_SOURCE is explicitly accelerometer. Do not promise fresh
    // temperature when this mode suspends the accelerometer.
    return status | sensors[mode] | (mode >= 8 ? fusion : 0) | ((sensors[mode] & accel) ? 128 : 0);
}
} // namespace mod_imu_bno055
