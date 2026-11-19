#include "history.hpp"
#include <stdexcept>

namespace drivelink::agent
{
History::History(size_t capacity)
{
    if (!capacity) { throw std::invalid_argument("history capacity must be at least 1"); }
    m_ring.resize(capacity);
}

void History::push(HistoryEntry entry)
{
    if (entry.body.size() > 255) { entry.body.resize(255); }
    m_ring[m_next] = std::move(entry);
    m_next = (m_next + 1) % m_ring.size();
    if (m_count < m_ring.size()) { ++m_count; }
}

std::vector<HistoryEntry> History::entries() const
{
    std::vector<HistoryEntry> out;
    out.reserve(m_count);
    const size_t first = (m_next + m_ring.size() - m_count) % m_ring.size();
    for (size_t i = 0; i < m_count; ++i) { out.push_back(m_ring[(first + i) % m_ring.size()]); }
    return out;
}

std::optional<StatusArm> parseStatusArm(std::span<const uint8_t> p)
{
    // version | epoch | counter u16 | timestamp u64 | arm_state | flags | errors |
    // cpu_load[2] | loop_count | loop_count * 5 | last_disarm_reason
    constexpr size_t kFixed = 19, kLoopCount = 17;
    if (p.size() < kFixed) { return std::nullopt; }
    const size_t loops = p[kLoopCount];
    if (p.size() != kFixed + 5 * loops) { return std::nullopt; }
    return StatusArm{p[12], p[p.size() - 1]};
}
} // namespace drivelink::agent
