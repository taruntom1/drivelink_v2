#include "agent.hpp"
#include "mdns.hpp"
#include "groups.hpp"
#include "history.hpp"
#include "lease.hpp"
#include "drivelink/agent.hpp"
#include "drivelink/client.hpp"
#include "drivelink/session.hpp"
#include "drivelink/time.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/scanner.hpp"
#include <asio.hpp>
#include <unistd.h>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace drivelink::agent
{
namespace
{
namespace p = protocol;
namespace defs = protocol::defs;
namespace msg = protocol::defs::msg;

constexpr size_t kMaxClients = 64;          // accepted (helloed) connections
constexpr size_t kMaxDeviceClients = 16;    // device connections per device (a ClientList fits one frame)
constexpr size_t kMaxConnections = 128;     // including those still before Hello
constexpr size_t kStreamQueueLimit = 256 * 1024; // drop stream frames above this
constexpr size_t kQueueLimit = 4 * 1024 * 1024;  // close a client that falls this far behind
constexpr unsigned kClientInflight = 32;         // forwarded requests per client (DISARM exempt)
constexpr auto kCloseDeadline = std::chrono::milliseconds(1000); // flush time for a Closing event
constexpr size_t kMaxListPages = 64;
constexpr auto kClockPeriod = std::chrono::milliseconds(500); // how often a changed clock model is sent
constexpr size_t kMaxEventBody = p::kMaxPayload - 2;           // agent_event: version | event | body
constexpr uint8_t kCoreVersion = p::core_wire::kVersion;

uint8_t u8(defs::Result r) { return static_cast<uint8_t>(r); }
uint8_t u8(defs::Detail d) { return static_cast<uint8_t>(d); }
bool accepted(uint8_t result) { return result == u8(defs::Result::Ok) || result == u8(defs::Result::PendingReboot); }

void log(const std::string &line) { std::cerr << "drivelink-agent: " << line << std::endl; }

Bytes frame(uint8_t type, std::span<const uint8_t> payload)
{
    Bytes out(p::frameSize(payload.size()));
    if (!p::encodeFrame(out.data(), out.size(), type, payload.data(), payload.size()))
    {
        throw std::length_error("frame payload of " + std::to_string(payload.size()) + " bytes exceeds MAX_PAYLOAD");
    }
    return out;
}
Bytes coreReply(uint8_t seq, uint8_t family, uint8_t op, uint8_t result, uint8_t detail, std::span<const uint8_t> body = {})
{
    Bytes out{kCoreVersion, seq, family, op, result, detail};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
uint64_t unixMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string clipped(const std::string &s, size_t max) { return s.size() <= max ? s : s.substr(0, max); }
} // namespace

struct Device;
struct Connection;

struct Agent::Impl
{
    AgentConfig config;
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    asio::ip::tcp::acceptor acceptor{io};
    asio::signal_set signals{io};
    std::vector<std::unique_ptr<Device>> devices;
    std::map<uint64_t, std::shared_ptr<Connection>> clients;
    uint64_t next_client = 1;
    uint16_t next_wire = 0;
    bool stopping = false;
    std::unique_ptr<Advertiser> advertiser; // mDNS, LAN binds only (run() to shutdown())

    explicit Impl(AgentConfig c) : config(std::move(c)) {}
    ~Impl();
    void accept();
    size_t helloed() const;
    void shutdown();
    agent::DeviceRecord record(const Device &d) const;
    Device *findDevice(const std::string &name);
    uint16_t allocWireId(); // nonzero, not used by an open connection
    Connection *client(uint64_t id) const;
};

struct Connection : std::enable_shared_from_this<Connection>
{
    enum class State { AwaitHello, Lobby, Device, Closed };
    Agent::Impl &agent;
    asio::ip::tcp::socket socket;
    uint64_t id;
    uint16_t wire_id = 0; // client_id on the wire
    std::string name;
    uint8_t kind = 0; // AgentClientKind from the Hello
    State state = State::AwaitHello;
    struct Device *device = nullptr;
    p::Scanner scanner{p::kMaxPayload};
    std::array<uint8_t, 4096> input{};
    std::deque<std::shared_ptr<Bytes>> out;
    size_t out_bytes = 0;
    bool writing = false, close_after_write = false;
    asio::steady_timer hello_timer, close_timer;
    uint64_t dropped = 0;
    unsigned inflight = 0; // forwarded requests not yet answered

    Connection(Agent::Impl &a, asio::ip::tcp::socket s, uint64_t i)
        : agent(a), socket(std::move(s)), id(i), hello_timer(a.io), close_timer(a.io) {}
    void start();
    void read();
    void handle(const p::Scanner::Frame &f);
    void hello(const p::Scanner::Frame &f);
    void agentCommand(const p::Scanner::Frame &f);
    void deviceFrame(const p::Scanner::Frame &f);
    void send(uint8_t type, std::span<const uint8_t> payload, bool stream = false);
    void sendAgent(uint8_t seq, uint8_t op, uint8_t result, uint8_t detail, std::span<const uint8_t> body = {});
    void write();
    // reason: send a Closing event first (AgentCloseReason), 0 for none.
    void close(uint8_t reason = 0);
    void finishClose();
};

struct Device
{
    Agent::Impl &agent;
    DeviceConfig config;
    std::shared_ptr<Session> session; // shared so in-flight work can't outlive it
    uint64_t observer = 0;
    bool online = false, connecting = false, losing = false;
    uint64_t link = 0;         // bumps on every online/offline transition
    uint64_t generation = 0;   // the Session's boot generation last seen
    uint64_t map_generation = 0; // bumps on every GroupMap reset
    std::array<uint8_t, 6> device_id{};
    std::string name;
    uint16_t max_payload = p::kMaxPayload;
    GroupMap groups;
    std::array<std::set<uint8_t>, 2> clearing; // per Kind: slots with an agent CLEAR in flight
    std::set<std::shared_ptr<Connection>> clients;
    asio::steady_timer retry, maintenance, clock_timer;
    std::thread connector;
    std::string last_failure;
    uint64_t dropped = 0;
    // A DISARM goes out at once. Only when the Session cannot admit another
    // (its reserved DISARM slot is taken while ordinary requests fill the
    // queue) does it wait, at most one per connection, and those waiting are
    // sent together as one DISARM when one in flight completes. The agent's
    // own DISARMs (a lease ending) are waiters with `done` and no client.
    struct Waiter
    {
        std::weak_ptr<Connection> client;
        uint8_t seq = 0, family = 0, op = 0;
        std::function<void()> done; // the agent's own: runs once the DISARM completed, answered or not
    };
    unsigned disarm_inflight = 0;
    std::vector<Waiter> disarm_next;
    // The lease (docs/protocol/agent.md, "The lease").
    Lease lease;
    uint64_t lease_serial = 0; // bumps on every lease end and link change; stale completions compare it
    LeaseEnd end;              // what an ending lease still waits for
    bool backstop = false;     // a DISARM for "armed with nobody holding the lease" is in flight
    struct TakeWaiter { std::weak_ptr<Connection> client; uint8_t seq = 0; };
    std::optional<TakeWaiter> take_waiter;
    // Status-stream arm state, history and the clock model sent to clients.
    std::optional<StatusArm> arm;
    History history;
    ClockModel clock_sent;

    Device(Agent::Impl &a, DeviceConfig c)
        : agent(a), config(std::move(c)), retry(a.io), maintenance(a.io), clock_timer(a.io), history(a.config.history_size) {}
    ~Device() { if (connector.joinable()) { connector.join(); } }
    void open();
    void connected(std::shared_ptr<Session> s, DeviceInfo info, GroupList list, uint64_t observer_id, std::shared_ptr<bool> lost);
    void failed(const std::string &why);
    void offline(const std::string &why);
    void linkLost();
    void frame(const Frame &f);
    void boot();
    void refreshIdentity();
    void cleanDirty();
    void scheduleMaintenance();
    // Forwarding: completions run on the agent thread.
    // false: refused locally (answered Busy) without reaching the device.
    // `after` runs on completion (answered or not) while the link is the same.
    bool forwardCore(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t family, uint8_t op, Bytes body,
                     std::function<void()> after = {});
    void forwardArm(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t family, uint8_t op, Bytes body);
    void forwardDiag(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op, Bytes body);
    void groupOp(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op, Bytes body);
    void listGroups(const std::shared_ptr<Connection> &c, uint8_t seq, uint16_t start);
    void commandFrame(const std::shared_ptr<Connection> &c, uint8_t vid, std::span<const uint8_t> payload);
    void release(const std::shared_ptr<Connection> &c);
    void disarm(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t family, uint8_t op);
    void disarmInternal(std::function<void()> done);
    bool sendDisarm(std::vector<Waiter> waiters); // false: not admitted
    // Lease
    void leaseTake(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op);
    void leaseRelease(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op);
    void endLease(ClientId old);
    void advanceEnd(); // CLEARs, then the grant, as `end` allows
    void finishLease();
    void announceLease(uint16_t holder, const std::string &holder_name, uint8_t holder_kind, uint16_t previous,
                       defs::LeaseChange reason);
    bool commandSlotsLeft() const; // an earlier holder's command slot may still be on the device
    // Clients
    void greet(const std::shared_ptr<Connection> &c); // History, then ClockModel
    void clientList(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op);
    void record(defs::HistoryKind kind, Bytes body);
    void broadcastEvent(defs::AgentEvent event, std::span<const uint8_t> body);
    void scheduleClock();
};

namespace
{
// A request that failed before it could reach the device (queue full, IDs
// exhausted, link already gone) is answered Busy; a timeout stays unanswered.
bool localFailure(const Error &e) { return !e.outcome_unknown && e.kind != ErrorKind::Timeout; }
} // namespace

// ---------------------------------------------------------------- Connection ----

void Connection::start()
{
    hello_timer.expires_after(agent.config.hello_timeout);
    hello_timer.async_wait([weak = weak_from_this()](auto ec) {
        auto self = weak.lock();
        if (!ec && self && self->state == State::AwaitHello) { self->close(); }
    });
    read();
}

void Connection::read()
{
    socket.async_read_some(asio::buffer(input), [self = shared_from_this()](auto ec, size_t n) {
        if (self->state == State::Closed) { return; }
        if (ec) { self->finishClose(); return; }
        for (size_t offset = 0; offset < n;)
        {
            size_t space = 0;
            auto *dst = self->scanner.writable(space);
            const auto count = std::min(space, n - offset);
            if (!count) { self->finishClose(); return; }
            std::copy_n(self->input.data() + offset, count, dst);
            self->scanner.commit(count);
            offset += count;
            p::Scanner::Frame f;
            while (self->state != State::Closed && !self->close_after_write && self->scanner.next(f)) { self->handle(f); }
        }
        if (self->state != State::Closed && !self->close_after_write) { self->read(); }
    });
}

void Connection::handle(const p::Scanner::Frame &f)
{
    switch (state)
    {
    case State::AwaitHello: hello(f); return;
    case State::Lobby:
        if (f.msg_type == msg::kAgentCommand) { agentCommand(f); }
        else { close(); }
        return;
    case State::Device: deviceFrame(f); return;
    case State::Closed: return;
    }
}

void Connection::send(uint8_t type, std::span<const uint8_t> payload, bool stream)
{
    if (state == State::Closed || close_after_write) { return; }
    if (stream && out_bytes > kStreamQueueLimit) { ++dropped; return; }
    if (out_bytes > kQueueLimit)
    {
        // Never close inline: callers may be iterating a device's clients.
        log("client " + std::to_string(id) + " fell behind; closing it");
        close_after_write = true;
        asio::post(agent.io, [self = shared_from_this()] { self->finishClose(); });
        return;
    }
    std::shared_ptr<Bytes> bytes;
    try { bytes = std::make_shared<Bytes>(frame(type, payload)); }
    catch (const std::length_error &e) { log(e.what()); return; }
    out_bytes += bytes->size();
    out.push_back(std::move(bytes));
    if (!writing) { write(); }
}

void Connection::sendAgent(uint8_t seq, uint8_t op, uint8_t result, uint8_t detail, std::span<const uint8_t> body)
{
    const auto payload = encodeResponse({seq, op, result, detail, Bytes(body.begin(), body.end())});
    send(msg::kAgentResponse, payload);
}

void Connection::write()
{
    if (out.empty())
    {
        writing = false;
        if (close_after_write) { finishClose(); }
        return;
    }
    writing = true;
    auto bytes = out.front();
    asio::async_write(socket, asio::buffer(*bytes), [self = shared_from_this(), bytes](auto ec, size_t) {
        if (self->state == State::Closed) { return; }
        if (ec) { self->finishClose(); return; }
        self->out_bytes -= bytes->size();
        self->out.pop_front();
        self->write();
    });
}

void Connection::close(uint8_t reason)
{
    if (state == State::Closed || close_after_write) { return; }
    if (reason)
    {
        const auto event = encodeEvent({static_cast<uint8_t>(defs::AgentEvent::Closing), encodeClosing(reason)});
        send(msg::kAgentEvent, event);
    }
    close_after_write = true;
    if (!writing) { finishClose(); return; }
    // A peer that stopped reading must not hold the connection open.
    close_timer.expires_after(kCloseDeadline);
    close_timer.async_wait([weak = weak_from_this()](auto ec) {
        if (auto self = weak.lock(); !ec && self) { self->finishClose(); }
    });
}

void Connection::finishClose()
{
    if (state == State::Closed) { return; }
    const bool had_device = state == State::Device;
    state = State::Closed;
    hello_timer.cancel();
    close_timer.cancel();
    asio::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    out.clear();
    out_bytes = 0;
    auto self = shared_from_this();
    if (had_device && device) { device->release(self); }
    agent.clients.erase(id);
}

void Connection::hello(const p::Scanner::Frame &f)
{
    // Only an agent Hello may come first; anything else closes silently.
    if (f.msg_type != msg::kAgentCommand || f.len < 3) { close(); return; }
    const uint8_t seq = f.payload[1], op = f.payload[2];
    const auto hello_op = static_cast<uint8_t>(defs::AgentOp::Hello);
    auto refuse = [&](defs::Result r, defs::Detail d, Bytes body = {}) {
        sendAgent(seq, op, u8(r), u8(d), body);
        close();
    };
    const auto version_body = encodeVersionRefusal({kVersion, kCoreVersion});
    if (f.payload[0] != kVersion) { refuse(defs::Result::InvalidArgument, defs::Detail::AgentVersion, version_body); return; }
    if (op != hello_op) { close(); return; }
    const auto request = decodeHello({f.payload + 3, f.len - 3});
    if (!request) { refuse(defs::Result::InvalidArgument, defs::Detail::None); return; }
    if (request->core_version != kCoreVersion) { refuse(defs::Result::InvalidArgument, defs::Detail::AgentVersion, version_body); return; }
    if (!tokenEquals(request->token, agent.config.token)) { refuse(defs::Result::InvalidArgument, defs::Detail::BadToken); return; }
    if (agent.helloed() >= kMaxClients) { refuse(defs::Result::NoMemory, defs::Detail::Capacity); return; }
    HelloAccepted acceptedHello;
    acceptedHello.client_id = wire_id;
    if (request->device.empty())
    {
        acceptedHello.lobby = true;
        state = State::Lobby;
    }
    else
    {
        auto *d = agent.findDevice(request->device);
        if (!d) { refuse(defs::Result::NotFound, defs::Detail::UnknownDevice); return; }
        if (!d->online) { refuse(defs::Result::Busy, defs::Detail::DeviceOffline); return; }
        if (d->clients.size() >= kMaxDeviceClients) { refuse(defs::Result::NoMemory, defs::Detail::Capacity); return; }
        state = State::Device;
        device = d;
        d->clients.insert(shared_from_this());
        acceptedHello.lobby = false;
    }
    name = request->client_name;
    kind = request->client_kind;
    if (device) { acceptedHello.device = agent.record(*device); }
    hello_timer.cancel();
    sendAgent(seq, op, u8(defs::Result::Ok), u8(defs::Detail::None), encodeHelloAccepted(acceptedHello));
    if (device) { device->greet(shared_from_this()); }
    log("client " + std::to_string(wire_id) + " (" + (name.empty() ? "unnamed" : name) + ") " +
        (device ? "on " + device->config.alias : std::string("in the lobby")));
}

void Connection::agentCommand(const p::Scanner::Frame &f)
{
    if (f.len < 3) { close(); return; }
    const uint8_t seq = f.payload[1], op = f.payload[2];
    if (f.payload[0] != kVersion)
    {
        sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::AgentVersion),
                  encodeVersionRefusal({kVersion, kCoreVersion}));
        return;
    }
    const auto agent_op = static_cast<defs::AgentOp>(op);
    switch (agent_op)
    {
    case defs::AgentOp::Hello:
        sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::WrongState));
        return;
    case defs::AgentOp::ListDevices:
    case defs::AgentOp::LeaseTake:
    case defs::AgentOp::LeaseRelease:
    case defs::AgentOp::ClientList:
    case defs::AgentOp::TimePing: break;
    default: sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::Unsupported)); return;
    }
    if (f.len != 3) { sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::None)); return; }
    switch (agent_op)
    {
    case defs::AgentOp::ListDevices:
    {
        std::vector<DeviceRecord> records;
        for (const auto &d : agent.devices) { records.push_back(agent.record(*d)); }
        sendAgent(seq, op, u8(defs::Result::Ok), u8(defs::Detail::None), encodeDeviceList(records));
        return;
    }
    case defs::AgentOp::TimePing:
        // The frame was read just now on this thread: its arrival time.
        sendAgent(seq, op, u8(defs::Result::Ok), u8(defs::Detail::None), encodeTimePing(monotonicUs()));
        return;
    default: break;
    }
    // Device-connection ops.
    if (state != State::Device || !device || !device->online)
    {
        sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::WrongState));
        return;
    }
    auto self = shared_from_this();
    if (agent_op == defs::AgentOp::LeaseTake) { device->leaseTake(self, seq, op); }
    else if (agent_op == defs::AgentOp::LeaseRelease) { device->leaseRelease(self, seq, op); }
    else { device->clientList(self, seq, op); }
}

