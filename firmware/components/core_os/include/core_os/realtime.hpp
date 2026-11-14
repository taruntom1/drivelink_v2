#pragma once

#include <cstdint>

namespace core_os {

// Backend selected by sources. Native records violations; IDF is a no-op.
bool enterRealtime(bool enabled) noexcept;
void leaveRealtime(bool previous) noexcept;
bool realtimeActive() noexcept;
void recordRealtimeViolation() noexcept;
uint64_t realtimeViolations() noexcept;

class RealtimeScope {
public:
    explicit RealtimeScope(bool enabled) noexcept : m_previous(enterRealtime(enabled)) {}
    ~RealtimeScope() { leaveRealtime(m_previous); }
    RealtimeScope(const RealtimeScope &) = delete;
    RealtimeScope &operator=(const RealtimeScope &) = delete;
private:
    bool m_previous;
};
} // namespace core_os
