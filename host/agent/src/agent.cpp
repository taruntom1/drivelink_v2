#include "agent.hpp"
#include "groups.hpp"
#include "drivelink/agent.hpp"
#include "drivelink/client.hpp"
#include "drivelink/session.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/gen/registry.hpp"
#include "drivelink/protocol/scanner.hpp"
#include <asio.hpp>
#include <deque>
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
constexpr size_t kMaxConnections = 128;     // including those still before Hello
constexpr size_t kStreamQueueLimit = 256 * 1024; // drop stream frames above this
constexpr size_t kQueueLimit = 4 * 1024 * 1024;  // close a client that falls this far behind
constexpr uint8_t kCoreVersion = p::core_wire::kVersion;

uint8_t u8(defs::Result r) { return static_cast<uint8_t>(r); }
uint8_t u8(defs::Detail d) { return static_cast<uint8_t>(d); }
bool accepted(uint8_t result) { return result == u8(defs::Result::Ok) || result == u8(defs::Result::PendingReboot); }

void log(const std::string &line) { std::cerr << "drivelink-agent: " << line << std::endl; }

Bytes frame(uint8_t type, std::span<const uint8_t> payload)
{
    Bytes out(p::frameSize(payload.size()));
    p::encodeFrame(out.data(), out.size(), type, payload.data(), payload.size());
    return out;
}
Bytes coreReply(uint8_t seq, uint8_t family, uint8_t op, uint8_t result, uint8_t detail, std::span<const uint8_t> body = {})
{
    Bytes out{kCoreVersion, seq, family, op, result, detail};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
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
    bool stopping = false;

    explicit Impl(AgentConfig c) : config(std::move(c)) {}
    ~Impl();
    void accept();
    size_t helloed() const;
    void shutdown();
    agent::DeviceRecord record(const Device &d) const;
    Device *findDevice(const std::string &name);
};

struct Connection : std::enable_shared_from_this<Connection>
{
    enum class State { AwaitHello, Lobby, Device, Closed };
    Agent::Impl &agent;
    asio::ip::tcp::socket socket;
    uint64_t id;
    std::string name;
    State state = State::AwaitHello;
    struct Device *device = nullptr;
    p::Scanner scanner{p::kMaxPayload};
    std::array<uint8_t, 4096> input{};
    std::deque<std::shared_ptr<Bytes>> out;
    size_t out_bytes = 0;
    bool writing = false, close_after_write = false;
    asio::steady_timer hello_timer;
    uint64_t dropped = 0;

    Connection(Agent::Impl &a, asio::ip::tcp::socket s, uint64_t i)
        : agent(a), socket(std::move(s)), id(i), hello_timer(a.io) {}
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
    bool online = false, connecting = false;
    uint64_t link = 0;         // bumps on every online/offline transition
    uint64_t generation = 0;   // the Session's boot generation last seen
    uint64_t map_generation = 0; // bumps on every GroupMap reset
    std::array<uint8_t, 6> device_id{};
    std::string name;
    uint16_t max_payload = p::kMaxPayload;
    GroupMap groups;
    std::set<uint8_t> clearing;
    std::set<std::shared_ptr<Connection>> clients;
    asio::steady_timer retry, maintenance;
    std::thread connector;
    std::string last_failure;
    uint64_t dropped = 0;

    Device(Agent::Impl &a, DeviceConfig c) : agent(a), config(std::move(c)), retry(a.io), maintenance(a.io) {}
    ~Device() { if (connector.joinable()) { connector.join(); } }
    void open();
    void connected(std::shared_ptr<Session> s, DeviceInfo info, GroupList list);
    void failed(const std::string &why);
    void offline(const std::string &why);
    void frame(const Frame &f);
    void boot();
    void refreshIdentity();
    void cleanDirty();
    void scheduleMaintenance();
    // Forwarding: completions run on the agent thread.
    void forwardCore(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t family, uint8_t op, Bytes body);
    void forwardDiag(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op, Bytes body);
    void groupOp(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op, Bytes body);
    void listGroups(const std::shared_ptr<Connection> &c, uint8_t seq, uint16_t start);
    void release(const std::shared_ptr<Connection> &c);
};

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
        log("client " + std::to_string(id) + " fell behind; closing it");
        finishClose();
        return;
    }
    auto bytes = std::make_shared<Bytes>(frame(type, payload));
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
    if (!writing) { finishClose(); }
}