void Connection::deviceFrame(const p::Scanner::Frame &f)
{
    auto *d = device;
    if (!d || !d->online) { return; }
    auto self = shared_from_this();
    if (f.msg_type == msg::kCoreCommand || f.msg_type == msg::kDisarm)
    {
        // A malformed header is answered as the device would: family/op 0xFF.
        if (f.len < 4 || f.payload[0] != kCoreVersion)
        {
            const uint8_t seq = f.len >= 2 && f.payload[0] == kCoreVersion ? f.payload[1] : 0;
            send(msg::kCoreResponse, coreReply(seq, 0xFF, 0xFF, u8(defs::Result::InvalidArgument), u8(defs::Detail::None)));
            return;
        }
        const uint8_t seq = f.payload[1], family = f.payload[2], op = f.payload[3];
        Bytes body(f.payload + 4, f.payload + f.len);
        switch (classify(f.msg_type, family, op))
        {
        case Route::Disarm:
            if (!body.empty())
            {
                send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::None)));
                return;
            }
            d->disarm(self, seq, family, op);
            return;
        case Route::Malformed:
            send(msg::kCoreResponse, coreReply(seq, 0xFF, 0xFF, u8(defs::Result::InvalidArgument), u8(defs::Detail::None)));
            return;
        case Route::Arm:
            if (!d->lease.mayCommand(id))
            {
                send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::LeaseRequired)));
                return;
            }
            if (d->commandSlotsLeft())
            {
                // An earlier holder's command group would count towards ARM.
                send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::None)));
                return;
            }
            d->forwardArm(self, seq, family, op, std::move(body));
            return;
        case Route::Exclusive:
            if (!d->lease.mayExclusive(id, d->arm && d->arm->armed()))
            {
                send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::LeaseRequired)));
                return;
            }
            d->forwardCore(self, seq, family, op, std::move(body));
            return;
        case Route::CmdGroup:
            if (!d->lease.mayCommand(id))
            {
                send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::LeaseRequired)));
                return;
            }
            d->groupOp(self, seq, op, std::move(body));
            return;
        case Route::Group: d->groupOp(self, seq, op, std::move(body)); return;
        case Route::Forward: d->forwardCore(self, seq, family, op, std::move(body)); return;
        default: ++dropped; return;
        }
    }
    switch (classify(f.msg_type))
    {
    case Route::Diag:
        if (f.len < 3) { ++dropped; return; }
        d->forwardDiag(self, f.payload[1], f.payload[2], Bytes(f.payload + 3, f.payload + f.len));
        return;
    case Route::Agent: agentCommand(f); return;
    case Route::CommandFrame:
        if (!d->lease.mayCommand(id)) { ++dropped; return; }
        d->commandFrame(self, static_cast<uint8_t>(f.msg_type - defs::kCmdGroupChannelFirst), {f.payload, f.len});
        return;
    default: ++dropped; return; // time pings, other types
    }
}

