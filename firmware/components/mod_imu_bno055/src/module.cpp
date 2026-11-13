#include "mod_imu_bno055/modules.hpp"
#include "mod_imu_bno055/driver.hpp"
#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/type_imu_bno055.hpp"

namespace mod_imu_bno055 {
namespace {
namespace defs = drivelink::protocol::defs;
namespace d = defs::imu_bno055;
namespace codes = defs::diag::codes::imu_bno055;
constexpr uint32_t kPins = 1, kBus = 2, kInit = 3, kRestore = 4;

class Imu final : public core::Instance {
public:
    bool start(core::InstanceContext &ctx) override {
        m_instance = ctx.instanceId();
        if (!m_state.bind(ctx) || !ctx.bindParam(d::kSdaPin, &m_config.sda) ||
            !ctx.bindParam(d::kSclPin, &m_config.scl) || !ctx.bindParam(d::kPort, &m_config.port) ||
            !ctx.bindParam(d::kAddress, &m_config.address) || !ctx.bindParam(d::kBusHz, &m_config.hz) ||
            !ctx.bindParam(d::kTimeoutMs, &m_config.timeout_ms)) { return false; }
        if (m_config.sda == 255 || m_config.scl == 255) { ctx.fail(kPins, -1); return false; }
        core_os::Err err = 0;
        m_bus = openBus(m_config, err);
        if (!m_bus) { ctx.fail(kBus, err ? err : -1); return false; }
        m_driver = std::make_unique<Driver>(*m_bus);
        err = m_driver->init();
        if (err) { ctx.fail(kInit, err); return false; }
        if (m_state.staged.valid && (err = m_driver->applyCalibration(m_state.staged))) {
            ctx.fail(kRestore, err); return false;
        }
        err = m_driver->setMode(m_state.startup_mode);
        if (err) { ctx.fail(kInit, err); return false; }
        m_state.actual_mode = m_driver->mode();
        return true;
    }
    void tick(const core::TickContext &) override {
        const auto before = core_os::nowUs();
        m_state.age(before);
        if (m_driver->mode() == 0) { m_state.valid = false; m_state.available = 0; return; }
        Sample sample;
        const auto err = m_driver->readSample(sample);
        m_state.actual_mode = m_driver->mode();
        m_state.available = modeAvailability(m_state.actual_mode);
        if (err) {
            m_state.valid = false; m_state.age(core_os::nowUs());
            core_os::raiseDiag(codes::kReadFailed, m_instance);
        } else {
            m_state.publish(sample, before + (core_os::nowUs()-before)/2);
            core_os::clearDiag(codes::kReadFailed, m_instance);
        }
    }
    core::Status runAction(uint16_t id, const core::ActionArgs &in, core::ActionOut &out) override {
        Calibration cal;
        core_os::Err err = 0;
        if (id == kCalibrationWrite) {
            if (!decodeCalibration(in.blob, cal)) { return badCalibration(); }
            calibrationUpdates(cal, out);
            return {};
        }
        if (id == kCalibrationRead) {
            err = m_driver->readCalibration(cal);
            if (!err) {
                if (!encodeCalibration(cal, {out.blob.data(), kCalibrationBlobBytes})) { return badCalibration(); }
                out.blob_size = kCalibrationBlobBytes; calibrationUpdates(cal, out);
            }
        } else if (id == kCalibrationApply) {
            if (!m_state.staged.valid) { return badCalibration(); }
            err = m_driver->applyCalibration(m_state.staged);
        } else if (id == kSetMode) {
            if (in.values[0] > 12) { return badCalibration(); }
            err = m_driver->setMode(static_cast<uint8_t>(in.values[0]));
        } else { return core::Status::fail(core::Result::InvalidArgument, core::Detail::UnknownRef); }
        m_state.actual_mode = m_driver->mode();
        m_state.available = modeAvailability(m_state.actual_mode);
        m_state.valid = false; m_state.age(core_os::nowUs());
        if (err) {
            core_os::reportDiag(codes::kActionFailed, err, id, m_driver->mode(), m_instance);
            return core::Status::fail(core::Result::Fault, core::Detail::BadValue);
        }
        return {};
    }
private:
    BusConfig m_config;
    State m_state;
    uint8_t m_instance = 255;
    std::unique_ptr<Bus> m_bus;
    std::unique_ptr<Driver> m_driver;
};
core::Instance *make(const core::InstanceConfig &) { return new Imu; }
}
const core::ModuleType kType{d::kDesc, make};
} // namespace mod_imu_bno055
