// The wiring order (core/wiring_order.hpp): stable Kahn sorts over the
// layout index. Pure, configure time.

#include "core/wiring_order.hpp"


namespace core {

namespace {

/// `before[b]` lists the entries that must come before b. Picks the lowest
/// ready index each round.
std::vector<size_t> stableSort(const std::vector<bool> &include, const std::vector<std::vector<size_t>> &before)
{
    const size_t n = include.size();
    std::vector<size_t> order;
    std::vector<bool> done(n, false);
    size_t wanted = 0;
    for (size_t i = 0; i < n; ++i) {
        wanted += include[i] ? 1U : 0U;
    }
    order.reserve(wanted);
    while (order.size() < wanted) {
        bool progressed = false;
        for (size_t i = 0; i < n; ++i) {
            if (!include[i] || done[i]) {
                continue;
            }
            bool ready = true;
            for (const size_t p : before[i]) {
                if (!done[p]) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                done[i] = true;
                order.push_back(i);
                progressed = true;
                break; // restart from the lowest index
            }
        }
        if (!progressed) {
            // A cycle: validation rules it out; keep the rest in index order.
            for (size_t i = 0; i < n; ++i) {
                if (include[i] && !done[i]) {
                    done[i] = true;
                    order.push_back(i);
                }
            }
        }
    }
    return order;
}

std::vector<size_t> sortBy(std::span<const LayoutEntry> entries, const std::vector<bool> &include, bool tick)
{
    const size_t n = entries.size();
    std::vector<std::vector<size_t>> before(n);
    for (size_t i = 0; i < n; ++i) {
        if (!include[i]) {
            continue;
        }
        for (const RefLink &link : refLinks(entries, i)) {
            if (link.target == kNoEntry || !include[link.target] ||
                link.target == i) {
                continue;
            }
            if (tick && link.ticks_after) {
                before[link.target].push_back(i); // the target ticks after the entry
            } else {
                before[i].push_back(link.target); // the target comes first
            }
        }
        if (tick) {
            for (const auto &link : sourceLinks(entries, i)) {
                if (link.target != kNoEntry && include[link.target] &&
                    signalLoop(entries[link.target]) == entries[i].loop) {
                    before[i].push_back(link.target);
                }
            }
        }
    }
    return stableSort(include, before);
}

} // namespace

std::vector<size_t> creationOrder(std::span<const LayoutEntry> entries, const std::vector<bool> &include)
{
    return sortBy(entries, include, false);
}

std::vector<size_t> tickOrder(std::span<const LayoutEntry> entries, const std::vector<bool> &include)
{
    return sortBy(entries, include, true);
}

} // namespace core
