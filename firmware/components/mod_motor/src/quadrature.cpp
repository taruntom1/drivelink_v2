#include "mod_motor/modules.hpp"
#include "mod_motor/interfaces.hpp"
#include "mod_motor/hardware.hpp"
#include "mod_motor/velocity_estimator.hpp"
#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/type_feedback_quadrature.hpp"
#include <limits>

namespace mod_motor {
namespace {
namespace defs = drivelink::protocol::defs;
namespace d = defs::feedback_quadrature;
namespace codes = defs::diag::codes;
// feedback_quadrature.InitFailed arg0 stages (documented in diag_codes.yaml).
constexpr uint32_t kStartStagePins = 1;
constexpr uint32_t kStartStageDriver = 2;

/// feedback.quadrature: a PCNT quadrature encoder. The module never resets
/// the count; arm transitions only re-prime the velocity estimator.
class Quadrature final : public Feedback {
public:
    bool start(core::InstanceContext &ctx) override {
        m_instance = ctx.instanceId();
        const bool bound = ctx.bindParam(d::kPinA, &m_pin_a) && ctx.bindParam(d::kPinB, &m_pin_b) &&
            ctx.bindParam(d::kPullup, &m_pullup) && ctx.bindParam(d::kCountsPerRev, &m_cpr) &&
            ctx.bindParam(d::kGearRatio, &m_ratio) && ctx.bindParam(d::kVelocityCutoff, &m_cutoff) &&
            ctx.bindParam(d::kInvert, &m_invert, &invertChanged) &&
            ctx.bindSignal(d::kCount, &m_measurement.count) &&
            ctx.bindSignal(d::kVelocity, &m_measurement.velocity) &&
            ctx.bindSignal(d::kPosition, &m_measurement.position);
        if (!bound) { return false; }
        if (m_pin_a == defs::kNoGpio || m_pin_b == defs::kNoGpio) {
            ctx.fail(kStartStagePins, -1);
            return false;
        }
        core_os::Err err = core_os::kOk;
        m_counter = hw::openQuadrature(m_pin_a, m_pin_b, m_pullup, err);
        if (!m_counter) {
            ctx.fail(kStartStageDriver, err != core_os::kOk ? err : -1);
            return false;
        }
        return true;
    }

    void tick(const core::TickContext &t) override {
        int32_t raw = 0;
        if (m_counter->read(raw) != core_os::kOk) {
            if (!m_fault) {
                m_fault = true;
                core_os::raiseDiag(codes::feedback_quadrature::kReadFailed, m_instance);
            }
            // The count keeps its last value. A non-finite measurement makes
            // the controller output zero duty and reset on this tick.
            m_measurement.position = m_measurement.velocity = std::numeric_limits<float>::quiet_NaN();
            m_estimator.reset();
            return;
        }
        if (m_fault) {
            m_fault = false;
            core_os::clearDiag(codes::feedback_quadrature::kReadFailed, m_instance);
        }
        // Negate in unsigned arithmetic, so INT32_MIN stays defined.
        const int32_t count = m_invert ? static_cast<int32_t>(0U - static_cast<uint32_t>(raw)) : raw;
        m_measurement.count = count;
        m_measurement.position = countsToRadians(count, m_cpr, m_ratio);
        m_measurement.velocity = m_estimator.update(count, t.dt_s, m_cpr, m_ratio, m_cutoff);
    }

    void onArmTransition(bool) override { m_estimator.reset(); }

    core::Status runAction(uint16_t, const core::ActionArgs &, core::ActionOut &) override {
        return core::Status::fail(core::Result::InvalidArgument, core::Detail::UnknownRef);
    }

private:
    /// The count changes sign: re-prime the estimator so no velocity step appears.
    static void invertChanged(void *self, uint16_t) { static_cast<Quadrature *>(self)->m_estimator.reset(); }

    std::unique_ptr<hw::QuadratureCounter> m_counter;  // set in start(), immutable after
    VelocityEstimator m_estimator;
    uint8_t m_instance = 255;
    uint8_t m_pin_a = defs::kNoGpio, m_pin_b = defs::kNoGpio;
    bool m_pullup = true, m_invert = false, m_fault = false;
    float m_cpr = 1440, m_ratio = 1, m_cutoff = 30;
};

core::Instance *make(const core::InstanceConfig &) { return new Quadrature; }
} // namespace

const core::ModuleType kQuadratureType{d::kDesc, make};
} // namespace mod_motor
