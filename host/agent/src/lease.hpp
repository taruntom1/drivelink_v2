#pragma once
// The control lease of one device (docs/protocol/agent.md, "The lease"): pure
// state, no I/O. The server in agent.cpp drives it on the agent thread and
// carries out what it returns (DISARM, clearing the old holder's command
// groups, Lease events).
#include <cstdint>
#include <optional>

namespace drivelink::agent
{
using ClientId = uint64_t; // the agent's connection id; 0 is never a client

class Lease
{
  public:
    // Free: nobody holds it. Held: holder() does. Ending: a holder's lease
    // ended and the agent is disarming and clearing its command groups;
    // pending() (if any) is granted when that completes.
    enum class Phase : uint8_t { Free, Held, Ending };

    Phase phase() const { return m_phase; }
    ClientId holder() const { return m_phase == Phase::Held ? m_holder : 0; }
    ClientId pending() const { return m_pending; }
    // The client whose lease is ending (Ending only).
    ClientId ending() const { return m_phase == Phase::Ending ? m_holder : 0; }

    enum class Take : uint8_t
    {
        AlreadyHolder, // answer Ok, nothing changes
        Granted,       // it was free: the caller holds it now (Lease event, no DISARM)
        Revoked,       // the holder lost it: send its revocation, DISARM, clear, then finish()
        Waiting,       // another lease is already ending: granted by finish()
        Busy,          // a Take is already waiting: answer Busy/None
    };
    Take take(ClientId client);

    // The holder gives it up: true when `client` held it (Phase becomes
    // Ending: DISARM, clear, finish()). False: answer WrongState.
    bool release(ClientId client);

    // A connection closed. Holder: Ending as release(). Pending: its Take is
    // dropped (the end still completes). Returns what it was.
    enum class Left : uint8_t { None, Holder, Pending };
    Left leave(ClientId client);

    // The DISARM and the CLEARs of an Ending lease completed. Returns the
    // client granted now (pending), or nullopt (Free).
    std::optional<ClientId> finish();

    // The device went offline: everything is dropped (connections close).
    void reset() { *this = Lease{}; }

    // ARM, CMD_CONFIG, CMD_CLEAR and command frames.
    bool mayCommand(ClientId client) const { return m_phase == Phase::Held && m_holder == client; }
    // Exclusive operations: the holder, or nobody holding it, nothing ending,
    // and the device not armed.
    bool mayExclusive(ClientId client, bool device_armed) const
    {
        return mayCommand(client) || (m_phase == Phase::Free && !device_armed);
    }

  private:
    Phase m_phase = Phase::Free;
    ClientId m_holder = 0;  // Held: the holder; Ending: the one whose lease ends
    ClientId m_pending = 0; // Ending: the client a Take is waiting for
};

// What a lease end still waits for (agent.md, "The end of a lease"): its
// DISARM; every ARM in flight and the extra DISARM each needs once its
// sender no longer holds the lease (and any DISARM the agent sends because the
// status shows the device armed with nobody holding it); then the CLEARs of
// the old holder's command groups. Pure counters; the agent sends the frames.
class LeaseEnd
{
  public:
    // ARMs in flight (any holder) and extra DISARMs in flight; counted
    // whether or not a lease is ending, so an end that starts later waits too.
    void armSent() { ++m_arms; }
    void armDone() { if (m_arms) { --m_arms; } }
    void redisarmSent() { ++m_redisarms; }
    void redisarmDone() { if (m_redisarms) { --m_redisarms; } }
    unsigned arms() const { return m_arms; }

    // A lease end begins: its DISARM was sent.
    void start() { m_ending = true; m_disarm = true; m_clearing = false; m_clears = 0; }
    bool ending() const { return m_ending; }
    void disarmDone() { m_disarm = false; }
    // Send the CLEARs now: the end's DISARM, every ARM and every extra DISARM completed.
    bool readyToClear() const { return m_ending && !m_clearing && !m_disarm && !m_arms && !m_redisarms; }
    void clearsStarted(unsigned n) { m_clearing = true; m_clears = n; }
    void clearDone() { if (m_clears) { --m_clears; } }
    // Grant (or free) the lease now: every CLEAR completed.
    bool readyToFinish() const { return m_ending && m_clearing && !m_clears; }
    void finished() { m_ending = false; m_disarm = false; m_clearing = false; m_clears = 0; }
    void reset() { *this = LeaseEnd{}; }

  private:
    unsigned m_arms = 0, m_redisarms = 0, m_clears = 0;
    bool m_ending = false, m_disarm = false, m_clearing = false;
};
} // namespace drivelink::agent
