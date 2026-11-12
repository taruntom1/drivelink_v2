#include "core/instance.hpp"

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

void InstanceContext::fail(uint32_t stage, int32_t err)
{
    if (!m_failed) {
        m_failed = true;
        m_fail_stage = stage;
        m_fail_err = err;
    }
}

} // namespace core
