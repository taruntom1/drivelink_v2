// The pending layout (core/pending_layout.hpp). Config task side; configure
// operations, so it allocates freely.

#include "core/pending_layout.hpp"

#include <cstring>
#include <utility>

#include "drivelink/protocol/gen/registry.hpp"

namespace core {

namespace {

constexpr uint8_t kMaxInstanceId = 0xFE; // 0xFF stays free (an "unparsed" marker on the wire)

uint16_t key(uint8_t type_id, uint8_t instance)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(type_id) << 8U) | instance);
}

size_t itemIndex(const defs::TypeDesc &desc, uint16_t id)
{
    for (size_t i = 0; i < desc.items.size(); ++i) {
        if (desc.items[i].id == id) {
            return i;
        }
    }
    return detail::kNoItem;
}

/// True if `label` fits (at most kLabelBytes characters). Emptiness and
/// uniqueness are validation's.
bool labelFits(const char *label)
{
    if (label == nullptr) {
        return false;
    }
    size_t n = 0;
    while (label[n] != '\0') {
        if (++n > kLabelBytes) {
            return false;
        }
    }
    return true;
}

void copyLabel(char (&dst)[kLabelBytes + 1], const char *src)
{
    size_t n = 0;
    while (n < kLabelBytes && src[n] != '\0') {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = '\0';
}

std::vector<LayoutEntry> viewOf(const std::vector<PendingEntry> &entries)
{
    std::vector<LayoutEntry> view;
    view.reserve(entries.size());
    for (const PendingEntry &e : entries) {
        view.push_back(LayoutEntry{e.desc, e.type_id, e.instance, e.loop, e.label, e.values.data()});
    }
    return view;
}

} // namespace

PendingLayout::PendingLayout(const Registry &registry, detail::InstanceTable &table, ValidationConfig config)
    : m_registry(registry), m_table(table), m_config(config)
{
}

bool PendingLayout::ensureCopy()
{
    if (m_active) {
        return false;
    }
    m_entries.clear();
    m_entries.reserve(m_table.size());
    for (size_t i = 0; i < m_table.size(); ++i) {
        detail::InstanceRecord &r = m_table.at(i);
        if (r.type_id == drivelink::protocol::defs::kSystemTypeId) {
            continue;
        }
        PendingEntry e{};
        e.desc = r.desc;
        e.type_id = r.type_id;
        e.instance = r.instance;
        e.loop = r.loop;
        copyLabel(e.label, r.label);
        e.origin = &r;
        e.values.assign(r.shadow.get(), r.shadow.get() + r.desc->items.size());
        m_entries.push_back(std::move(e));
    }
    return true;
}

PendingEntry *PendingLayout::find(uint8_t type_id, uint8_t instance)
{
    for (PendingEntry &e : m_entries) {
        if (e.type_id == type_id && e.instance == instance) {
            return &e;
        }
    }
    return nullptr;
}

const PendingEntry *PendingLayout::findAdded(uint8_t type_id, uint8_t instance) const
{
    for (const PendingEntry &e : m_entries) {
        if (e.type_id == type_id && e.instance == instance) {
            return (e.origin == nullptr) ? &e : nullptr;
        }
    }
    return nullptr;
}

std::vector<LayoutEntry> PendingLayout::view() const
{
    return viewOf(m_entries);
}

Status PendingLayout::check(const std::vector<PendingEntry> &before, std::span<const uint16_t> touched) const
{
    const std::vector<LayoutEntry> after_view = view();
    std::vector<Status> after(after_view.size());
    if (validateLayout(after_view, m_config, after) == 0U) {
        return Status::success();
    }
    const std::vector<LayoutEntry> before_view = viewOf(before);
    std::vector<Status> was(before_view.size());
    (void)validateLayout(before_view, m_config, was);

    for (size_t i = 0; i < after.size(); ++i) {
        if (after[i].ok()) {
            continue;
        }
        const LayoutEntry &e = after_view[i];
        bool is_touched = false;
        for (const uint16_t k : touched) {
            is_touched = is_touched || (k == key(e.type_id, e.instance));
        }
        const size_t j = findEntry(before_view, e.type_id, e.instance);
        const bool passed_before = (j != kNoEntry) && was[j].ok();
        if (is_touched || passed_before) {
            Status st = after[i];
            st.index = key(e.type_id, e.instance); // the caller turns it into its own index
            return st;
        }
    }
    return Status::success();
}

template <typename Op> Status PendingLayout::run(Op op)
{
    const bool created = ensureCopy();
    std::vector<PendingEntry> before = m_entries;
    std::vector<uint16_t> touched;
    Status st = op(touched);
    if (st.ok()) {
        st = check(before, touched);
    }
    if (!st.ok()) {
        m_entries = std::move(before);
        if (created) {
            m_entries.clear(); // still inactive
        }
        return st;
    }
    m_active = true;
    return st;
}