// ---------------------------------------------------------------- Device ----

void Device::open()
{
    if (agent.stopping || online || connecting) { return; }
    connecting = true;
    if (connector.joinable()) { connector.join(); }
    SessionOptions options;
    options.baud = config.baud;
    options.timeout = agent.config.request_timeout;
    options.request_capacity = 128;
    options.event_capacity = 4096;
    options.observe_streams = true;
    options.retire_quarantine = std::max(std::chrono::milliseconds(30000), 10 * agent.config.request_timeout);
    options.dispatcher = [io = &agent.io](Task task) { asio::post(*io, std::move(task)); };
    // Opening and setup use the synchronous API, off the agent thread.
    connector = std::thread([this, options, endpoint = config.endpoint, io = &agent.io]() mutable {
        try
        {
            auto session = std::make_shared<Session>(endpoint, options);
            // Observe before setup, so a link lost before connected() runs is seen.
            auto lost = std::make_shared<bool>(false);
            const auto observer_id = session->observe([this, raw = session.get(), lost](const Frame &f) {
                if (f.type == 0) { *lost = true; }
                if (this->session.get() == raw && online) { frame(f); }
            });
            drivelink::Client client(*session);
            auto info = client.info(); // checks the core version
            client.disarm();
            auto list = client.groups();
            for (const auto &g : list.groups)
            {
                if (g.configured) { client.groupClear(g.id, g.kind == GroupKind::Command); }
            }
            list = client.groups();
            asio::post(*io, [this, session, info = std::move(info), list = std::move(list), observer_id, lost]() mutable {
                connected(std::move(session), std::move(info), std::move(list), observer_id, std::move(lost));
            });
        }
        catch (const std::exception &e)
        {
            asio::post(*io, [this, why = std::string(e.what())] { failed(why); });
        }
    });
}

