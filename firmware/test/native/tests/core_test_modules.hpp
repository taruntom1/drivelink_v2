#pragma once

// Test module types for the core tests, built on the generator's fixture
// types:
//   - fixture.full: every kind and value type, both loops
//   - fixture.sensor: one signal, sensor loop only
//   - actuator.fixture, feedback.fixture: wiring targets with one pin each
//   - fixture.wired: refs to a feedback (ticks before), an actuator (ticks
//     after) and two `fixture.*` peers, and two pins
// plus the layout helpers: tests write a layout straight to a native KvStore
// and boot a Core on it (a reboot is a new Core on the same store).
// Test-only code: it allocates freely.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/core.hpp"
#include "core/instance.hpp"
#include "core/module_type.hpp"
#include "core/persistence.hpp"
#include "core_os/kv_store.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/native_kv.hpp"
#include "doctest.h"
#include "drivelink/protocol/gen/registry.hpp"
#include "type_actuator_fixture.hpp"
#include "type_feedback_fixture.hpp"
#include "type_fixture_full.hpp"
#include "type_fixture_sensor.hpp"
#include "type_fixture_wired.hpp"

namespace testmod {

namespace fx = drivelink::protocol::defs::fixture_full;
namespace fxs = drivelink::protocol::defs::fixture_sensor;
namespace act = drivelink::protocol::defs::actuator_fixture;
namespace fb = drivelink::protocol::defs::feedback_fixture;
namespace wd = drivelink::protocol::defs::fixture_wired;
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

/// (type << 8 | inst), the tick log's id of an instance.
inline int key(uint8_t type, uint8_t inst)
{
    return (type << 8) | inst;
}

/// Shared test state; reset() it at the start of each test.
struct World {
    StartMode mode = StartMode::Normal;
    std::vector<FullModule *> created;
    std::map<int, core::Instance *> objects; ///< every created module, by key()
    std::vector<int> fail_start;             ///< keys whose start() fails (wiring types)
    std::vector<int> tick_log;               ///< ids of ticked instances, in order (key())
    std::vector<int> create_log;             ///< ids of created instances, in order
    std::mutex log_mutex;                    ///< the two loop threads both tick
};

inline World &world()
{
    static World w;
    return w;
}

inline void logTick(int id)
{
    std::lock_guard<std::mutex> lock(world().log_mutex);
    world().tick_log.push_back(id);
}

inline bool failsStart(int id)
{
    for (const int k : world().fail_start) {
        if (k == id) {
            return true;
        }
    }
    return false;
}

inline void reset()
{
    World &w = world();
    w.mode = StartMode::Normal;
    w.created.clear();
    w.objects.clear();
    w.fail_start.clear();
    w.tick_log.clear();
    w.create_log.clear();
    core_os::native::resetRecordedDiag();
    core_os::native::resetBootInfo();
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
        logTick((m_type << 8) | m_inst);
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

/// The wiring types' modules share this: a start that can be told to fail,
/// and a tick that logs.
class WiringModule : public core::Instance {
public:
    explicit WiringModule(const core::InstanceConfig &cfg) : m_id(key(cfg.type_id, cfg.instance)) {}

    void tick(const core::TickContext &t) override
    {
        (void)t;
        logTick(m_id);
    }

protected:
    bool shouldFail(core::InstanceContext &ctx) const
    {
        if (failsStart(m_id)) {
            ctx.fail(1, -1);
            return true;
        }
        return false;
    }

    int m_id;
};

class ActuatorModule final : public WiringModule {
public:
    using WiringModule::WiringModule;
    bool start(core::InstanceContext &ctx) override
    {
        if (shouldFail(ctx)) {
            return false;
        }
        ctx.bindParam(act::kPin, &pin);
        ctx.bindParam(act::kDutyLimit, &duty_limit);
        ctx.bindSignal(act::kDuty, &duty);
        return true;
    }
    uint8_t pin = 0;
    float duty_limit = 0.0f;
    float duty = 0.0f;
};

class FeedbackModule final : public WiringModule {
public:
    using WiringModule::WiringModule;
    bool start(core::InstanceContext &ctx) override
    {
        if (shouldFail(ctx)) {
            return false;
        }
        ctx.bindParam(fb::kPin, &pin);
        ctx.bindSignal(fb::kPosition, &position);
        return true;
    }
    uint8_t pin = 0;
    float position = 0.0f;
};

class WiredModule final : public WiringModule {
public:
    using WiringModule::WiringModule;
    bool start(core::InstanceContext &ctx) override
    {
        if (shouldFail(ctx)) {
            return false;
        }
        ctx.bindParam(wd::kFeedback, &feedback);
        ctx.bindParam(wd::kActuator, &actuator);
        ctx.bindParam(wd::kPeer, &peer);
        ctx.bindParam(wd::kFollower, &follower);
        ctx.bindParam(wd::kPinA, &pin_a);
        ctx.bindParam(wd::kPinB, &pin_b);
        ctx.bindParam(wd::kGain, &gain);
        ctx.bindSignal(wd::kOutput, &output);
        feedback_obj = ctx.ref(wd::kFeedback);
        actuator_obj = ctx.ref(wd::kActuator);
        peer_obj = ctx.ref(wd::kPeer);
        not_a_ref = ctx.ref(wd::kGain);
        return true;
    }
    uint32_t feedback = 0;
    uint32_t actuator = 0;
    uint32_t peer = 0;
    uint32_t follower = 0;
    uint8_t pin_a = 0;
    uint8_t pin_b = 0;
    float gain = 0.0f;
    float output = 0.0f;
    core::Instance *feedback_obj = nullptr;
    core::Instance *actuator_obj = nullptr;
    core::Instance *peer_obj = nullptr;
    core::Instance *not_a_ref = reinterpret_cast<core::Instance *>(1);
};

template <typename T> core::Instance *remember(const core::InstanceConfig &cfg, T *m)
{
    world().objects[key(cfg.type_id, cfg.instance)] = m;
    world().create_log.push_back(key(cfg.type_id, cfg.instance));
    return m;
}

inline core::Instance *createFull(const core::InstanceConfig &cfg)
{
    if (world().mode == StartMode::CreateNull) {
        return nullptr;
    }
    auto *m = new FullModule(cfg);
    world().created.push_back(m);
    return remember(cfg, m);
}

inline core::Instance *createSensor(const core::InstanceConfig &cfg)
{
    return remember(cfg, new SensorModule());
}

inline core::Instance *createActuator(const core::InstanceConfig &cfg)
{
    return remember(cfg, new ActuatorModule(cfg));
}

inline core::Instance *createFeedback(const core::InstanceConfig &cfg)
{
    return remember(cfg, new FeedbackModule(cfg));
}

inline core::Instance *createWired(const core::InstanceConfig &cfg)
{
    return remember(cfg, new WiredModule(cfg));
}

inline const core::ModuleType kFullType{fx::kDesc, &createFull};
inline const core::ModuleType kSensorType{fxs::kDesc, &createSensor};
inline const core::ModuleType kActuatorType{act::kDesc, &createActuator};
inline const core::ModuleType kFeedbackType{fb::kDesc, &createFeedback};
inline const core::ModuleType kWiredType{wd::kDesc, &createWired};
inline const std::array<const core::ModuleType *, 5> kTypes{&kFullType, &kSensorType, &kActuatorType,
                                                            &kFeedbackType, &kWiredType};

/// The tests' pin policy: GPIO 0..39 except 13.
inline constexpr uint64_t kTestGpioMask = core::gpio_policy::gpioRange(0, 39) & ~core::gpio_policy::gpioBit(13);

inline core::CoreConfig testConfig(uint16_t max_instances = 16)
{
    core::CoreConfig cfg;
    cfg.max_instances = max_instances;
    cfg.gpio_allowed_mask = kTestGpioMask;
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

/// A wiring value.
inline uint32_t wire(uint8_t type, uint8_t inst)
{
    return defs::wiringRef(type, inst);
}

// -- layouts ---------------------------------------------------------------------

/// One layout entry for `writeLayout`, with its stored params.
struct Inst {
    uint8_t type;
    uint8_t inst;
    uint8_t loop;
    std::string label;
    std::vector<core::ParamPair> values = {};
};

/// Write a layout straight to the store, as generation `gen`, through the
/// core's own SAVE path (`persist::saveConfig`).
inline void writeLayout(core_os::KvStore &store, const std::vector<Inst> &insts, uint32_t gen = 1,
                        const std::vector<core::ParamPair> &system = {})
{
    std::vector<core::persist::StoredInstance> items;
    for (const Inst &i : insts) {
        core::persist::StoredInstance s{};
        s.type_id = i.type;
        s.instance = i.inst;
        s.loop = i.loop;
        std::memcpy(s.label, i.label.data(), std::min(i.label.size(), core::kLabelBytes));
        s.pairs = i.values;
        items.push_back(std::move(s));
    }
    REQUIRE(core::persist::saveConfig(store, gen, items, system).ok());
}

/// init() with every test type, then boot() on `store`.
inline core::Status bootCore(core::Core &c, core_os::KvStore &store)
{
    REQUIRE(c.init(kTypes).ok());
    return c.boot(store);
}

/// Write `insts` as the layout and boot `c` on it.
inline void bootWith(core::Core &c, core_os::KvStore &store, const std::vector<Inst> &insts)
{
    writeLayout(store, insts);
    REQUIRE(bootCore(c, store).ok());
}

/// The state of (type, inst), which must have a record.
inline core::InstanceState stateOf(const core::Core &c, uint8_t type, uint8_t inst)
{
    const core::detail::InstanceRecord *r = c.findRecord(type, inst);
    REQUIRE(r != nullptr);
    return r->state;
}

/// One param from the shadow.
inline uint32_t getParam(core::Core &c, uint8_t type, uint8_t inst, uint16_t id)
{
    const core::Ref r = ref(type, inst, id);
    uint32_t v = 0xA5A5A5A5;
    REQUIRE(c.params().get({&r, 1}, {&v, 1}).ok());
    return v;
}

} // namespace testmod
