#pragma once
// A device's history (docs/protocol/agent.md, "History"): a bounded ring of
// entries kept for the agent's lifetime, and the status-stream fields it
// records. Pure; no I/O and no clocks.
#include "drivelink/agent.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace drivelink::agent
{
class History
{
  public:
    explicit History(size_t capacity);
    // Keeps the newest `capacity` entries; a body over 255 bytes is truncated.
    void push(HistoryEntry entry);
    std::vector<HistoryEntry> entries() const; // oldest first
    size_t size() const { return m_count; }
    size_t capacity() const { return m_ring.size(); }

  private:
    std::vector<HistoryEntry> m_ring;
    size_t m_next = 0, m_count = 0;
};

// The arm state and last disarm reason of a status_stream payload
// (core.md, "Status"); nullopt for a malformed one.
struct StatusArm
{
    uint8_t state = 0, last_disarm_reason = 0;
    bool armed() const { return state == 1; }
};
std::optional<StatusArm> parseStatusArm(std::span<const uint8_t> payload);
} // namespace drivelink::agent
