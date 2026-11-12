#pragma once

// Test module types for the core tests, built on the generator's fixture
// types (fixture.full: every kind and value type, both loops; fixture.sensor:
// one signal, sensor loop only). Test-only code: it allocates freely.

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/core.hpp"
#include "core/instance.hpp"
#include "core/module_type.hpp"
#include "core_os/native_diag.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "type_fixture_full.hpp"
#include "type_fixture_sensor.hpp"

namespace testmod {

namespace fx = drivelink::protocol::defs::fixture_full;
namespace fxs = drivelink::protocol::defs::fixture_sensor;
namespace defs = drivelink::protocol::defs;

/// How the next FullModule behaves in start().
enum class StartMode {
    Normal,
    SkipGain,     ///< leave the gain param unbound
    BadBind,      ///< bind gain with the wrong C++ type (and nothing else wrong)
    FailStage,    ///< ctx.fail(7, -3) and return false
    ReturnFalse,  ///< return false without fail()
    CreateNull,   ///< create() returns nullptr
};

enum class FxMode : uint8_t {
    A = 0,
    B = 1,
    C = 2,
};

enum class FxState : uint8_t {
    Idle = 0,
    Run = 1,
};

class FullModule;

/// Shared test state; reset() it at the start of each test.
struct World {
    StartMode mode = StartMode::Normal;
    std::vector<FullModule *> created;
    std::vector<int> tick_log; ///< ids of ticked instances, in order (type << 8 | inst)
    std::mutex log_mutex;      ///< the two loop threads both tick FullModules
};

inline World &world()
{
    static World w;
    return w;
}

inline void reset()
{
    World &w = world();
    w.mode = StartMode::Normal;
    w.created.clear();
    w.tick_log.clear();
    core_os::native::resetRecordedDiag();
}

class FullModule final : public core::Instance {
public:
    explicit FullModule(const core::InstanceConfig &cfg) : m_type(cfg.type_id), m_inst(cfg.instance) {}

    bool start(core::InstanceContext &ctx) override
    {
        boot_actuator = ctx.param(fx::kActuator);
        const StartMode mode = world().mode;
        if (mode == StartMode::FailStage) {
            ctx.fail(7, -3);
            return false;
        }
        if (mode == StartMode::ReturnFalse) {
            return false;
        }
        ctx.bindParam(fx::kActuator, &actuator);
        ctx.bindParam(fx::kMode, &mode_value);
        if (mode == StartMode::BadBind) {
            ctx.bindParam(fx::kGain, &level); // u8 storage for an f32 param
        } else if (mode != StartMode::SkipGain) {
            ctx.bindParam(fx::kGain, &gain, &FullModule::onChange);
        }
        ctx.bindParam(fx::kOffset, &offset);
        ctx.bindParam(fx::kEnabled, &enabled);
        ctx.bindParam(fx::kLevel, &level, &FullModule::onChange);
        ctx.bindParam(fx::kCount, &count);
        ctx.bindParam(fx::kVelKp, &vel_kp);
        ctx.bindSignal(fx::kPosition, &position);
        ctx.bindSignal(fx::kReady, &ready);
        ctx.bindSignal(fx::kState, &state);
        ctx.bindInput(fx::kVelocitySp, &velocity_sp);
        ctx.bindInput(fx::kRaw, &raw);
        return true;
    }

    void tick(const core::TickContext &t) override
    {
        {
            std::lock_guard<std::mutex> lock(world().log_mutex);
            world().tick_log.push_back((m_type << 8) | m_inst);
        }
        ++ticks;
        last_dt = t.dt_s;
        last_elapsed = t.elapsed;
    }

    static void onChange(void *self, uint16_t id)
    {
        auto *m = static_cast<FullModule *>(self);
        m->hook_ids.push_back(id);
        m->hook_saw_gain = m->gain;
        m->hook_saw_level = m->level;
    }

    // Bound storage.
    uint32_t actuator = 0;
    FxMode mode_value = FxMode::A;
    float gain = 0.0f;
    int32_t offset = 0;
    bool enabled = false;
    uint8_t level = 0;
    uint16_t count = 0;
    float vel_kp = 0.0f;
    float position = 0.0f;
    bool ready = false;
    FxState state = FxState::Idle;
    float velocity_sp = 0.0f;
    uint32_t raw = 0;

    // Observations.
    uint32_t boot_actuator = 0xFFFFFFFF;
    int ticks = 0;
    float last_dt = 0.0f;
    uint32_t last_elapsed = 0;
    std::vector<uint16_t> hook_ids;
    float hook_saw_gain = 0.0f;
    uint8_t hook_saw_level = 0;

private:
    int m_type;
    int m_inst;
};

class SensorModule final : public core::Instance {
public:
    bool start(core::InstanceContext &ctx) override { return ctx.bindSignal(fxs::kValue, &value); }
    void tick(const core::TickContext &t) override { value += t.dt_s; }

    float value = 0.0f;
};

inline core::Instance *createFull(const core::InstanceConfig &cfg)
{
    if (world().mode == StartMode::CreateNull) {
        return nullptr;
    }
    auto *m = new FullModule(cfg);
    world().created.push_back(m);
    return m;
}

inline core::Instance *createSensor(const core::InstanceConfig &cfg)
{
    (void)cfg;
    return new SensorModule();
}

inline const core::ModuleType kFullType{fx::kDesc, &createFull};
inline const core::ModuleType kSensorType{fxs::kDesc, &createSensor};
inline const std::array<const core::ModuleType *, 2> kTypes{&kFullType, &kSensorType};

inline core::CoreConfig testConfig(uint16_t max_instances = 16)
{
    core::CoreConfig cfg;
    cfg.max_instances = max_instances;
    return cfg;
}

/// A slot value for an f32.
inline uint32_t f(float v)
{
    return defs::f32Bits(v);
}

inline core::Ref ref(uint8_t type, uint8_t inst, uint16_t id)
{
    return core::Ref{type, inst, id};
}

inline core::SetEntry entry(uint8_t type, uint8_t inst, uint16_t id, uint32_t bits)
{
    return core::SetEntry{core::Ref{type, inst, id}, bits};
}

} // namespace testmod
