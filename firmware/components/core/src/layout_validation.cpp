// Layout validation (core/layout_validation.hpp): pure, configure time. The
// layouts are small (at most CORE_MAX_INSTANCES entries), so the checks are
// plain quadratic scans.

#include "core/layout_validation.hpp"

#include <cstring>

#include "core/instance.hpp"
#include "core/loop.hpp"

namespace core {

namespace {

bool labelValid(const char *label)
{
    if (label == nullptr || label[0] == '\0') {
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

Status invalid(Detail detail)
{
    return Status::fail(Result::InvalidArgument, detail);
}

/// The pins of entry `e` (each `gpio` param other than kNoGpio): checks them
/// against the policy and each other, and against `claimed`. Adds them to
/// `mine` as a mask.
Status checkPins(const LayoutEntry &e, const ValidationConfig &config, uint64_t claimed, uint64_t &mine)
{
    mine = 0;
    for (size_t k = 0; k < e.desc->items.size(); ++k) {
        const defs::ItemDesc &item = e.desc->items[k];
        if (item.kind != defs::ItemKind::Param || !defs::hasFlag(item, defs::ItemFlag::Gpio)) {
            continue;
        }
        const uint32_t pin = e.values[k];
        if (pin == defs::kNoGpio) {
            continue;
        }
        const uint64_t bit = (pin < 64U) ? (uint64_t{1} << pin) : 0U;
        if (bit == 0U || (config.gpio_allowed_mask & bit) == 0U) {
            return invalid(Detail::GpioNotAllowed);
        }
        if (((mine | claimed) & bit) != 0U) {
            return invalid(Detail::GpioConflict);
        }
        mine |= bit;
    }
    return Status::success();
}

Status checkRefs(std::span<const LayoutEntry> entries, size_t i)
{
    const LayoutEntry &e = entries[i];
    for (const RefLink &link : refLinks(entries, i)) {
        if (!defs::isWiringRef(link.value) || link.target == kNoEntry) {
            return invalid(Detail::BadRef);
        }
        const LayoutEntry &t = entries[link.target];
        if (t.desc == nullptr || !defs::inCategory(t.desc->name, e.desc->items[link.item].ref)) {
            return invalid(Detail::BadRef);
        }
        if (t.loop != e.loop) {
            return invalid(Detail::LoopMismatch);
        }
    }
    return Status::success();
}

/// True if `from` reaches `to` over `edges` (an adjacency list).
bool reaches(const std::vector<std::vector<size_t>> &edges, size_t from, size_t to)
{
    std::vector<bool> seen(edges.size(), false);
    std::vector<size_t> stack(edges[from].begin(), edges[from].end());
    while (!stack.empty()) {
        const size_t n = stack.back();
        stack.pop_back();
        if (n == to) {
            return true;
        }
        if (!seen[n]) {
            seen[n] = true;
            stack.insert(stack.end(), edges[n].begin(), edges[n].end());
        }
    }
    return false;
}

} // namespace

size_t findEntry(std::span<const LayoutEntry> entries, uint8_t type_id, uint8_t instance)
{
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].type_id == type_id && entries[i].instance == instance) {
            return i;
        }
    }
    return kNoEntry;
}

std::vector<RefLink> refLinks(std::span<const LayoutEntry> entries, size_t i)
{
    std::vector<RefLink> links;
    const LayoutEntry &e = entries[i];
    if (e.desc == nullptr) {
        return links;
    }
    for (size_t k = 0; k < e.desc->items.size(); ++k) {
        const defs::ItemDesc &item = e.desc->items[k];
        if (item.kind != defs::ItemKind::Param || item.ref == nullptr) {
            continue;
        }
        const uint32_t value = e.values[k];
        if (value == defs::kUnwired) {
            continue;
        }
        const size_t target = defs::isWiringRef(value)
                                  ? findEntry(entries, defs::wiringRefType(value), defs::wiringRefInstance(value))
                                  : kNoEntry;
        links.push_back(RefLink{k, target, defs::hasFlag(item, defs::ItemFlag::RefTicksAfter), value});
    }
    return links;
}

size_t validateLayout(std::span<const LayoutEntry> entries, const ValidationConfig &config, std::span<Status> out)
{
    const size_t n = entries.size();
    uint64_t claimed = 0;

    // 1. Each entry on its own and against the earlier ones.
    for (size_t i = 0; i < n; ++i) {
        const LayoutEntry &e = entries[i];
        Status st = Status::success();
        if (e.desc == nullptr) {
            st = Status::fail(Result::NotFound, Detail::UnknownType);
        } else if (i >= config.max_instances) {
            st = Status::fail(Result::NoMemory, Detail::Capacity);
        } else if (findEntry(entries.first(i), e.type_id, e.instance) != kNoEntry) {
            st = Status::fail(Result::Exists, Detail::None);
        } else if (e.loop >= kLoopCount || !defs::runsIn(*e.desc, loopClassOf(e.loop))) {
            st = invalid(Detail::LoopMismatch);
        } else if (!labelValid(e.label)) {
            st = invalid(Detail::BadLabel);
        } else {
            for (size_t j = 0; j < i; ++j) {
                if (std::strcmp(entries[j].label, e.label) == 0) {
                    st = invalid(Detail::LabelInUse);
                    break;
                }
            }
        }
        uint64_t mine = 0;
        if (st.ok()) {
            st = checkPins(e, config, claimed, mine);
        }
        if (st.ok()) {
            st = checkRefs(entries, i);
        }
        if (st.ok()) {
            claimed |= mine;
        }
        out[i] = st;
    }

    // 2. Cycles, among the entries still valid.
    std::vector<std::vector<size_t>> ref_edges(n);
    std::vector<std::vector<size_t>> tick_edges(n); // a → b: a ticks before b
    for (size_t i = 0; i < n; ++i) {
        if (!out[i].ok()) {
            continue;
        }
        for (const RefLink &link : refLinks(entries, i)) {
            if (!out[link.target].ok()) {
                continue;
            }
            ref_edges[i].push_back(link.target);
            if (link.ticks_after) {
                tick_edges[i].push_back(link.target);
            } else {
                tick_edges[link.target].push_back(i);
            }
        }
    }
    std::vector<bool> on_cycle(n, false);
    for (size_t i = 0; i < n; ++i) {
        on_cycle[i] = out[i].ok() && (reaches(ref_edges, i, i) || reaches(tick_edges, i, i));
    }
    for (size_t i = 0; i < n; ++i) {
        if (on_cycle[i]) {
            out[i] = invalid(Detail::WiringCycle);
        }
    }

    // 3. Dependencies: wired to an entry that failed, transitively.
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < n; ++i) {
            if (!out[i].ok()) {
                continue;
            }
            for (const RefLink &link : refLinks(entries, i)) {
                if (!out[link.target].ok()) {
                    out[i] = invalid(Detail::DependencyMissing);
                    changed = true;
                    break;
                }
            }
        }
    }

    size_t failed = 0;
    for (size_t i = 0; i < n; ++i) {
        failed += out[i].ok() ? 0U : 1U;
    }
    return failed;
}

} // namespace core
