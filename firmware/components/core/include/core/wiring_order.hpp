#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "core/layout_validation.hpp"

namespace core {

/// Creation order: every included entry after the entries it is wired to. A
/// stable Kahn sort over the index (`include` has one flag per entry): among
/// the ready entries the lowest index goes first. Links to entries that are
/// not included are ignored. An entry left on a cycle (validation rules that
/// out) goes last, in index order. Pure, configure time (it allocates).
std::vector<size_t> creationOrder(std::span<const LayoutEntry> entries, const std::vector<bool> &include);

/// Tick order: like the creation order, but a link flagged `RefTicksAfter`
/// puts its target after the entry instead of before it. Feed each loop's
/// instances to `Loop::addInstance` in this order.
std::vector<size_t> tickOrder(std::span<const LayoutEntry> entries, const std::vector<bool> &include);

} // namespace core