void Device::connected(std::shared_ptr<Session> s, DeviceInfo info, GroupList list, uint64_t observer_id, std::shared_ptr<bool> lost)
{
    connecting = false;
    observer = observer_id;
    losing = false;
    disarm_inflight = 0;
    disarm_next.clear();
    if (agent.stopping) { s->close(); return; }
    session = std::move(s);
    online = true;
    ++link;
    last_failure.clear();
    device_id = info.device_id;
    name = info.name;
    max_payload = info.max_payload ? info.max_payload : p::kMaxPayload;
    generation = session->generation();
    groups.reset(static_cast<uint8_t>(list.subscription_capacity), static_cast<uint8_t>(list.command_capacity));
    ++map_generation;
    for (auto &set : clearing) { set.clear(); }
    lease.reset();
    ++lease_serial;
    end.reset();
    backstop = false;
    take_waiter.reset();
    arm.reset();
    clock_sent = {};
    log("device " + config.alias + " online: " + formatDeviceId(device_id) + (name.empty() ? "" : " \"" + name + "\"") +
        ", " + std::to_string(groups.subs()) + " subscription and " + std::to_string(groups.cmds()) + " command groups");
    record(defs::HistoryKind::Online, Bytes(device_id.begin(), device_id.end()));
    scheduleMaintenance();
    scheduleClock();
    if (*lost) { linkLost(); }
}

void Device::failed(const std::string &why)
{
    connecting = false;
    if (why != last_failure) { log("device " + config.alias + " unavailable: " + why); last_failure = why; }
    if (agent.stopping) { return; }
    retry.expires_after(agent.config.retry);
    retry.async_wait([this](auto ec) { if (!ec) { open(); } });
}

void Device::offline(const std::string &why)
{
    if (!online) { return; }
    online = false;
    ++link;
    log("device " + config.alias + " offline: " + why);
    maintenance.cancel();
    clock_timer.cancel();
    record(defs::HistoryKind::Offline, {});
    auto members = std::move(clients);
    clients.clear();
    for (const auto &c : members) { c->device = nullptr; c->close(static_cast<uint8_t>(defs::AgentCloseReason::DeviceOffline)); }
    if (session)
    {
        session->unobserve(observer);
        session->close();
    }
    session.reset();
    groups.reset(0, 0);
    ++map_generation;
    for (auto &set : clearing) { set.clear(); }
    // Every connection is closed: the lease ends with them.
    lease.reset();
    ++lease_serial;
    end.reset();
    backstop = false;
    take_waiter.reset();
    arm.reset();
    disarm_inflight = 0;
    disarm_next.clear(); // the agent's own waiters belong to this link's lease serial
    last_failure = why;
    failed(why);
}

void Device::frame(const Frame &f)
{
    if (f.type == 0) { linkLost(); return; }
    const bool booted = f.generation != generation;
    if (booted) { generation = f.generation; boot(); }
    if (f.type == msg::kStatusStream)
    {
        if (auto now = parseStatusArm(f.payload))
        {
            if (arm && arm->state != now->state)
            {
                record(defs::HistoryKind::Arm, Bytes{now->state, now->last_disarm_reason});
            }
            arm = now;
            if (now->armed() && lease.phase() != Lease::Phase::Held && !backstop)
            {
                // Armed with nobody holding the lease (an old holder's ARM
                // that landed late): disarm, one at a time. An ending lease
                // waits for it like for any extra DISARM.
                log("device " + config.alias + " is armed with no lease holder; disarming");
                backstop = true;
                end.redisarmSent();
                // Per link, not per lease end: the counter spans lease ends.
                disarmInternal([this, link_id = link] {
                    if (link != link_id) { return; }
                    backstop = false;
                    end.redisarmDone();
                    advanceEnd();
                });
            }
        }
    }
    else if (f.type == msg::kDiagEvent)
    {
        record(defs::HistoryKind::DiagEvent, Bytes(f.payload.begin(), f.payload.end()));
    }
    if (f.type == msg::kStatusStream || f.type == msg::kDiagEvent || f.type == msg::kCoreEvent)
    {
        const bool stream = f.type == msg::kStatusStream;
        const auto members = clients; // send() never closes inline, but stay safe
        for (const auto &c : members) { c->send(f.type, f.payload, stream); }
        if (booted && session)
        {
            // After the Boot event, so a client's own Boot reset does not
            // discard it: the Session's clock was reset with the Boot.
            const auto model = session->clockModel();
            broadcastEvent(defs::AgentEvent::ClockModel, encodeClockModel(model));
            clock_sent = model;
        }
        return;
    }
    if (f.type >= defs::kSubGroupChannelFirst && f.type < defs::kSubGroupChannelFirst + defs::kSubGroupChannelCount)
    {
        if (f.payload.size() < 2) { ++dropped; return; }
        const auto slot = static_cast<uint8_t>(f.type - defs::kSubGroupChannelFirst);
        auto owner = groups.route(slot, f.payload[1]);
        if (!owner) { ++dropped; return; }
        auto it = agent.clients.find(owner->first);
        if (it == agent.clients.end()) { ++dropped; return; }
        it->second->send(static_cast<uint8_t>(defs::kSubGroupChannelFirst + owner->second), f.payload, true);
    }
}

void Device::linkLost()
{
    // The lost notice skips the Session's ordered completion delivery, so a
    // reply already received (a REBOOT reply just before the link closes) may
    // still be queued behind it. Go offline through that same order: a
    // service call on the closed Session completes after every earlier one.
    if (!online || losing) { return; }
    losing = true;
    try
    {
        session->async<int>([] { return 0; }, [this, link_id = link](std::expected<int, Error>) {
            if (link == link_id) { offline("link lost"); }
        });
    }
    catch (const Error &)
    {
        offline("link lost");
    }
}

void Device::boot()
{
    // The device lost every group: all slots are free again.
    log("device " + config.alias + " rebooted");
    groups.reset(groups.subs(), groups.cmds());
    ++map_generation;
    for (auto &set : clearing) { set.clear(); }
    arm.reset();
    refreshIdentity();
    clock_sent = {}; // frame() sends the new model after the Boot event
}

void Device::refreshIdentity()
{
    if (!session) { return; }
    auto *raw = session.get();
    try
    {
        session->async<DeviceInfo>([raw] { return drivelink::Client(*raw).info(); },
            [this, link_id = link](std::expected<DeviceInfo, Error> info) {
                if (link != link_id || !online || !info) { return; }
                device_id = info->device_id;
                name = info->name;
            });
    }
    catch (const Error &) { /* busy: the next Boot or rename refreshes it */ }
}

void Device::scheduleMaintenance()
{
    maintenance.expires_after(agent.config.retry);
    maintenance.async_wait([this, link_id = link](auto ec) {
        if (ec || link != link_id || !online) { return; }
        cleanDirty();
        scheduleMaintenance();
    });
}

