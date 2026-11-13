#include "core/instance.hpp"

#include "core/instance_table.hpp"

namespace core {

namespace detail {

size_t InstanceRecord::itemIndex(uint16_t id) const
{
    if (desc == nullptr) {
        return kNoItem;
    }
    for (size_t i = 0; i < desc->items.size(); ++i) {
        if (desc->items[i].id == id) {
            return i;
        }
    }
    return kNoItem;
}

} // namespace detail

bool InstanceContext::bindSourceStorage(uint16_t id, defs::ValueType type, void *value,
                                      uint32_t *age, bool *valid, bool *fresh)
{
    const size_t index = m_record.itemIndex(id);
    const auto *param = index == detail::kNoItem ? nullptr : &m_record.desc->items[index];
    const auto *sample = param ? defs::sourceSignal(*m_record.desc, *param, ".value") : nullptr;
    const auto *age_item = param ? defs::sourceSignal(*m_record.desc, *param, ".age_us") : nullptr;
    if (!param || !defs::hasFlag(*param, defs::ItemFlag::Source) || !sample || !age_item ||
        param->kind != defs::ItemKind::Param || param->type != defs::ValueType::U32 ||
        age_item->type != defs::ValueType::U32) {
        if (m_bind_error == Detail::None) { m_bind_error = Detail::WrongKind; }
        return false;
    }
    if (!value || !age || !valid || !fresh) {
        if (m_bind_error == Detail::None) { m_bind_error = Detail::NullStorage; }
        return false;
    }
    if (!bind(id, defs::ItemKind::Param, defs::ValueType::U32, &m_record.shadow[index], nullptr) ||
        !bind(sample->id, defs::ItemKind::Signal, type, value, nullptr) ||
        !bind(age_item->id, defs::ItemKind::Signal, defs::ValueType::U32, age, nullptr)) { return false; }
    *age = kInvalidAgeUs;
    *valid = false;
    *fresh = false;
    m_record.sources.push_back({index, type, value, age, valid, fresh});
    return true;
}

bool InstanceContext::bind(uint16_t id, defs::ItemKind kind, defs::ValueType type, void *storage, OnChange hook)
{
    Detail error = Detail::None;
    const size_t index = m_record.itemIndex(id);
    if (index == detail::kNoItem) {
        error = Detail::UnknownRef;
    } else {
        const defs::ItemDesc &item = m_record.desc->items[index];
        if (item.kind != kind) {
            error = Detail::WrongKind;
        } else if (item.type != type) {
            error = Detail::WrongType;
        } else if (storage == nullptr) {
            error = Detail::NullStorage;
        } else if (m_record.bindings[index].bound) {
            error = Detail::AlreadyBound;
        }
    }
    if (error != Detail::None) {
        if (m_bind_error == Detail::None) {
            m_bind_error = error;
        }
        return false;
    }

    detail::Binding &b = m_record.bindings[index];
    b.ptr = storage;
    b.hook = hook;
    b.bound = true;
    if (kind == defs::ItemKind::Param) {
        // The storage starts at the boot value, so it matches the shadow.
        storeValue(type, storage, m_record.shadow[index]);
    }
    return true;
}

uint32_t InstanceContext::param(uint16_t id) const
{
    const size_t index = m_record.itemIndex(id);
    if (index == detail::kNoItem || m_record.desc->items[index].kind != defs::ItemKind::Param) {
        return 0U;
    }
    return m_record.shadow[index];
}

Instance *InstanceContext::ref(uint16_t id) const
{
    const size_t index = m_record.itemIndex(id);
    if (m_table == nullptr || index == detail::kNoItem) {
        return nullptr;
    }
    const defs::ItemDesc &item = m_record.desc->items[index];
    if (item.kind != defs::ItemKind::Param || item.ref == nullptr) {
        return nullptr;
    }
    const uint32_t value = m_record.shadow[index];
    if (value == defs::kUnwired || !defs::isWiringRef(value)) {
        return nullptr;
    }
    const detail::InstanceRecord *target =
        m_table->find(defs::wiringRefType(value), defs::wiringRefInstance(value));
    return (target != nullptr) ? target->object.get() : nullptr;
}

void InstanceContext::fail(uint32_t stage, int32_t err)
{
    if (!m_failed) {
        m_failed = true;
        m_fail_stage = stage;
        m_fail_err = err;
    }
}

} // namespace core