void Connection::finishClose()
{
    if (state == State::Closed) { return; }
    const bool had_device = state == State::Device;
    state = State::Closed;
    hello_timer.cancel();
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
    acceptedHello.client_id = static_cast<uint16_t>(id);
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
        state = State::Device;
        device = d;
        d->clients.insert(shared_from_this());
        acceptedHello.lobby = false;
        acceptedHello.device = agent.record(*d);
    }
    name = request->client_name;
    hello_timer.cancel();
    sendAgent(seq, op, u8(defs::Result::Ok), u8(defs::Detail::None), encodeHelloAccepted(acceptedHello));
    log("client " + std::to_string(id) + " (" + (name.empty() ? "unnamed" : name) + ") " +
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
    switch (static_cast<defs::AgentOp>(op))
    {
    case defs::AgentOp::Hello:
        sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::WrongState));
        return;
    case defs::AgentOp::ListDevices:
    {
        if (f.len != 3) { sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::None)); return; }
        std::vector<DeviceRecord> records;
        for (const auto &d : agent.devices) { records.push_back(agent.record(*d)); }
        sendAgent(seq, op, u8(defs::Result::Ok), u8(defs::Detail::None), encodeDeviceList(records));
        return;
    }
    default: sendAgent(seq, op, u8(defs::Result::InvalidArgument), u8(defs::Detail::Unsupported)); return;
    }
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
            d->forwardCore(self, seq, family, op, {});
            return;
        case Route::Malformed:
            send(msg::kCoreResponse, coreReply(seq, 0xFF, 0xFF, u8(defs::Result::InvalidArgument), u8(defs::Detail::None)));
            return;
        case Route::RefuseLease:
            send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::LeaseRequired)));
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
    default: ++dropped; return; // time pings, command-group frames (interim rule), other types
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
            drivelink::Client client(*session);
            auto info = client.info(); // checks the core version
            client.disarm();
            auto list = client.groups();
            for (const auto &g : list.groups)
            {
                if (g.configured) { client.groupClear(g.id, g.kind == GroupKind::Command); }
            }
            list = client.groups();
            asio::post(*io, [this, session, info = std::move(info), list = std::move(list)]() mutable {
                connected(std::move(session), std::move(info), std::move(list));
            });
        }
        catch (const std::exception &e)
        {
            asio::post(*io, [this, why = std::string(e.what())] { failed(why); });
        }
    });
}

void Device::connected(std::shared_ptr<Session> s, DeviceInfo info, GroupList list)
{
    connecting = false;
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
    clearing.clear();
    observer = session->observe([this, link_id = link](const Frame &f) {
        if (link == link_id && online) { frame(f); }
    });
    log("device " + config.alias + " online: " + formatDeviceId(device_id) + (name.empty() ? "" : " \"" + name + "\"") +
        ", " + std::to_string(groups.subs()) + " subscription groups");
    scheduleMaintenance();
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
    clearing.clear();
    last_failure = why;
    failed(why);
}

