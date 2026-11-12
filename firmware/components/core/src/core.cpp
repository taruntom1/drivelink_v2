#include "core/core.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "core_os/diag_sink.hpp"
#include "drivelink/protocol/gen/diag_codes.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/gen/type_system.hpp"

namespace core {

namespace {

namespace sys = drivelink::protocol::defs::system;
namespace diag_codes = drivelink::protocol::defs::diag::codes;

constexpr const char *kControlTaskName = "core_ctrl";
constexpr const char *kSensorTaskName = "core_sens";

/// The core's own storage for the system params. It runs in no loop: the
/// param service applies system params on the config task.
class SystemInstance final : public Instance {
public:
    bool start(InstanceContext &ctx) override
    {
        ctx.bindParam(sys::kControlRateHz, &m_control_rate_hz);
        ctx.bindParam(sys::kSensorRateHz, &m_sensor_rate_hz);
        return true;
    }

    void tick(const TickContext &t) override { (void)t; }

private:
    uint16_t m_control_rate_hz = 0;
    uint16_t m_sensor_rate_hz = 0;
};

uint32_t defaultPeriodUs(uint16_t rate_id)
{
    const defs::ItemDesc *item = defs::findItem(sys::kDesc, rate_id);
    return periodUsFromHz((item != nullptr) ? item->default_bits : 0U);
}

uint32_t instanceArg(uint8_t type_id, uint8_t instance)
{
    return (static_cast<uint32_t>(type_id) << 8U) | instance;
}

} // namespace

Core::Core(const CoreConfig &config)
    : m_config(config),
      m_table(static_cast<size_t>(config.max_instances) + 1U), // + the system instance
      m_control(kControlLoop, "control", defs::LoopClass::Control, config.max_instances,
                defaultPeriodUs(sys::kControlRateHz)),
      m_sensor(kSensorLoop, "sensor", defs::LoopClass::Sensor, config.max_instances,
               defaultPeriodUs(sys::kSensorRateHz)),
      m_params(m_table, {&m_control, &m_sensor}, &Core::applySystemParam, this)
{
    std::unique_ptr<detail::InstanceRecord> record =
        makeRecord(sys::kDesc, drivelink::protocol::defs::kSystemTypeId, 0, kNoLoop, "system");
    record->object = std::make_unique<SystemInstance>();
    m_system_ok = startInstance(*record).ok();
    if (m_system_ok) {
        (void)m_table.add(std::move(record)); // the table has room: it was just made
    }
}

Core::~Core()
{
    stop();
}

Status Core::init(std::span<const ModuleType *const> types)
{
    if (!m_system_ok) {
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    return m_registry.init(types);
}

std::unique_ptr<detail::InstanceRecord> Core::makeRecord(const defs::TypeDesc &desc, uint8_t type_id,
                                                         uint8_t instance, uint8_t loop, const char *label)
{
    auto record = std::make_unique<detail::InstanceRecord>();
    record->desc = &desc;
    record->type_id = type_id;
    record->instance = instance;
    record->loop = loop;
    const size_t label_len = std::min(std::strlen(label), kLabelBytes);
    std::memcpy(record->label, label, label_len);
    record->label[label_len] = '\0';
    const size_t n = desc.items.size();
    record->bindings = std::make_unique<detail::Binding[]>(n);
    record->shadow = std::make_unique<uint32_t[]>(n);
    for (size_t i = 0; i < n; ++i) {
        const defs::ItemDesc &item = desc.items[i];
        // Boot values: the YAML defaults until F3a-2 loads them from NVS.
        record->shadow[i] = (item.kind == defs::ItemKind::Param) ? item.default_bits : 0U;
    }
    return record;
}

Status Core::startInstance(detail::InstanceRecord &record)
{
    InstanceContext ctx(record);
    const bool started = record.object->start(ctx);

    uint32_t stage = 0;
    int32_t err = 0;
    if (ctx.failed()) {
        stage = ctx.failStage();
        err = ctx.failErr();
    } else if (ctx.bindError() != Detail::None) {
        stage = kStartStageBind;
    } else if (!started) {
        stage = kStartStageNoStage;
    } else {
        for (size_t i = 0; i < record.desc->items.size(); ++i) {
            if (record.desc->items[i].kind != defs::ItemKind::Action && !record.bindings[i].bound) {
                stage = kStartStageUnbound;
                break;
            }
        }
    }
    if (stage == 0U && (ctx.failed() || !started)) {
        stage = kStartStageNoStage; // fail() was called with stage 0
    }
    if (stage != 0U) {
        core_os::reportDiag(diag_codes::core::kInstanceStartFailed, err, instanceArg(record.type_id, record.instance),
                            stage);
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    return Status::success();
}

Status Core::createInstance(uint8_t type_id, uint8_t instance, uint8_t loop_id, const char *label)
{
    if (m_started) {
        return Status::fail(Result::Busy, Detail::NotAtBoot);
    }
    const ModuleType *type = m_registry.find(type_id);
    if (type == nullptr) {
        return Status::fail(Result::NotFound, Detail::UnknownType);
    }
    if (loop_id >= kLoopCount || !defs::runsIn(type->desc, loop(loop_id).loopClass())) {
        return Status::fail(Result::InvalidArgument, Detail::LoopMismatch);
    }
    if (m_table.find(type_id, instance) != nullptr) {
        return Status::fail(Result::Exists, Detail::None);
    }
    if (label == nullptr) {
        label = "";
    }
    if (std::strlen(label) > kLabelBytes) {
        return Status::fail(Result::InvalidArgument, Detail::BadLabel);
    }
    if (instanceCount() >= m_config.max_instances) {
        return Status::fail(Result::NoMemory, Detail::Capacity);
    }

    std::unique_ptr<detail::InstanceRecord> record = makeRecord(type->desc, type_id, instance, loop_id, label);
    const InstanceConfig cfg{&type->desc, type_id, instance, loop_id, record->label};
    record->object.reset(type->create(cfg));
    if (!record->object) {
        core_os::reportDiag(diag_codes::core::kInstanceStartFailed, 0, instanceArg(type_id, instance),
                            kStartStageCreate);
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    const Status st = startInstance(*record);
    if (!st.ok()) {
        return st; // the record and the object go away here
    }
    Instance *object = record->object.get();
    if (!m_table.add(std::move(record)) || !loop(loop_id).addInstance(object)) {
        // Not reachable: both are sized for max_instances, checked above.
        return Status::fail(Result::NoMemory, Detail::Capacity);
    }
    return Status::success();
}

Status Core::start()
{
    if (m_running) {
        return Status::success();
    }
    m_start_stage = 0;
    m_start_err = core_os::kOk;

    core_os::Err err = m_control_task.start(kControlTaskName, m_config.control.stack_bytes, m_config.control.priority,
                                            m_config.control.core, m_control.periodUs(), &Loop::taskEntry,
                                            &m_control);
    if (err != core_os::kOk) {
        m_start_stage = kInitStageControlTask;
        m_start_err = err;
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    err = m_sensor_task.start(kSensorTaskName, m_config.sensor.stack_bytes, m_config.sensor.priority,
                              m_config.sensor.core, m_sensor.periodUs(), &Loop::taskEntry, &m_sensor);
    if (err != core_os::kOk) {
        m_control_task.stop();
        m_start_stage = kInitStageSensorTask;
        m_start_err = err;
        return Status::fail(Result::Fault, Detail::StartFailed);
    }
    m_running = true;
    m_started = true;
    return Status::success();
}

void Core::stop()
{
    m_control_task.stop();
    m_sensor_task.stop();
    m_running = false;
}

uint32_t Core::loopStackHighWater(uint8_t id) const
{
    return (id == kSensorLoop) ? m_sensor_task.stackHighWaterMark() : m_control_task.stackHighWaterMark();
}

bool Core::applySystemParam(void *ctx, uint16_t id, uint32_t bits)
{
    auto *self = static_cast<Core *>(ctx);
    uint8_t loop_id = 0;
    if (id == sys::kControlRateHz) {
        loop_id = kControlLoop;
    } else if (id == sys::kSensorRateHz) {
        loop_id = kSensorLoop;
    } else {
        return true; // nothing to do beyond storing it
    }
    const uint32_t period_us = periodUsFromHz(bits);
    const core_os::Err err = self->task(loop_id).setPeriod(period_us);
    if (err != core_os::kOk) {
        core_os::reportDiag(diag_codes::core::kTimerRestartFailed, err, loop_id, period_us);
        return false;
    }
    self->loop(loop_id).setPeriodUs(period_us);
    return true;
}

} // namespace core
