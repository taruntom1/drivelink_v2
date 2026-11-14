#pragma once

#include "core/instance.hpp"
#include <memory>

namespace mod_motor {
inline constexpr uint32_t kActuatorInterface = 0x4D4F5401;
inline constexpr uint32_t kFeedbackInterface = 0x4D4F5402;

struct Measurement { float position = 0, velocity = 0; int32_t count = 0; };
struct PlantObservation { double position = 0; uint32_t reset_generation = 0; };

// Loop-owned port. Shared lifetime lets a controller release its claim even
// when the core destroys layout records in provider-before-consumer order.
// Allocate/copy shared ownership at startup only; tick accesses plain fields.
struct ControlPort { bool claimed = false; float duty = 0; };
class Actuator : public core::Instance {
public:
    void *queryInterface(uint32_t id) override { return id == kActuatorInterface ? this : nullptr; }
    std::shared_ptr<ControlPort> claim() {
        if (m_port->claimed) { return {}; }
        m_port->claimed = true; m_port->duty = 0; return m_port;
    }
    virtual bool plantObservation(PlantObservation &) const { return false; }
protected:
    std::shared_ptr<ControlPort> m_port = std::make_shared<ControlPort>();
};
class Feedback : public core::Instance {
public:
    void *queryInterface(uint32_t id) override { return id == kFeedbackInterface ? this : nullptr; }
    const Measurement &measurement() const { return m_measurement; }
protected:
    Measurement m_measurement;
};
} // namespace mod_motor
