#pragma once
// Pure agent logic (docs/protocol/agent.md): the routing policy for client
// frames, each device's virtual group map (subscription and command slots)
// and LIST synthesis.
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
    Forward,       // open to every client: forward on the device link with a new seq
    Disarm,        // forward at once on the fast path (0x03 or system op 3)
    Malformed,     // 0x03 with another family/op: answered as a device would
    Arm,           // system ARM: the lease holder only
    Exclusive,     // the lease, or nobody holding it and the device not armed
    Group,         // a subscription-group op or LIST, virtualized
    CmdGroup,      // CMD_CONFIG / CMD_CLEAR: the lease holder only, virtualized
    CommandFrame,  // a command-group frame (0xA0..0xBF): the holder's, rewritten
    Diag,          // diag_query: forward with a new seq
    Agent,         // agent_command
    Drop,          // dropped and counted (time pings, other types)
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

enum class Kind : uint8_t { Sub = 0, Cmd = 1 };

// Device group slots of both kinds and the clients that own them. Every slot
// call takes the kind (subscriptions by default); slots, epochs and records
// of the two kinds are independent.
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
        uint64_t serial = 0;     // which allocation of the slot this is
    };
    // Device online or Boot: every slot free, every client record dropped.
    void reset(uint8_t subs, uint8_t cmds);
    // The pool sizes read after a Boot (they are settings). Keeps the mappings:
    // a slot past a smaller pool is only still known to the request on it, which
    // the device refuses.
    void resize(uint8_t subs, uint8_t cmds);
    uint8_t subs() const { return count(Kind::Sub); }
    uint8_t cmds() const { return count(Kind::Cmd); }
    uint8_t count(Kind kind) const { return m_count[static_cast<size_t>(kind)]; }
    const Slot &slot(uint8_t device_slot, Kind kind = Kind::Sub) const { return pool(kind).at(device_slot); }

    // The device slot behind a client's id, if any.
    std::optional<uint8_t> find(ClientId client, uint8_t vid, Kind kind = Kind::Sub) const;
    // find(), else take the lowest free slot for it; nullopt when none is left.
    // Counts one request in flight on the slot.
    std::optional<uint8_t> acquire(ClientId client, uint8_t vid, Kind kind = Kind::Sub);
    // Count one more request in flight on an existing mapping.
    void begin(uint8_t device_slot, Kind kind = Kind::Sub) { ++pool(kind).at(device_slot).inflight; }
    // A forwarded request on the slot finished; `serial` is slot().serial when
    // it was sent, so an answer for an earlier allocation changes nothing. accepted: the device returned
    // epoch; cleared: it was an accepted CLEAR. A refused or unanswered
    // request on a slot that never had an accepted mutation frees it once
    // nothing else is in flight; an accepted CLEAR frees it the same way.
    void finish(uint8_t device_slot, uint64_t serial, bool accepted, uint8_t epoch, bool cleared, bool uncertain,
                Kind kind = Kind::Sub);
    // Stream routing (subscriptions): the owner and its id, when the frame's epoch is current.
    std::optional<std::pair<ClientId, uint8_t>> route(uint8_t device_slot, uint8_t epoch) const;
    // A client left (Sub) or lost the lease (Cmd): its slots of that kind
    // become Dirty (returned for CLEAR) and its records of that kind go.
    std::vector<uint8_t> release(ClientId client, Kind kind = Kind::Sub);
    // A Free slot found configured on the device (a mutation that landed after
    // a Boot reset the map): Dirty until cleared.
    void markDirty(uint8_t device_slot, Kind kind = Kind::Sub);
    // CLEAR of a Dirty slot succeeded.
    void cleaned(uint8_t device_slot, Kind kind = Kind::Sub);
    std::vector<uint8_t> dirty(Kind kind = Kind::Sub) const;
    // The last epoch a client saw for one of its ids (0 if none).
    uint8_t lastEpoch(ClientId client, uint8_t vid, Kind kind = Kind::Sub) const;
    void setLastEpoch(ClientId client, uint8_t vid, uint8_t epoch, Kind kind = Kind::Sub);

  private:
    std::vector<Slot> &pool(Kind kind) { return m_slots[static_cast<size_t>(kind)]; }
    const std::vector<Slot> &pool(Kind kind) const { return m_slots[static_cast<size_t>(kind)]; }
    std::array<std::vector<Slot>, 2> m_slots;
    std::array<uint8_t, 2> m_count{};
    uint64_t m_serial = 0;
    struct Seen { ClientId client; Kind kind; uint8_t vid; uint8_t epoch; };
    std::vector<Seen> m_seen;
};

// A client's LIST answer body: its own slots of both kinds (from the device
// entries, group id rewritten), every other id empty with its last epoch; paged from `start`
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
