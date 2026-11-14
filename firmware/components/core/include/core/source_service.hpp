#pragma once

#include <atomic>
#include "core_os/atomics.hpp"
#include <memory>
#include <span>
#include <vector>
#include "core/group_service.hpp"
#include "core/instance_table.hpp"

namespace core {

namespace detail {

static_assert(core_os::lockFreeAtomic<uint8_t>()); // see core_os/atomics.hpp

/// Single producer/single consumer triple buffer. Only the middle index is
/// shared. Acq_rel exchanges transfer exclusive ownership of complete arrays.
class SourceBuffer {
public:
    explicit SourceBuffer(size_t count) : m_values(3 * count), m_count(count) {}
    std::span<uint32_t> backValues() { return {m_values.data() + m_back * m_count, m_count}; }
    void publish(int64_t timestamp_us);
    bool take(); // consumer: true iff a new complete snapshot was acquired
    std::span<const uint32_t> values() const { return {m_values.data() + m_front * m_count, m_count}; }
    int64_t timestampUs() const { return m_timestamps[m_front]; }
    bool valid() const { return m_valid; } // consumer-owned
private:
    std::vector<uint32_t> m_values;
    size_t m_count;
    int64_t m_timestamps[3]{};
    static constexpr uint8_t kDirty = 0x80;
    std::atomic<uint8_t> m_middle{1};
    uint8_t m_back = 2; // producer-owned
    uint8_t m_front = 0; // consumer-owned
    bool m_valid = false;
};

} // namespace detail

/// Built once after module binding and before any loop is enabled. Only loop
/// owners touch signal/sample storage; cross-loop traffic goes through buffers.
class SourceService {
public:
    SourceService();
    ~SourceService();
    void build(const detail::InstanceTable &table);
    static constexpr size_t kNoConsumer = static_cast<size_t>(-1);
    size_t consumerIndex(const Instance *instance) const; // boot only
    void beginTick(uint8_t loop, int64_t now_us);
    void beforeInstance(size_t consumer_index);
    void endTick(uint8_t loop, int64_t timestamp_us);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace core
