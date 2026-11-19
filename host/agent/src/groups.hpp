#pragma once
// Pure agent logic (docs/protocol/agent.md): the routing policy for client
// frames, each device's virtual subscription-group map and LIST synthesis.
// No I/O and no clocks; the server in agent.cpp drives it on one thread.
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace drivelink::agent
{
using ClientId = uint64_t;

// What the agent does with a frame from a device connection's client.
enum class Route : uint8_t
{
    Forward,       // forward on the device link with a new seq
    Disarm,        // forward at once on the fast path (0x03 or system op 3)
    Malformed,     // 0x03 with another family/op: answered as a device would
    RefuseLease,   // answer Busy/LeaseRequired (interim rule, H3a-1)
    Group,         // a subscription-group op the agent virtualizes
    Diag,          // diag_query: forward with a new seq
    Agent,         // agent_command
    Drop,          // dropped and counted (time pings, command frames, other types)
};
// msg_type and, for core_command/disarm, the header's family and op.
Route classify(uint8_t msg_type, uint8_t family = 0, uint8_t op = 0);

// One entry of a device LIST page (core.md, "Group").
struct GroupEntry
{
    uint8_t kind = 0, group = 0, flags = 0, loop = 0xFF, epoch = 0;
    uint16_t divider = 1;
    std::vector<std::array<uint8_t, 4>> refs; // raw Ref bytes
    size_t size() const { return 8 + 4 * refs.size(); }
};
struct ListPage
{
    uint16_t total = 0, next = 0;
    std::vector<GroupEntry> entries;
};
// Parse a LIST reply body; refuses truncated entries and trailing bytes.
std::expected<ListPage, std::string> parseListPage(std::span<const uint8_t> body);
inline constexpr uint16_t kListEnd = 0xFFFF;

// Device subscription slots and the clients that own them. Command groups are
// not handed out in H3a-1 (interim rule).
class GroupMap
{
  public:
    enum class State : uint8_t { Free, Owned, Dirty };
    struct Slot
    {
        State state = State::Free;
        ClientId client = 0;
        uint8_t vid = 0;      // the owner's id for it
        unsigned inflight = 0; // agent requests not yet answered for it
        bool configured = false; // the owner's last accepted mutation left it configured
        bool maybe = false;      // an unanswered mutation may have configured it
        uint8_t epoch = 0;       // from the owner's last accepted mutation
    };
    // Device online or Boot: every slot free, every client record dropped.
    void reset(uint8_t subs, uint8_t cmds);
    uint8_t subs() const { return static_cast<uint8_t>(m_slots.size()); }
    uint8_t cmds() const { return m_cmds; }
    const Slot &slot(uint8_t device_slot) const { return m_slots.at(device_slot); }

    // The device slot behind a client's id, if any.
    std::optional<uint8_t> find(ClientId client, uint8_t vid) const;
    // find(), else take the lowest free slot for it; nullopt when none is left.
    // Counts one request in flight on the slot.
    std::optional<uint8_t> acquire(ClientId client, uint8_t vid);
    // Count one more request in flight on an existing mapping.
    void begin(uint8_t device_slot) { ++m_slots.at(device_slot).inflight; }
    // A forwarded request on the slot finished. accepted: the device returned
    // epoch; cleared: it was an accepted CLEAR. A refused or unanswered
    // request on a slot that never had an accepted mutation frees it once
    // nothing else is in flight; an accepted CLEAR frees it the same way.
    void finish(uint8_t device_slot, bool accepted, uint8_t epoch, bool cleared, bool uncertain);
    // Stream routing: the owner and its id, when the frame's epoch is current.
    std::optional<std::pair<ClientId, uint8_t>> route(uint8_t device_slot, uint8_t epoch) const;
    // A client left: its slots become Dirty (returned for CLEAR) and its records go.
    std::vector<uint8_t> release(ClientId client);
    // CLEAR of a Dirty slot succeeded.
    void cleaned(uint8_t device_slot);
    std::vector<uint8_t> dirty() const;
    // The last epoch a client saw for one of its ids (0 if none).
    uint8_t lastEpoch(ClientId client, uint8_t vid) const;
    void setLastEpoch(ClientId client, uint8_t vid, uint8_t epoch);

  private:
    std::vector<Slot> m_slots;
    uint8_t m_cmds = 0;
    struct Seen { ClientId client; uint8_t vid; uint8_t epoch; };
    std::vector<Seen> m_seen;
};

// A client's LIST answer body: its own slots (from the device entries, group
// id rewritten), every other id empty with its last epoch; paged from `start`
// with whole entries within `max_body` bytes. nullopt when start > total
// (InvalidArgument), or when one entry cannot fit (BufferTooSmall).
struct ListAnswer
{
    std::vector<uint8_t> body;
    bool too_small = false;
};
std::optional<ListAnswer> synthesizeList(const GroupMap &map, ClientId client,
                                         std::span<const GroupEntry> device_entries,
                                         uint16_t start, size_t max_body);
} // namespace drivelink::agent