void Device::cleanDirty()
{
    if (!session) { return; }
    for (const auto kind : {Kind::Sub, Kind::Cmd})
    {
        // An ending lease clears command slots itself, after its DISARM.
        if (kind == Kind::Cmd && end.ending()) { continue; }
        const auto op = kind == Kind::Sub ? defs::GroupOp::SubClear : defs::GroupOp::CmdClear;
        auto &in_flight = clearing[static_cast<size_t>(kind)];
        for (auto slot : groups.dirty(kind))
        {
            if (in_flight.contains(slot)) { continue; }
            in_flight.insert(slot);
            try
            {
                session->asyncRequest(static_cast<uint8_t>(defs::Family::Group), static_cast<uint8_t>(op), Bytes{slot},
                    [this, slot, kind, gen = map_generation](std::expected<Reply, Error> reply) {
                        if (gen != map_generation) { return; }
                        clearing[static_cast<size_t>(kind)].erase(slot);
                        if (reply && accepted(reply->result)) { groups.cleaned(slot, kind); }
                    });
            }
            catch (const Error &) { in_flight.erase(slot); }
        }
    }
}

bool Device::commandSlotsLeft() const
{
    return !groups.dirty(Kind::Cmd).empty();
}

void Device::release(const std::shared_ptr<Connection> &c)
{
    clients.erase(c);
    groups.release(c->id);
    switch (lease.leave(c->id))
    {
    case Lease::Left::Holder:
        announceLease(0, {}, 0, c->wire_id, defs::LeaseChange::Disconnected);
        endLease(c->id);
        break;
    case Lease::Left::Pending: take_waiter.reset(); break;
    case Lease::Left::None: break;
    }
    if (online) { cleanDirty(); }
}

void Device::forwardArm(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t family, uint8_t op, Bytes body)
{
    const auto client_id = c->id;
    end.armSent();
    const bool sent = forwardCore(c, seq, family, op, std::move(body), [this, client_id] {
        if (!lease.mayCommand(client_id))
        {
            // The lease ended while this ARM was in flight: it may have landed
            // after the lease's DISARM. Disarm again; an ending lease waits
            // for that DISARM too.
            end.redisarmSent();
            disarmInternal([this, link_id = link] {
                if (link != link_id) { return; }
                end.redisarmDone();
                advanceEnd();
            });
        }
        end.armDone();
        advanceEnd();
    });
    if (!sent) { end.armDone(); }
}

bool Device::forwardCore(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t family, uint8_t op, Bytes body,
                         std::function<void()> after)
{
    if (c->inflight >= kClientInflight)
    {
        c->send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::None)));
        return false;
    }
    try
    {
        session->asyncRequest(family, op, std::move(body),
            [this, weak = std::weak_ptr<Connection>(c), seq, family, op, after = std::move(after), link_id = link](std::expected<Reply, Error> reply) {
                if (link != link_id) { return; }
                if (after) { after(); }
                if (reply && accepted(reply->result) && family == static_cast<uint8_t>(defs::Family::System) &&
                    (op == static_cast<uint8_t>(defs::SystemOp::SetName) || op == static_cast<uint8_t>(defs::SystemOp::Revert) ||
                     op == static_cast<uint8_t>(defs::SystemOp::FactoryReset)))
                {
                    refreshIdentity();
                }
                auto client = weak.lock();
                if (!client) { return; }
                --client->inflight;
                if (reply)
                {
                    client->send(msg::kCoreResponse, coreReply(seq, family, op, reply->result, reply->detail, reply->body));
                }
                else if (localFailure(reply.error()))
                {
                    client->send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::None)));
                }
                // A timeout stays unanswered: the client's own timeout applies.
            });
        ++c->inflight;
        return true;
    }
    catch (const Error &)
    {
        // The device link's queue is full: refuse without reaching the device.
        c->send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::None)));
        return false;
    }
}

void Device::disarm(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t family, uint8_t op)
{
    if (sendDisarm({Waiter{c, seq, family, op, {}}})) { return; }
    // Not admitted: wait for the DISARM in flight. One waiter per connection;
    // an earlier one from the same connection is answered Busy.
    for (auto &w : disarm_next)
    {
        if (w.client.lock() == c)
        {
            c->send(msg::kCoreResponse, coreReply(w.seq, w.family, w.op, u8(defs::Result::Busy), u8(defs::Detail::None)));
            w = Waiter{c, seq, family, op, {}};
            return;
        }
    }
    if (disarm_inflight == 0)
    {
        // Nothing in flight to wait for: the queue is full of other work.
        c->send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::None)));
        return;
    }
    disarm_next.push_back(Waiter{c, seq, family, op, {}});
}

void Device::disarmInternal(std::function<void()> done)
{
    Waiter own;
    own.done = std::move(done);
    if (sendDisarm({own})) { return; }
    if (disarm_inflight)
    {
        // The DISARM in flight may predate what this one must cover: send
        // another when it completes.
        disarm_next.push_back(std::move(own));
        return;
    }
    // The Session admits a DISARM whenever none is in flight; only a closed
    // link refuses one, and the device goes offline with it.
    log("device " + config.alias + ": the agent's DISARM was not admitted");
    asio::post(agent.io, [this, link_id = link, done = std::move(own.done)] {
        if (link == link_id) { done(); }
    });
}

bool Device::sendDisarm(std::vector<Waiter> waiters)
{
    auto answer = [](const std::vector<Waiter> &to, uint8_t result, uint8_t detail) {
        for (const auto &w : to)
        {
            if (w.done) { continue; }
            if (auto client = w.client.lock()) { client->send(msg::kCoreResponse, coreReply(w.seq, w.family, w.op, result, detail)); }
        }
    };
    auto finished = [](const std::vector<Waiter> &to) {
        for (const auto &w : to)
        {
            if (w.done) { w.done(); }
        }
    };
    try
    {
        // The Session's DISARM path: fast-path MSG_TYPE, its own admission
        // slot, ahead of queued requests.
        session->asyncRequest(static_cast<uint8_t>(defs::Family::System), static_cast<uint8_t>(defs::SystemOp::Disarm), {},
            [this, waiters, answer, finished, link_id = link](std::expected<Reply, Error> reply) {
                if (link != link_id) { return; }
                if (disarm_inflight) { --disarm_inflight; }
                if (reply) { answer(waiters, reply->result, reply->detail); }
                else if (localFailure(reply.error())) { answer(waiters, u8(defs::Result::Busy), u8(defs::Detail::None)); }
                if (!disarm_next.empty())
                {
                    auto next = std::exchange(disarm_next, {});
                    if (!sendDisarm(next))
                    {
                        answer(next, u8(defs::Result::Busy), u8(defs::Detail::None));
                        finished(next);
                    }
                }
                finished(waiters);
            });
        ++disarm_inflight;
        return true;
    }
    catch (const Error &)
    {
        return false;
    }
}

// ---------------------------------------------------------------- Lease ----

void Device::announceLease(uint16_t holder, const std::string &holder_name, uint8_t holder_kind, uint16_t previous,
                           defs::LeaseChange reason)
{
    const auto body =
        encodeLease({holder, clipped(holder_name, kMaxString), holder_kind, previous, static_cast<uint8_t>(reason)});
    record(defs::HistoryKind::Lease, body);
    broadcastEvent(defs::AgentEvent::Lease, body);
    log("device " + config.alias + ": lease " + (holder ? "held by client " + std::to_string(holder) : std::string("free")));
}