void Device::frame(const Frame &f)
{
    if (f.type == 0) { offline("link lost"); return; }
    if (f.generation != generation) { generation = f.generation; boot(); }
    if (f.type == msg::kStatusStream || f.type == msg::kDiagEvent || f.type == msg::kCoreEvent)
    {
        const bool stream = f.type == msg::kStatusStream;
        for (const auto &c : clients) { c->send(f.type, f.payload, stream); }
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

void Device::boot()
{
    // The device lost every group: all slots are free again.
    log("device " + config.alias + " rebooted");
    groups.reset(groups.subs(), groups.cmds());
    ++map_generation;
    clearing.clear();
    refreshIdentity();
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
    for (auto slot : groups.dirty())
    {
        if (clearing.contains(slot)) { continue; }
        clearing.insert(slot);
        try
        {
            session->asyncRequest(static_cast<uint8_t>(defs::Family::Group), static_cast<uint8_t>(defs::GroupOp::SubClear), Bytes{slot},
                [this, slot, gen = map_generation](std::expected<Reply, Error> reply) {
                    if (gen != map_generation) { return; }
                    clearing.erase(slot);
                    if (reply && accepted(reply->result)) { groups.cleaned(slot); }
                });
        }
        catch (const Error &) { clearing.erase(slot); }
    }
}

void Device::release(const std::shared_ptr<Connection> &c)
{
    clients.erase(c);
    groups.release(c->id);
    if (online) { cleanDirty(); }
}

void Device::forwardCore(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t family, uint8_t op, Bytes body)
{
    try
    {
        session->asyncRequest(family, op, std::move(body),
            [this, weak = std::weak_ptr<Connection>(c), seq, family, op, link_id = link](std::expected<Reply, Error> reply) {
                if (link != link_id) { return; }
                if (reply && accepted(reply->result) && family == static_cast<uint8_t>(defs::Family::System) &&
                    (op == static_cast<uint8_t>(defs::SystemOp::SetName) || op == static_cast<uint8_t>(defs::SystemOp::Revert) ||
                     op == static_cast<uint8_t>(defs::SystemOp::FactoryReset)))
                {
                    refreshIdentity();
                }
                auto client = weak.lock();
                if (!client || !reply) { return; } // unanswered: the client's own timeout applies
                client->send(msg::kCoreResponse, coreReply(seq, family, op, reply->result, reply->detail, reply->body));
            });
    }
    catch (const Error &)
    {
        // The device link's queue is full: refuse without reaching the device.
        c->send(msg::kCoreResponse, coreReply(seq, family, op, u8(defs::Result::Busy), u8(defs::Detail::None)));
    }
}

void Device::forwardDiag(const std::shared_ptr<Connection> &c, uint8_t seq, uint8_t op, Bytes body)
{
    constexpr uint8_t kDiagVersion = 3;
    try
    {
        session->asyncDiagRequest(op, std::move(body),
            [weak = std::weak_ptr<Connection>(c), seq, op, this, link_id = link](std::expected<Bytes, Error> reply) {
                if (link != link_id) { return; }
                auto client = weak.lock();
                if (!client) { return; }
                Bytes out{kDiagVersion, seq, op};
                if (reply) { out.push_back(0); out.insert(out.end(), reply->begin(), reply->end()); }
                else if (reply.error().kind == ErrorKind::Rejected) { out.push_back(reply.error().result); }
                else { return; }
                client->send(msg::kDiagQueryResponse, out);
            });
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
    const uint8_t vid = body[0];
    if (vid >= groups.subs()) { reply(defs::Result::InvalidArgument, defs::Detail::Capacity); return; }
    std::optional<uint8_t> slot;
    if (gop == defs::GroupOp::SubConfig)
    {
        slot = groups.acquire(c->id, vid);
        if (!slot) { reply(defs::Result::InvalidArgument, defs::Detail::Capacity); return; }
    }
    else
    {
        slot = groups.find(c->id, vid);
        if (!slot)
        {
            if (gop == defs::GroupOp::SubEnable) { reply(defs::Result::InvalidArgument, defs::Detail::UnknownRef); return; }
            // Clearing an id with no slot: Ok, as clearing an empty device slot is.
            reply(defs::Result::Ok, defs::Detail::None,
                  Bytes{groups.lastEpoch(c->id, vid), static_cast<uint8_t>(defs::kSubGroupChannelFirst + vid)});
            return;
        }
        groups.begin(*slot);
    }
    body[0] = *slot;
    const bool clearing_op = gop == defs::GroupOp::SubClear;
    try
    {
        session->asyncRequest(family, op, std::move(body),
            [this, weak = std::weak_ptr<Connection>(c), seq, op, vid, s = *slot, clearing_op, gen = map_generation](std::expected<Reply, Error> r) {
                if (gen == map_generation)
                {
                    const bool ok = r && accepted(r->result) && r->body.size() == 2;
                    groups.finish(s, ok, ok ? r->body[0] : 0, clearing_op && ok, !r);
                }
                auto client = weak.lock();
                if (!client || !r) { return; }
                Bytes b = r->body;
                if (accepted(r->result) && b.size() == 2) { b[1] = static_cast<uint8_t>(defs::kSubGroupChannelFirst + vid); }
                client->send(msg::kCoreResponse, coreReply(seq, static_cast<uint8_t>(defs::Family::Group), op, r->result, r->detail, b));
            });
    }
    catch (const Error &)
    {
        groups.finish(*slot, false, 0, false, false);
        reply(defs::Result::Busy, defs::Detail::None);
    }
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
    ListJob(Device &d, std::weak_ptr<Connection> c, uint8_t s, uint16_t st, uint64_t l)
        : device(d), client(std::move(c)), seq(s), start(st), link(l) {}
    static constexpr uint8_t family = static_cast<uint8_t>(defs::Family::Group);
    static constexpr uint8_t op = static_cast<uint8_t>(defs::GroupOp::List);
    void answer(uint8_t result, uint8_t detail, std::span<const uint8_t> body = {})
    {
        if (auto c = client.lock()) { c->send(msg::kCoreResponse, coreReply(seq, family, op, result, detail, body)); }
    }
    void next(uint16_t cursor)
    {
        if (device.link != link || !device.session) { return; }
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
        if (device.link != link || !r) { return; } // unanswered: the client's timeout applies
        if (!accepted(r->result)) { answer(r->result, r->detail); return; }
        auto parsed = parseListPage(r->body);
        if (!parsed || (parsed->entries.empty() && parsed->next != kListEnd)) { return; }
        for (auto &e : parsed->entries) { entries.push_back(std::move(e)); }
        if (parsed->next != kListEnd) { next(parsed->next); return; }
        auto c = client.lock();
        if (!c) { return; }
        auto result = synthesizeList(device.groups, c->id, entries, start, static_cast<size_t>(device.max_payload) - 6);
        if (!result) { answer(u8(defs::Result::InvalidArgument), u8(defs::Detail::None)); return; }
        if (result->too_small) { answer(u8(defs::Result::InvalidArgument), u8(defs::Detail::BufferTooSmall)); return; }
        answer(u8(defs::Result::Ok), u8(defs::Detail::None), result->body);
    }
};
} // namespace

void Device::listGroups(const std::shared_ptr<Connection> &c, uint8_t seq, uint16_t start)
{
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
    return r;
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
                clients.emplace(id, client);
                client->start();
            }
        }
        accept();
    });
}

void Agent::Impl::shutdown()
{
    if (stopping) { return; }
    stopping = true;
    log("shutting down");
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
    d.io.run();
}

void Agent::stop()
{
    asio::post(m->io, [impl = m.get()] { impl->shutdown(); });
}
} // namespace drivelink::agent
