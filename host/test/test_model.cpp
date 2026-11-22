#include "doctest.h"
#include "test_options.hpp"
#include "test_dispatcher.hpp"
#include "drivelink/model.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/framing.hpp"
#include <asio.hpp>
#include <atomic>
#include <thread>
#include <chrono>

namespace
{
using namespace drivelink;
namespace p = drivelink::protocol;
using Socket = asio::ip::tcp::socket;
enum class Mode { Normal, Version, TypeDuplicate, ItemDuplicate, EnumDuplicate,
                  InstanceDuplicate, UnknownType, MissingSystem, InvalidEnum, Refusal, Truncated };

void u16(Bytes &b, uint16_t n) { b.push_back(uint8_t(n)); b.push_back(uint8_t(n >> 8)); }
void u32(Bytes &b, uint32_t n) { u16(b, uint16_t(n)); u16(b, uint16_t(n >> 16)); }
void string(Bytes &b, const std::string &s) { u16(b, uint16_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
uint16_t at16(const Bytes &b, size_t i) { return uint16_t(b.at(i) | (uint16_t(b.at(i + 1)) << 8)); }

Bytes item(uint16_t id, ValueType type, ItemKind kind = ItemKind::Param)
{
    p::defs::ItemDesc desc{};
    desc.id = id; desc.type = type; desc.kind = kind;
    desc.name = "discovered"; desc.unit = "u"; desc.description = "d"; desc.ref = "";
    desc.flags = 0x0180; // source and tune (core v10 u16 flags) survive without special module code
    Bytes out(128);
    p::wire::Writer w(out.data(), out.size()); p::core_wire::item(w, desc);
    out.resize(w.size()); return out;
}
Bytes page(const std::vector<Bytes> &records, uint16_t cursor)
{
    Bytes out; u16(out, uint16_t(records.size()));
    const bool have = cursor < records.size();
    u16(out, have && cursor + 1U < records.size() ? uint16_t(cursor + 1) : 65535);
    out.push_back(have ? 1 : 0);
    if (have) { out.insert(out.end(), records[cursor].begin(), records[cursor].end()); }
    return out;
}
Bytes instance(uint8_t type, uint8_t id, const char *label)
{
    Bytes out{type, id, 255, 4}; string(out, label); return out;
}

// Deliberately unknown module/type and item IDs. A negotiated small payload
// forces both descriptor paging and several parameter batches per instance.
struct ModelPeer
{
    asio::io_context io;
    asio::ip::tcp::acceptor listener{io, {asio::ip::address_v4::loopback(), 0}};
    std::atomic<Mode> mode{Mode::Normal};
    std::atomic<uint32_t> revision{17};
    std::atomic<size_t> largest_batch{0};
    std::atomic<int> inject{0};
    std::atomic<bool> close_after_diag{false};
    uint16_t cap;
    std::thread worker;
    std::exception_ptr error;

    explicit ModelPeer(uint16_t payload = 80) : cap(payload), worker([this] { run(); }) {}
    ~ModelPeer() { worker.join(); CHECK(error == nullptr); }
    std::string endpoint() const { return "tcp://127.0.0.1:" + std::to_string(listener.local_endpoint().port()); }

    Bytes respond(const Bytes &req)
    {
        const auto current = mode.load();
        const auto family = req.at(2), op = req.at(3);
        Bytes body;
        if (family == 1 && op == 1)
        {
            body.push_back(current == Mode::Version ? 99 : p::core_wire::kVersion);
            string(body, "test"); string(body, std::to_string(revision.load()));
            u32(body, 0x12345678); u32(body, 4096); u16(body, cap);
            body.insert(body.end(), {0, 8, 0, 2, 0x44, 0x4c, 0, 0, 1});
            const auto name=std::to_string(revision.load());body.push_back(static_cast<uint8_t>(name.size()));
            body.insert(body.end(),name.begin(),name.end()); // safe mode, no running loops
        }
        else if (family == 3)
        {
            std::vector<Bytes> records;
            if (op == 1)
            {
                for (uint8_t type : {uint8_t(0), uint8_t(223)})
                {
                    if (type == 0 && current == Mode::MissingSystem) { continue; }
                    Bytes entry{type, 3}; string(entry, type == 0 ? "system" : "unknown.module"); records.push_back(entry);
                }
                if (current == Mode::TypeDuplicate) { records.push_back(records.back()); }
            }
            else if (op == 2)
            {
                const auto type = req.at(4);
                if (type == 0) { records.push_back(item(1234, ValueType::U16)); }
                else
                {
                    for (uint16_t id = 100; id < 170; ++id) { records.push_back(item(id, ValueType::U32)); }
                    records.push_back(item(900, ValueType::Enum));
                    records.push_back(item(901, ValueType::F32, ItemKind::Signal));
                    records.push_back(item(902, ValueType::F32, ItemKind::Input));
                    records.push_back(item(903, ValueType::None, ItemKind::Action));
                }
                if (current == Mode::ItemDuplicate) { records.push_back(records.back()); }
            }
            else if (op == 3)
            {
                Bytes entry{42}; string(entry, "custom_mode"); records.push_back(entry);
                if (current == Mode::EnumDuplicate) { records.push_back(entry); }
            }
            else if (op == 4)
            {
                Bytes entry{223}; u16(entry, 4321); entry.insert(entry.end(), {2, 1, 0}); string(entry, "custom.code");
                records.push_back(entry);
            }
            else { throw std::runtime_error("unexpected describe op"); }
            body = page(records, at16(req, req.size() - 2));
        }
        else if (family == 2 && op == 1)
        {
            std::vector<Bytes> records{instance(223, 2, "two"), instance(223, 19, "nineteen")};
            if (req.at(4)) { records = {instance(223, 19, "pending"), instance(223, 31, "new")}; }
            if (current == Mode::InstanceDuplicate) { records.push_back(records.back()); }
            if (current == Mode::UnknownType) { records.push_back(instance(222, 7, "unknown")); }
            body = page(records, at16(req, 5));
        }
        else if (family == 4 && (op == 1 || op == 3))
        {
            const auto count = req.at(4);
            largest_batch.store(std::max(largest_batch.load(), size_t(count)));
            CHECK(count <= 64); CHECK(req.size() <= cap); CHECK(7U + 4U * count <= cap);
            body.push_back(count);
            for (size_t i = 0; i < count; ++i)
            {
                const auto type = req.at(5 + i * 4), id = req.at(6 + i * 4);
                const auto item_id = at16(req, 7 + i * 4);
                if (type == 0)
                {
                    if (current == Mode::Refusal)
                    {
                        return {p::core_wire::kVersion, req[1], family, op, 1, 3};
                    }
                    if (current == Mode::Truncated) { break; }
                    u32(body, 456);
                }
                else if (item_id == 900) { u32(body, current == Mode::InvalidEnum ? 9 : 42); }
                else { u32(body, revision.load() + id + item_id + (op == 3 ? 1000 : 0)); }
            }
        }
        else { throw std::runtime_error("snapshot attempted a mutation or unexpected request"); }
        Bytes response{p::core_wire::kVersion, req[1], family, op, 0, 0};
        response.insert(response.end(), body.begin(), body.end());
        CHECK(response.size() <= cap);
        return response;
    }

    void run()
    {
        try
        {
            Socket socket(io); listener.accept(socket); socket.set_option(asio::ip::tcp::no_delay(true));
            while (true)
            {
                std::array<uint8_t, 7> header{}; asio::error_code ec;
                asio::read(socket, asio::buffer(header), ec);
                if (ec == asio::error::eof || ec == asio::error::connection_reset) { return; }
                if (ec) { throw asio::system_error(ec); }
                const size_t n = size_t(header[4]) + (size_t(header[5]) << 8);
                if (n > 1024) { throw std::runtime_error("bad request"); }
                Bytes req(n + 2); asio::read(socket, asio::buffer(req)); req.resize(n);
                const auto injection=inject.exchange(0);
                if(injection) {
                    Bytes event(injection==1?19:17);p::wire::Writer w(event.data(),event.size());
                    if(injection==1) {
                        w.appendU8(p::core_wire::kVersion);w.appendU8(0);w.appendU16Le(1);w.appendU64Le(100);
                        w.appendU8(1);w.appendU8(12);w.appendU8(0);w.appendU8(1);w.appendU8(2);w.appendU8(0);w.appendU8(0);
                    } else {
                        w.appendU8(3);w.appendU16Le(0);w.appendU8(4);w.appendU8(0);w.appendU64Le(1);
                        w.appendU8(0);w.appendU8(0);w.appendU8(0);w.appendU8(0);
                    }
                    Bytes frame(p::frameSize(event.size()));p::encodeFrame(frame.data(),frame.size(),injection==1?0x80:0xc0,event.data(),event.size());
                    asio::write(socket,asio::buffer(frame));
                }
                const auto out = header[3]==p::defs::msg::kDiagQuery?Bytes{3,req[1],2,0,0,255,0}:respond(req);
                Bytes frame(p::frameSize(out.size()));
                p::encodeFrame(frame.data(), frame.size(), header[3]==p::defs::msg::kDiagQuery?p::defs::msg::kDiagQueryResponse:p::defs::msg::kCoreResponse, out.data(), out.size());
                asio::write(socket, asio::buffer(frame));
                if(header[3]==p::defs::msg::kDiagQuery && close_after_diag.exchange(false)) {return;}
            }
        }
        catch (...) { error = std::current_exception(); }
    }
};
} // namespace

TEST_CASE("snapshot discovers unknown types, sparse IDs, typed values and safe mode")
{
    ModelPeer peer;
    Session session(peer.endpoint(),testOptions()); Client client(session); DeviceModel model(client);
    const auto &s = model.snapshot();
    CHECK(s.info.name == "17"); CHECK(hex(s.info.device_id) == "02444c000001");
    CHECK(s.safeMode()); CHECK(s.info.loops.empty()); CHECK(s.info.descriptor_hash == 0x12345678);
    CHECK(s.types.at(223).descriptor.name == "unknown.module");
    CHECK(s.types.at(223).items.at(900).enums.at(0).name == "custom_mode");
    CHECK(s.types.at(223).items.at(901).descriptor.isSource());
    CHECK(s.types.at(223).items.at(901).descriptor.flags == 0x0180);
    CHECK(s.types.at(223).items.at(903).descriptor.kind == ItemKind::Action);
    CHECK(s.codes.at(0).code == 4321);
    CHECK(std::get<uint16_t>(s.system_params.at(1234)) == 456);
    CHECK(s.configured.size() == 2); CHECK(s.pending.size() == 2);
    CHECK(s.configured.at({223, 2}).params.size() == 71);
    CHECK(std::get<uint32_t>(s.configured.at({223, 2}).params.at(169)) == 188);
    CHECK(s.pending.at({223, 19}).instance.label == "pending");
    CHECK(std::get<EnumValue>(s.pending.at({223, 31}).params.at(900)).value == 42);
    CHECK(peer.largest_batch == 18);
    const auto copy = s;
    peer.revision = 100;
    model.reload();
    CHECK(model.snapshot().info.build_id == "100");
    CHECK(model.snapshot().info.name == "100");
    CHECK(std::get<uint32_t>(model.snapshot().pending.at({223, 31}).params.at(169)) == 1300);
    CHECK(copy.info.build_id == "17");
    CHECK(std::get<uint32_t>(copy.configured.at({223, 2}).params.at(169)) == 188);
}

TEST_CASE("snapshot parameter batches also respect the 64-reference wire maximum")
{
    ModelPeer peer(1024); Session session(peer.endpoint(),testOptions()); Client client(session); DeviceModel model(client);
    CHECK(model.snapshot().configured.at({223, 19}).params.size() == 71);
    CHECK(peer.largest_batch == 64);
}

TEST_CASE("snapshot rejects malformed discovery and preserves the last complete reload")
{
    ModelPeer peer; Session session(peer.endpoint(),testOptions()); Client client(session); DeviceModel model(client);
    const auto original = model.snapshotShared();
    for (const auto mode : {Mode::Version, Mode::TypeDuplicate, Mode::ItemDuplicate, Mode::EnumDuplicate,
                           Mode::InstanceDuplicate, Mode::UnknownType, Mode::MissingSystem,
                           Mode::InvalidEnum, Mode::Refusal, Mode::Truncated})
    {
        CAPTURE(int(mode)); peer.mode = mode; peer.revision = 200;
        CHECK_THROWS_AS(model.reload(), Error);
        CHECK(model.snapshotShared() == original);
        CHECK(model.snapshot().info.build_id == "17");
        CHECK(model.snapshot().pending.at({223, 31}).params.size() == 71);
    }
    peer.mode = Mode::Normal; model.reload();
    CHECK(model.snapshot().info.build_id == "200");
    session.close(); CHECK_THROWS_AS(model.reload(), Error);
    CHECK(model.snapshot().info.build_id == "200");
}
TEST_CASE("model follows status flags and automatically replaces its snapshot after Boot") {
    ModelPeer peer;Session session(peer.endpoint(),testOptions());Client client(session);DeviceModel model(client);
    std::atomic<unsigned> status=0,arm=0;
    model.onChange([&](auto change) {if(change==DeviceModel::Change::Status) {++status;}if(change==DeviceModel::Change::ArmState) {++arm;}});
    peer.inject=1;(void)client.info();
    const auto wait=[&](auto predicate) {
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!predicate() && std::chrono::steady_clock::now()<until) {std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        return predicate();
    };
    REQUIRE(wait([&]{return status>0 && arm>0;}));
    auto before=model.snapshotShared();REQUIRE(before->status);CHECK(before->status->arm_state==1);
    CHECK((before->info.flags&1)!=0);CHECK_FALSE(before->safeMode());
    peer.revision=501;peer.inject=2;try {(void)client.info();}catch(const Error &) {}
    REQUIRE(wait([&]{auto s=model.snapshotShared();return s->generation!=before->generation && !s->reloading && s->info.build_id=="501";}));
    CHECK(before->info.build_id=="17");
    session.close();REQUIRE(wait([&]{return !model.snapshotShared()->connected;}));
    model.onChange({});
}
TEST_CASE("a delayed successful Boot reload cannot resurrect a disconnected model") {
    using namespace drivelink_test;
    ModelPeer peer;auto gate=std::make_shared<CopyGate>();auto entered=gate->entered.get_future();
    auto options=testOptions();options.dispatcher=GatedDispatcher(gate);
    Session session(peer.endpoint(),options);OpenGateOnExit cleanup{gate};Client client(session);DeviceModel model(client);
    std::promise<std::thread::id> worker;auto worker_id=worker.get_future();
    session.async<std::thread::id>([]{return std::this_thread::get_id();},[&](auto r){REQUIRE(r);worker.set_value(*r);});
    REQUIRE(gate->output.run());gate->arm(worker_id.get());
    peer.revision=777;peer.close_after_diag=true;peer.inject=2;
    try {(void)client.info();}catch(const Error &) {}
    REQUIRE(gate->output.run()); // Boot starts the reload on the service executor.
    REQUIRE(entered.wait_for(std::chrono::seconds(3))==std::future_status::ready);
    while(model.snapshotShared()->connected) {REQUIRE(gate->output.run());}
    CHECK_FALSE(model.snapshotShared()->ready);
    gate->open();
    while(!model.snapshotShared()->ready) {REQUIRE(gate->output.run());}
    const auto snapshot=model.snapshotShared();CHECK(snapshot->info.build_id=="777");CHECK_FALSE(snapshot->connected);
    session.close();
}
TEST_CASE("configuration refresh rereads layouts and params, keeping descriptors and identity") {
    ModelPeer peer;Session session(peer.endpoint(),testOptions());Client client(session);DeviceModel model(client);
    const auto before=model.snapshotShared();
    std::atomic<unsigned> models=0;
    model.onChange([&](auto change) {if(change==DeviceModel::Change::Model) {++models;}});
    const auto refresh=[&] {
        std::promise<std::expected<OperationResult,Error>> done;auto result=done.get_future();
        model.refreshConfigAsync([&](auto r) {done.set_value(std::move(r));});
        REQUIRE(result.wait_for(std::chrono::seconds(3))==std::future_status::ready);
        return result.get();
    };
    peer.revision=100;
    REQUIRE(refresh());
    const auto after=model.snapshotShared();
    CHECK(models==1);
    CHECK(std::get<uint32_t>(after->pending.at({223,31}).params.at(169))==1300);
    CHECK(after->info.build_id=="17"); // GET_INFO and descriptors were not reread
    CHECK(after->info.name=="17");
    CHECK(after->types.at(223).items.at(900).enums.at(0).name=="custom_mode");
    CHECK(after->generation==before->generation);
    CHECK(std::get<uint32_t>(before->pending.at({223,31}).params.at(169))!=1300);
    // A failed read publishes nothing and keeps the last snapshot.
    peer.mode=Mode::Refusal;peer.revision=300;
    CHECK_FALSE(refresh());
    CHECK(model.snapshotShared()==after);
    peer.mode=Mode::Normal;session.close();
    CHECK_FALSE(refresh());
    CHECK(model.snapshotShared()->info.build_id=="17");
    model.onChange({});
}