void Device::leaseTake(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op)
{
    auto answer = [&](defs::Result r) { c->sendAgent(seq, op, u8(r), u8(defs::Detail::None)); };
    const auto previous = lease.holder();
    switch (lease.take(c->id))
    {
    case Lease::Take::AlreadyHolder: answer(defs::Result::Ok); return;
    case Lease::Take::Busy: answer(defs::Result::Busy); return;
    case Lease::Take::Granted:
        answer(defs::Result::Ok);
        announceLease(c->wire_id, c->name, c->kind, 0, defs::LeaseChange::Taken);
        return;
    case Lease::Take::Revoked:
    {
        take_waiter = TakeWaiter{c, seq};
        const auto *old = agent.client(previous);
        announceLease(0, {}, 0, old ? old->wire_id : 0, defs::LeaseChange::Taken);
        endLease(previous);
        return;
    }
    case Lease::Take::Waiting: take_waiter = TakeWaiter{c, seq}; return;
    }
}

void Device::leaseRelease(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op)
{
    if (!lease.release(c->id))
    {
        c->sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::WrongState));
        return;
    }
    c->sendAgent(seq, op, u8(defs::Result::Ok), u8(defs::Detail::None));
    announceLease(0, {}, 0, c->wire_id, defs::LeaseChange::Released);
    endLease(c->id);
}

void Device::endLease(ClientId old)
{
    // The old holder's holder-only frames stopped at lease.take/release/leave.
    // Its command slots become Dirty: nobody else may bind them until cleared.
    groups.release(old, Kind::Cmd);
    const auto serial = ++lease_serial;
    end.start();
    disarmInternal([this, serial] {
        if (serial != lease_serial) { return; }
        end.disarmDone();
        advanceEnd();
    });
}

void Device::advanceEnd()
{
    if (lease.phase() != Lease::Phase::Ending || !end.ending()) { return; }
    if (end.readyToClear())
    {
        // CLEAR every Dirty command slot; one already being cleared (a CLEAR
        // from before this end) is waited for by ARM, not by the grant.
        const auto serial = lease_serial;
        unsigned sent = 0;
        auto &in_flight = clearing[static_cast<size_t>(Kind::Cmd)];
        for (auto slot : groups.dirty(Kind::Cmd))
        {
            if (in_flight.contains(slot)) { continue; }
            in_flight.insert(slot);
            try
            {
                session->asyncRequest(static_cast<uint8_t>(defs::Family::Group), static_cast<uint8_t>(defs::GroupOp::CmdClear), Bytes{slot},
                    [this, slot, serial, gen = map_generation, link_id = link](std::expected<Reply, Error> reply) {
                        if (link != link_id) { return; }
                        if (gen == map_generation)
                        {
                            clearing[static_cast<size_t>(Kind::Cmd)].erase(slot);
                            if (reply && accepted(reply->result)) { groups.cleaned(slot, Kind::Cmd); }
                        }
                        // A failed CLEAR leaves the slot Dirty: maintenance retries
                        // it and ARM waits for it. The handoff goes on.
                        if (serial != lease_serial) { return; }
                        end.clearDone();
                        advanceEnd();
                    });
                ++sent;
            }
            catch (const Error &) { in_flight.erase(slot); }
        }
        end.clearsStarted(sent);
    }
    if (end.readyToFinish())
    {
        end.finished();
        finishLease();
        cleanDirty(); // command slots left Dirty go to maintenance now
    }
}

void Device::finishLease()
{
    const auto granted = lease.finish();
    auto waiter = std::exchange(take_waiter, std::nullopt);
    if (!granted) { return; }
    auto c = waiter ? waiter->client.lock() : nullptr;
    if (!c || c->id != *granted)
    {
        // Cannot happen: leave() drops a waiting Take. Stay safe: nobody holds it.
        log("device " + config.alias + ": a granted lease has no client");
        lease.reset();
        return;
    }
    c->sendAgent(waiter->seq, static_cast<uint8_t>(defs::AgentOp::LeaseTake), u8(defs::Result::Ok), u8(defs::Detail::None));
    announceLease(c->wire_id, c->name, c->kind, 0, defs::LeaseChange::Taken);
}

// ------------------------------------------------- Clients, history, clock ----

void Device::record(defs::HistoryKind kind, Bytes body)
{
    history.push({static_cast<uint8_t>(kind), unixMs(), std::move(body)});
}

void Device::broadcastEvent(defs::AgentEvent event, std::span<const uint8_t> body)
{
    const auto payload = encodeEvent({static_cast<uint8_t>(event), Bytes(body.begin(), body.end())});
    const auto members = clients;
    for (const auto &c : members) { c->send(msg::kAgentEvent, payload); }
}

void Device::greet(const std::shared_ptr<Connection> &c)
{
    const auto entries = history.entries();
    for (const auto &chunk : encodeHistory(entries, kMaxEventBody))
    {
        c->send(msg::kAgentEvent, encodeEvent({static_cast<uint8_t>(defs::AgentEvent::History), chunk}));
    }
    const auto model = session ? session->clockModel() : ClockModel{};
    c->send(msg::kAgentEvent, encodeEvent({static_cast<uint8_t>(defs::AgentEvent::ClockModel), encodeClockModel(model)}));
}

void Device::clientList(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op)
{
    std::vector<ClientRecord> records;
    for (const auto &member : clients)
    {
        records.push_back({member->wire_id, clipped(member->name, kMaxString), member->kind, lease.holder() == member->id});
    }
    std::ranges::sort(records, {}, &ClientRecord::client_id);
    // 16 clients of at most 2 + 33 + 1 + 1 bytes fit one frame.
    c->sendAgent(seq, op, u8(defs::Result::Ok), u8(defs::Detail::None), encodeClientList(records));
}

void Device::scheduleClock()
{
    clock_timer.expires_after(kClockPeriod);
    clock_timer.async_wait([this, link_id = link](auto ec) {
        if (ec || link != link_id || !online || !session) { return; }
        const auto model = session->clockModel();
        if (model.offset_valid != clock_sent.offset_valid || model.drift_valid != clock_sent.drift_valid ||
            model.updated_us != clock_sent.updated_us)
        {
            broadcastEvent(defs::AgentEvent::ClockModel, encodeClockModel(model));
            clock_sent = model;
        }
        scheduleClock();
    });
}

void Device::forwardDiag(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op, Bytes body)
{
    constexpr uint8_t kDiagVersion = 3;
    if (c->inflight >= kClientInflight)
    {
        c->send(msg::kDiagQueryResponse, Bytes{kDiagVersion, seq, op, u8(defs::Result::Busy)});
        return;
    }
    try
    {
        session->asyncDiagRequest(op, std::move(body),
            [weak = std::weak_ptr<Connection>(c), seq, op, this, link_id = link](std::expected<Bytes, Error> reply) {
                if (link != link_id) { return; }
                auto client = weak.lock();
                if (!client) { return; }
                --client->inflight;
                Bytes out{kDiagVersion, seq, op};
                if (reply) { out.push_back(0); out.insert(out.end(), reply->begin(), reply->end()); }
                else if (reply.error().kind == ErrorKind::Rejected) { out.push_back(reply.error().result); }
                else if (localFailure(reply.error())) { out.push_back(u8(defs::Result::Busy)); }
                else { return; }
                client->send(msg::kDiagQueryResponse, out);
            });
        ++c->inflight;
    }
    catch (const Error &)
    {
        c->send(msg::kDiagQueryResponse, Bytes{kDiagVersion, seq, op, u8(defs::Result::Busy)});
    }
}

