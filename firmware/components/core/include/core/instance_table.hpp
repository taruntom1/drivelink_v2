#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/instance.hpp"

namespace core::detail {

/// Every instance record, the system's included (first), then one per
/// configured layout entry in layout order. Filled at boot only; after that
/// the set of records is fixed, so lookups need no lock.
class InstanceTable {
public:
    /// Reserve room for `capacity` records (allocation at setup).
    explicit InstanceTable(size_t capacity) : m_capacity(capacity) { m_records.reserve(capacity); }

    /// Take a record. Boot only. @return false when full.
    bool add(std::unique_ptr<InstanceRecord> record)
    {
        if (m_records.size() >= m_capacity) {
            return false;
        }
        m_records.push_back(std::move(record));
        return true;
    }

    /// The record of (type, instance), or nullptr.
    InstanceRecord *find(uint8_t type_id, uint8_t instance) const
    {
        for (const auto &r : m_records) {
            if (r->type_id == type_id && r->instance == instance) {
                return r.get();
            }
        }
        return nullptr;
    }

    size_t size() const { return m_records.size(); }
    InstanceRecord &at(size_t i) const { return *m_records[i]; }
    size_t capacity() const { return m_capacity; }

    /// Destroy every instance object (records stay). The loops must be
    /// stopped.
    void destroyObjects()
    {
        for (auto &r : m_records) {
            r->object.reset();
        }
    }

private:
    size_t m_capacity;
    std::vector<std::unique_ptr<InstanceRecord>> m_records;
};

} // namespace core::detail
