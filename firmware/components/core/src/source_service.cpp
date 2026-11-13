#include "core/source_service.hpp"
#include "drivelink/protocol/gen/registry.hpp"

#include <algorithm>

namespace core {

void detail::SourceBuffer::publish(int64_t timestamp_us)
{
    m_timestamps[m_back] = timestamp_us;
    m_back = m_middle.exchange(static_cast<uint8_t>(m_back | kDirty), std::memory_order_acq_rel) & ~kDirty;
}

bool detail::SourceBuffer::take()
{
    if (!(m_middle.load(std::memory_order_acquire) & kDirty)) { return false; }
    m_front = m_middle.exchange(m_front, std::memory_order_acq_rel) & ~kDirty;
    m_valid = true;
    return true;
}

namespace {
using Sample = detail::InstanceRecord::SourceBinding;
void update(const Sample &sample, uint32_t bits, uint32_t age, bool valid, bool fresh)
{
    storeValue(sample.type, sample.value, bits);
    *sample.age = age;
    *sample.valid = valid;
    *sample.fresh = fresh;
}
uint8_t ownerLoop(const detail::InstanceRecord &record)
{
    return record.type_id == defs::kLoopTypeId ? record.instance : record.loop;
}
} // namespace

struct SourceService::Impl {
    struct Sink { Sample sample; size_t index; };
    struct Link {
        uint8_t producer, consumer;
        std::vector<BoundSignal> signals;
        std::vector<Sink> sinks;
        std::unique_ptr<detail::SourceBuffer> buffer;
    };
    struct Local { Sample sample; BoundSignal signal; };
    struct Consumer { const Instance *instance; std::vector<Local> bindings; };
    std::vector<Link> links;
    std::vector<Consumer> consumers;
};

SourceService::SourceService() : m_impl(std::make_unique<Impl>()) {}
SourceService::~SourceService() = default;

void SourceService::build(const detail::InstanceTable &table)
{
    auto &p = *m_impl;
    p.links.clear(); p.consumers.clear();
    for (size_t i = 0; i < table.size(); ++i) {
        const auto &record = table.at(i);
        if (record.state != InstanceState::Running) { continue; }
        for (const auto &sample : record.sources) {
            const auto ref = record.shadow[sample.param_index];
            const auto *target = ref ? table.find(defs::sourceRefType(ref), defs::sourceRefInstance(ref)) : nullptr;
            if (!target || target->state != InstanceState::Running) { continue; }
            const auto k = target->itemIndex(defs::sourceRefId(ref));
            if (k == detail::kNoItem || !target->bindings[k].bound) { continue; }
            const BoundSignal signal{{target->type_id, target->instance, defs::sourceRefId(ref)},
                                      &target->desc->items[k], target->bindings[k].ptr};
            const auto producer = ownerLoop(*target);
            if (producer == record.loop) {
                auto index = consumerIndex(record.object.get());
                if (index == kNoConsumer) {
                    index = p.consumers.size();
                    p.consumers.push_back({record.object.get(), {}});
                }
                p.consumers[index].bindings.push_back({sample, signal});
                continue;
            }
            auto link = std::find_if(p.links.begin(), p.links.end(), [&](const auto &entry) {
                return entry.producer == producer && entry.consumer == record.loop;
            });
            if (link == p.links.end()) {
                p.links.push_back({producer, record.loop, {}, {}, nullptr});
                link = p.links.end() - 1;
            }
            size_t index = 0;
            while (index < link->signals.size() &&
                   (link->signals[index].ref.type != signal.ref.type ||
                    link->signals[index].ref.instance != signal.ref.instance ||
                    link->signals[index].ref.id != signal.ref.id)) { ++index; }
            if (index == link->signals.size()) { link->signals.push_back(signal); }
            link->sinks.push_back({sample, index});
        }
    }
    for (auto &link : p.links) { link.buffer = std::make_unique<detail::SourceBuffer>(link.signals.size()); }
}

void SourceService::beginTick(uint8_t loop, int64_t now_us)
{
    for (auto &link : m_impl->links) {
        if (link.consumer != loop) { continue; }
        auto &buffer = *link.buffer;
        const bool fresh = buffer.take();
        const auto timestamp = buffer.timestampUs();
        for (const auto &sink : link.sinks) {
            update(sink.sample, buffer.values()[sink.index],
                   buffer.valid() ? sampleAgeUs(now_us, timestamp) : kInvalidAgeUs,
                   buffer.valid(), fresh);
        }
    }
}

size_t SourceService::consumerIndex(const Instance *instance) const
{
    for (size_t i = 0; i < m_impl->consumers.size(); ++i) {
        if (m_impl->consumers[i].instance == instance) { return i; }
    }
    return kNoConsumer;
}

void SourceService::beforeInstance(size_t consumer_index)
{
    if (consumer_index == kNoConsumer) { return; }
    for (const auto &local : m_impl->consumers[consumer_index].bindings) {
        update(local.sample, loadValue(local.signal.item->type, local.signal.storage), 0, true, true);
    }
}

void SourceService::endTick(uint8_t loop, int64_t timestamp_us)
{
    for (auto &link : m_impl->links) {
        if (link.producer != loop) { continue; }
        latchSignals(link.signals, link.buffer->backValues());
        link.buffer->publish(timestamp_us);
    }
}

} // namespace core