void Device::groupOp(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op, Bytes body)
{
    const auto family = static_cast<uint8_t>(defs::Family::Group);
    auto reply = [&](defs::Result r, defs::Detail d, Bytes b = {}) {
        c->send(msg::kCoreResponse, coreReply(seq, family, op, u8(r), u8(d), b));
    };
    const auto gop = static_cast<defs::GroupOp>(op);
    if (gop == defs::GroupOp::List)
    {
        if (body.size() != 2) { reply(defs::Result::InvalidArgument, defs::Detail::None); return; }
        listGroups(c, seq, static_cast<uint16_t>(body[0] | body[1] << 8));
        return;
    }
    if (body.empty()) { reply(defs::Result::InvalidArgument, defs::Detail::None); return; }
    if (c->inflight >= kClientInflight) { reply(defs::Result::Busy, defs::Detail::None); return; }
    const bool command = gop == defs::GroupOp::CmdConfig || gop == defs::GroupOp::CmdClear;
    const auto kind = command ? Kind::Cmd : Kind::Sub;
    const uint8_t channel_first = command ? defs::kCmdGroupChannelFirst : defs::kSubGroupChannelFirst;
    const uint8_t vid = body[0];
    if (vid >= groups.count(kind)) { reply(defs::Result::InvalidArgument, defs::Detail::Capacity); return; }
    std::optional<uint8_t> slot;
    if (gop == defs::GroupOp::SubConfig || gop == defs::GroupOp::CmdConfig)
    {
        slot = groups.acquire(c->id, vid, kind);
        if (!slot) { reply(defs::Result::InvalidArgument, defs::Detail::Capacity); return; }
    }
    else
    {
        slot = groups.find(c->id, vid, kind);
        if (!slot)
        {
            if (gop == defs::GroupOp::SubEnable) { reply(defs::Result::InvalidArgument, defs::Detail::UnknownRef); return; }
            // Clearing an id with no slot: Ok, as clearing an empty device slot is.
            reply(defs::Result::Ok, defs::Detail::None,
                  Bytes{groups.lastEpoch(c->id, vid, kind), static_cast<uint8_t>(channel_first + vid)});
            return;
        }
        groups.begin(*slot, kind);
    }
    body[0] = *slot;
    const bool clearing_op = gop == defs::GroupOp::SubClear || gop == defs::GroupOp::CmdClear;
    const auto serial = groups.slot(*slot, kind).serial;
    try
    {
        session->asyncRequest(family, op, std::move(body),
            [this, weak = std::weak_ptr<Connection>(c), seq, op, vid, s = *slot, serial, clearing_op, kind, channel_first,
             gen = map_generation, link_id = link](std::expected<Reply, Error> r) {
                if (link != link_id) { return; }
                const bool acc = r && accepted(r->result);
                const bool ok = acc && r->body.size() == 2;
                if (gen == map_generation)
                {
                    // A malformed accepted reply is uncertain, as an unanswered one is.
                    groups.finish(s, serial, ok, ok ? r->body[0] : 0, clearing_op && ok, !r || (acc && !ok), kind);
                }
                else if (acc && !clearing_op && session && r->generation == session->generation())
                {
                    // Accepted after a Boot reset the map: configured on the
                    // device but nobody's now. Clear it.
                    groups.markDirty(s, kind);
                    cleanDirty();
                }
                auto client = weak.lock();
                if (!client) { return; }
                --client->inflight;
                if (!r)
                {
                    if (localFailure(r.error())) { client->send(msg::kCoreResponse, coreReply(seq, static_cast<uint8_t>(defs::Family::Group), op, u8(defs::Result::Busy), u8(defs::Detail::None))); }
                    return;
                }
                Bytes b = r->body;
                if (ok) { b[1] = static_cast<uint8_t>(channel_first + vid); }
                client->send(msg::kCoreResponse, coreReply(seq, static_cast<uint8_t>(defs::Family::Group), op, r->result, r->detail, b));
            });
        ++c->inflight;
    }
    catch (const Error &)
    {
        groups.finish(*slot, serial, false, 0, false, false, kind);
        reply(defs::Result::Busy, defs::Detail::None);
    }
}

void Device::commandFrame(const std::shared_ptr<Connection> &c, uint8_t vid, std::span<const uint8_t> payload)
{
    // Only the holder's own, mapped command groups; the payload (and so the
    // epoch the client got from CMD_CONFIG) goes through unchanged.
    const auto slot = vid < groups.cmds() ? groups.find(c->id, vid, Kind::Cmd) : std::nullopt;
    if (!slot || !session) { ++c->dropped; return; }
    try
    {
        if (!session->sendCommandGroup(*slot, payload)) { ++c->dropped; }
    }
    catch (const Error &) { ++c->dropped; } // a malformed payload, or the link just closed
}

namespace
{
// Reads every device LIST page, then answers from the client's view.
struct ListJob : std::enable_shared_from_this<ListJob>
{
    Device &device;
    std::weak_ptr<Connection> client;
    uint8_t seq;
    uint16_t start;
    uint64_t link;
    std::vector<GroupEntry> entries;
    size_t pages = 0;
    uint16_t cursor = 0;
    ListJob(Device &d, std::weak_ptr<Connection> c, uint8_t s, uint16_t st, uint64_t l)
        : device(d), client(std::move(c)), seq(s), start(st), link(l)
    {
        if (auto conn = client.lock()) { ++conn->inflight; }
    }
    ~ListJob()
    {
        if (auto conn = client.lock()) { --conn->inflight; }
    }
    static constexpr uint8_t family = static_cast<uint8_t>(defs::Family::Group);
    static constexpr uint8_t op = static_cast<uint8_t>(defs::GroupOp::List);
    void answer(uint8_t result, uint8_t detail, std::span<const uint8_t> body = {})
    {
        if (auto c = client.lock()) { c->send(msg::kCoreResponse, coreReply(seq, family, op, result, detail, body)); }
    }
    void next(uint16_t at)
    {
        if (device.link != link || !device.session) { return; }
        cursor = at;
        try
        {
            device.session->asyncRequest(family, op, Bytes{static_cast<uint8_t>(cursor), static_cast<uint8_t>(cursor >> 8)},
                [self = shared_from_this()](std::expected<Reply, Error> r) { self->page(std::move(r)); });
        }
        catch (const Error &)
        {
            answer(u8(defs::Result::Busy), u8(defs::Detail::None));
        }
    }
    void page(std::expected<Reply, Error> r)
    {
        if (device.link != link) { return; }
        if (!r)
        {
            // A timeout stays unanswered; a local failure is answered Busy.
            if (localFailure(r.error())) { answer(u8(defs::Result::Busy), u8(defs::Detail::None)); }
            return;
        }
        if (!accepted(r->result)) { answer(r->result, r->detail); return; }
        auto parsed = parseListPage(r->body);
        // A malformed or non-advancing device listing is left unanswered.
        if (!parsed || ++pages > kMaxListPages || (parsed->next != kListEnd && parsed->next <= cursor) ||
            (parsed->entries.empty() && parsed->next != kListEnd))
        {
            log("device " + device.config.alias + ": malformed group LIST");
            return;
        }
        for (auto &e : parsed->entries) { entries.push_back(std::move(e)); }
        if (parsed->next != kListEnd) { next(parsed->next); return; }
        auto c = client.lock();
        if (!c) { return; }
        auto result = synthesizeList(device.groups, c->id, entries, start, std::min<size_t>(device.max_payload, p::kMaxPayload) - 6);
        if (!result) { answer(u8(defs::Result::InvalidArgument), u8(defs::Detail::None)); return; }
        if (result->too_small) { answer(u8(defs::Result::InvalidArgument), u8(defs::Detail::BufferTooSmall)); return; }
        answer(u8(defs::Result::Ok), u8(defs::Detail::None), result->body);
    }
};
} // namespace