Status PendingLayout::addInstance(uint8_t type_id, uint8_t loop, const char *label, uint8_t &instance)
{
    const ModuleType *type = m_registry.find(type_id);
    if (type == nullptr) {
        return Status::fail(Result::NotFound, Detail::UnknownType);
    }
    if (!labelFits(label)) {
        return Status::fail(Result::InvalidArgument, Detail::BadLabel);
    }
    uint8_t chosen = 0;
    Status st = run([&](std::vector<uint16_t> &touched) {
        bool found = false;
        for (unsigned id = 0; id <= kMaxInstanceId && !found; ++id) {
            if (find(type_id, static_cast<uint8_t>(id)) == nullptr) {
                chosen = static_cast<uint8_t>(id);
                found = true;
            }
        }
        if (!found) {
            return Status::fail(Result::NoMemory, Detail::Capacity);
        }
        PendingEntry e{};
        e.desc = &type->desc;
        e.type_id = type_id;
        e.instance = chosen;
        e.loop = loop;
        copyLabel(e.label, label);
        e.origin = nullptr;
        e.values.resize(type->desc.items.size());
        for (size_t i = 0; i < type->desc.items.size(); ++i) {
            const defs::ItemDesc &item = type->desc.items[i];
            e.values[i] = (item.kind == defs::ItemKind::Param) ? item.default_bits : 0U;
        }
        m_entries.push_back(std::move(e));
        touched.push_back(key(type_id, chosen));
        return Status::success();
    });
    if (!st.ok()) {
        st.index = 0;
        return st;
    }
    instance = chosen;
    return st;
}

Status PendingLayout::removeInstance(uint8_t type_id, uint8_t instance)
{
    Status st = run([&](std::vector<uint16_t> &touched) {
        (void)touched;
        PendingEntry *e = find(type_id, instance);
        if (e == nullptr) {
            return Status::fail(Result::NotFound, Detail::UnknownRef);
        }
        const auto index = static_cast<size_t>(e - m_entries.data());
        const std::vector<LayoutEntry> v = view();
        for (size_t j = 0; j < v.size(); ++j) {
            if (j == index) {
                continue;
            }
            for (const RefLink &link : refLinks(v, j)) {
                if (link.target == index) {
                    return Status::fail(Result::InvalidArgument, Detail::Referenced);
                }
            }
        }
        m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));
        return Status::success();
    });
    st.index = 0;
    return st;
}

Status PendingLayout::setLabel(uint8_t type_id, uint8_t instance, const char *label)
{
    if (!labelFits(label)) {
        return Status::fail(Result::InvalidArgument, Detail::BadLabel);
    }
    Status st = run([&](std::vector<uint16_t> &touched) {
        PendingEntry *e = find(type_id, instance);
        if (e == nullptr) {
            return Status::fail(Result::NotFound, Detail::UnknownRef);
        }
        copyLabel(e->label, label);
        touched.push_back(key(type_id, instance));
        return Status::success();
    });
    st.index = 0;
    return st;
}

void PendingLayout::discard()
{
    m_active = false;
    m_entries.clear();
}

Status PendingLayout::setValues(std::span<const SetEntry> entries, std::span<const uint32_t> applied)
{
    Status st = run([&](std::vector<uint16_t> &touched) {
        for (size_t k = 0; k < entries.size(); ++k) {
            const Ref &r = entries[k].ref;
            PendingEntry *e = find(r.type, r.instance);
            const size_t index = (e != nullptr) ? itemIndex(*e->desc, r.id) : detail::kNoItem;
            if (index == detail::kNoItem) {
                return Status::fail(Result::InvalidArgument, Detail::UnknownRef, static_cast<uint16_t>(k));
            }
            e->values[index] = applied[k];
            touched.push_back(key(r.type, r.instance));
        }
        return Status::success();
    });
    if (!st.ok() && st.detail != Detail::UnknownRef) {
        // check() put the failing entry's (type, inst) in index: name the
        // first batch entry that wrote to it (0 if none did).
        const uint16_t failing = st.index;
        st.index = 0;
        for (size_t k = 0; k < entries.size(); ++k) {
            if (key(entries[k].ref.type, entries[k].ref.instance) == failing) {
                st.index = static_cast<uint16_t>(k);
                break;
            }
        }
    }
    return st;
}

uint32_t PendingLayout::valueAt(const PendingEntry &entry, size_t index)
{
    const defs::ItemDesc &item = entry.desc->items[index];
    if (entry.origin != nullptr && !defs::hasFlag(item, defs::ItemFlag::RebootRequired)) {
        return entry.origin->shadow[index];
    }
    return entry.values[index];
}

Status PendingLayout::get(std::span<const Ref> refs, std::span<uint32_t> values) const
{
    if (values.size() < refs.size()) {
        return Status::fail(Result::InvalidArgument, Detail::BufferTooSmall);
    }
    for (size_t i = 0; i < refs.size(); ++i) {
        const auto index = static_cast<uint16_t>(i);
        const Ref &r = refs[i];
        const PendingEntry *pe = nullptr;
        const detail::InstanceRecord *rec = nullptr;
        if (m_active && r.type != drivelink::protocol::defs::kSystemTypeId) {
            for (const PendingEntry &e : m_entries) {
                if (e.type_id == r.type && e.instance == r.instance) {
                    pe = &e;
                    break;
                }
            }
        } else {
            rec = m_table.find(r.type, r.instance);
        }
        const defs::TypeDesc *desc = (pe != nullptr) ? pe->desc : ((rec != nullptr) ? rec->desc : nullptr);
        const size_t item = (desc != nullptr) ? itemIndex(*desc, r.id) : detail::kNoItem;
        if (item == detail::kNoItem) {
            return Status::fail(Result::InvalidArgument, Detail::UnknownRef, index);
        }
        if (desc->items[item].kind != defs::ItemKind::Param) {
            return Status::fail(Result::InvalidArgument, Detail::WrongKind, index);
        }
        values[i] = (pe != nullptr) ? valueAt(*pe, item) : rec->shadow[item];
    }
    return Status::success();
}

} // namespace core
