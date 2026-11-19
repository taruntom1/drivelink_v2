#include "lease.hpp"

namespace drivelink::agent
{
Lease::Take Lease::take(ClientId client)
{
    switch (m_phase)
    {
    case Phase::Free:
        m_phase = Phase::Held;
        m_holder = client;
        return Take::Granted;
    case Phase::Held:
        if (m_holder == client) { return Take::AlreadyHolder; }
        m_phase = Phase::Ending;
        m_pending = client;
        return Take::Revoked;
    case Phase::Ending:
        if (m_pending) { return Take::Busy; }
        m_pending = client;
        return Take::Waiting;
    }
    return Take::Busy;
}

bool Lease::release(ClientId client)
{
    if (!mayCommand(client)) { return false; }
    m_phase = Phase::Ending;
    m_pending = 0;
    return true;
}

Lease::Left Lease::leave(ClientId client)
{
    if (m_phase == Phase::Held && m_holder == client)
    {
        m_phase = Phase::Ending;
        m_pending = 0;
        return Left::Holder;
    }
    if (m_phase == Phase::Ending && m_pending == client)
    {
        m_pending = 0;
        return Left::Pending;
    }
    return Left::None;
}

std::optional<ClientId> Lease::finish()
{
    if (m_phase != Phase::Ending) { return std::nullopt; }
    if (m_pending)
    {
        m_phase = Phase::Held;
        m_holder = m_pending;
        m_pending = 0;
        return m_holder;
    }
    *this = Lease{};
    return std::nullopt;
}
} // namespace drivelink::agent