void Device::listGroups(const std::shared_ptr<Connection> &c, uint8_t seq, uint16_t start)
{
    if (c->inflight >= kClientInflight)
    {
        c->send(msg::kCoreResponse, coreReply(seq, static_cast<uint8_t>(defs::Family::Group),
                                              static_cast<uint8_t>(defs::GroupOp::List), u8(defs::Result::Busy), u8(defs::Detail::None)));
        return;
    }
    std::make_shared<ListJob>(*this, c, seq, start, link)->next(0);
}

// ------------------------------------------------------------------ Agent ----

Agent::Impl::~Impl()
{
    for (auto &d : devices)
    {
        if (d->connector.joinable()) { d->connector.join(); }
        if (d->session) { d->session->close(); }
    }
}

size_t Agent::Impl::helloed() const
{
    size_t n = 0;
    for (const auto &[id, c] : clients) { n += c->state == Connection::State::Lobby || c->state == Connection::State::Device; }
    return n;
}

Device *Agent::Impl::findDevice(const std::string &name)
{
    for (auto &d : devices)
    {
        if (d->config.alias == name) { return d.get(); }
    }
    std::array<uint8_t, 6> id{};
    if (!parseDeviceId(name, id) || id == std::array<uint8_t, 6>{}) { return nullptr; }
    for (auto &d : devices)
    {
        if (d->device_id == id) { return d.get(); }
    }
    return nullptr;
}

DeviceRecord Agent::Impl::record(const Device &d) const
{
    DeviceRecord r;
    r.alias = d.config.alias;
    r.online = d.online;
    r.device_id = d.device_id;
    r.name = d.name.size() <= kMaxString ? d.name : d.name.substr(0, kMaxString);
    r.endpoint = d.config.endpoint.size() <= kMaxString ? d.config.endpoint : d.config.endpoint.substr(0, kMaxString);
    r.clients = static_cast<uint8_t>(std::min<size_t>(d.clients.size(), 255));
    if (const auto *holder = client(d.lease.holder()))
    {
        r.holder_id = holder->wire_id;
        r.holder_name = clipped(holder->name, kMaxString);
        r.holder_kind = holder->kind;
    }
    return r;
}

Connection *Agent::Impl::client(uint64_t id) const
{
    if (!id) { return nullptr; }
    auto it = clients.find(id);
    return it == clients.end() ? nullptr : it->second.get();
}

uint16_t Agent::Impl::allocWireId()
{
    // At most kMaxConnections are open, so a free id is always found.
    for (;;)
    {
        if (++next_wire == 0) { next_wire = 1; }
        const bool used = std::ranges::any_of(clients, [this](const auto &entry) { return entry.second->wire_id == next_wire; });
        if (!used) { return next_wire; }
    }
}

void Agent::Impl::accept()
{
    acceptor.async_accept([this](auto ec, asio::ip::tcp::socket socket) {
        if (stopping) { return; }
        if (!ec)
        {
            if (clients.size() >= kMaxConnections)
            {
                asio::error_code ignored;
                socket.close(ignored);
            }
            else
            {
                asio::error_code ignored;
                socket.set_option(asio::ip::tcp::no_delay(true), ignored);
                const auto id = next_client++;
                auto client = std::make_shared<Connection>(*this, std::move(socket), id);
                client->wire_id = allocWireId();
                clients.emplace(id, client);
                client->start();
            }
        }
        else if (ec != asio::error::operation_aborted)
        {
            // A persistent error (no file descriptors left) must not spin.
            log("accept: " + ec.message());
            auto timer = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(200));
            timer->async_wait([this, timer](auto) { if (!stopping) { accept(); } });
            return;
        }
        else { return; }
        accept();
    });
}

void Agent::Impl::shutdown()
{
    if (stopping) { return; }
    stopping = true;
    log("shutting down");
    if (advertiser) { advertiser->stop(); } // goodbyes first: clients find out at once
    asio::error_code ignored;
    acceptor.close(ignored);
    signals.cancel(ignored);
    auto all = clients;
    for (auto &[id, c] : all) { c->close(static_cast<uint8_t>(defs::AgentCloseReason::Shutdown)); }
    for (auto &d : devices)
    {
        d->retry.cancel();
        d->maintenance.cancel();
        if (d->online && d->session)
        {
            // Best effort: nothing should stay armed once its clients are gone.
            try { d->session->request(static_cast<uint8_t>(defs::Family::System), static_cast<uint8_t>(defs::SystemOp::Disarm)); }
            catch (const std::exception &e) { log("device " + d->config.alias + ": DISARM at shutdown failed: " + e.what()); }
        }
    }
    // Let Closing events flush, then stop.
    auto timer = std::make_shared<asio::steady_timer>(io, std::chrono::milliseconds(200));
    timer->async_wait([this, timer](auto) {
        for (auto &[id, c] : std::map(clients)) { c->finishClose(); }
        work.reset();
        io.stop();
    });
}

Agent::Agent(AgentConfig config) : m(std::make_unique<Impl>(std::move(config))) {}
Agent::~Agent() = default;

uint16_t Agent::start()
{
    auto &d = *m;
    asio::error_code ec;
    const auto address = asio::ip::make_address(d.config.bind, ec);
    if (ec) { throw std::runtime_error("bind address: " + ec.message()); }
    const asio::ip::tcp::endpoint endpoint(address, d.config.port);
    d.acceptor.open(endpoint.protocol(), ec);
    if (!ec) { d.acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec); }
    if (!ec) { d.acceptor.bind(endpoint, ec); }
    if (!ec) { d.acceptor.listen(asio::socket_base::max_listen_connections, ec); }
    if (ec) { throw std::runtime_error("listen on " + d.config.bind + ":" + std::to_string(d.config.port) + ": " + ec.message()); }
    if (!d.config.loopback()) { log("listening on " + d.config.bind + ", not only loopback: every client needs the token"); }
    for (const auto &config : d.config.devices) { d.devices.push_back(std::make_unique<Device>(d, config)); }
    return d.acceptor.local_endpoint().port();
}

void Agent::run()
{
    auto &d = *m;
    d.work.emplace(asio::make_work_guard(d.io));
    d.signals.add(SIGINT);
    d.signals.add(SIGTERM);
    d.signals.async_wait([&d](auto ec, int) { if (!ec) { d.shutdown(); } });
    d.accept();
    for (auto &device : d.devices) { device->open(); }
    if (d.config.advertise && !d.config.loopback())
    {
        char hostname[256] = {};
        ::gethostname(hostname, sizeof(hostname) - 1);
        AdvertiseSettings settings{d.config.name.empty() ? defaultInstance(hostname) : d.config.name,
                                   discovery::hostLabel(hostname), d.config.bind, d.acceptor.local_endpoint().port(),
                                   agent::kVersion, DRIVELINK_AGENT_VERSION};
        d.advertiser = std::make_unique<Advertiser>(std::move(settings), [](const std::string &line) { log(line); });
        // Advertising is best effort: the agent serves TCP without it.
        try { d.advertiser->start(); }
        catch (const std::exception &e) { log(std::string("mDNS: not advertising: ") + e.what()); d.advertiser.reset(); }
    }
    d.io.run();
}

void Agent::stop()
{
    asio::post(m->io, [impl = m.get()] { impl->shutdown(); });
}
} // namespace drivelink::agent
