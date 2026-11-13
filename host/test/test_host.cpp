#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core_os/native_file.hpp"
#include "core_os/native_kv.hpp"
#include "diag_protocol.hpp"
#include "doctest.h"
#include "drivelink/client.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include <asio.hpp>
#include <bit>
#include <filesystem>
#include <future>
#include <thread>

using namespace drivelink;
namespace
{
using Socket = asio::ip::tcp::socket;
Bytes readFrame(Socket &socket)
{
    std::array<uint8_t, 7> header{};
    asio::read(socket, asio::buffer(header));
    const auto n = static_cast<size_t>(header[4]) + (static_cast<size_t>(header[5]) << 8);
    if (n > 1024)
    {
        throw std::runtime_error("bad test request");
    }
    Bytes body(n + 2);
    asio::read(socket, asio::buffer(body));
    body.resize(n);
    return body;
}
void sendFrame(Socket &socket, uint8_t type, const Bytes &payload)
{
    Bytes bytes(protocol::frameSize(payload.size()));
    protocol::encodeFrame(bytes.data(), bytes.size(), type, payload.data(), payload.size());
    asio::write(socket, asio::buffer(bytes));
}
void answer(Socket &socket, const Bytes &request, const Bytes &body = {})
{
    Bytes data{3, request[1], request[2], request[3], 0, 0};
    data.insert(data.end(), body.begin(), body.end());
    sendFrame(socket, 0x41, data);
}
struct FakeDevice
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    std::exception_ptr error;
    std::thread worker;
    explicit FakeDevice(std::function<void(Socket &)> run)
    {
        worker = std::thread(
            [this, run]
            {
                try
                {
                    Socket socket(io);
                    acceptor.accept(socket);
                    socket.set_option(asio::ip::tcp::no_delay(true));
                    run(socket);
                }
                catch (...)
                {
                    error = std::current_exception();
                }
            });
    }
    std::string endpoint() const
    {
        return "tcp://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
    }
    ~FakeDevice()
    {
        worker.join();
    }
};
} // namespace

using namespace drivelink;
TEST_CASE("typed values reject malformed inputs instead of narrowing")
{
    Json d = {{"value_type", 1}};
    CHECK(parseValue(d, "255") == 255);
    for (auto s : {"256", "-1", "1junk", "", "0x"})
    {
        CHECK_THROWS_AS(parseValue(d, s), Error);
    }
    d["value_type"] = 5;
    CHECK(parseValue(d, "0.5") == std::bit_cast<uint32_t>(0.5f));
    for (auto s : {"NaN", "inf", "1e100", "2x"})
    {
        CHECK_THROWS_AS(parseValue(d, s), Error);
    }
    d["value_type"] = 0;
    CHECK(parseValue(d, "true") == 1);
    CHECK_THROWS_AS(parseValue(d, "2"), Error);
    d["value_type"] = 4;
    CHECK(valueJson(4, parseValue(d, "-2147483648")) == -2147483648LL);
    CHECK_THROWS_AS(parseValue(d, "2147483648"), Error);
    d["value_type"] = 6;
    Json enums = Json::array({{{"name", "on"}, {"value", 1}}});
    CHECK(parseValue(d, "on", enums) == 1);
    CHECK_THROWS_AS(parseValue(d, "0", enums), Error);
    CHECK_THROWS_AS(valueJson(5, 0x7fc00000), Error);
}
TEST_CASE("refs and blobs have explicit bounds")
{
    auto r = parseRef("0x70:2:768");
    CHECK(r.type == 112);
    CHECK(r.instance == 2);
    CHECK(r.item == 768);
    for (auto s : {"256:0:1", "1:255:1", "1:0:65536", "1:0", "1:0:1:2"})
    {
        CHECK_THROWS_AS(parseRef(s), Error);
    }
    CHECK(hex(parseHex("00ffAA")) == "00ffaa");
    CHECK_THROWS_AS(parseHex("a"), Error);
    CHECK_THROWS_AS(parseHex("zz"), Error);
}
TEST_CASE("durable blobs survive reopening and an interrupted write")
{
    const auto dir =
        std::filesystem::temp_directory_path() /
        std::filesystem::path("drivelink-host-test-" +
                              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    core_os::KvStore a;
    REQUIRE(core_os::native::openFileStore(a, dir) == 0);
    uint8_t original = 42;
    REQUIRE(a.set("test", "p_a", &original, 1) == 0);
    core_os::native::failNextWrite(a, -1);
    uint8_t changed = 99;
    CHECK(a.set("test", "p_a", &changed, 1) != 0);
    core_os::KvStore b;
    REQUIRE(core_os::native::openFileStore(b, dir) == 0);
    uint8_t result = 0;
    size_t size = 1;
    REQUIRE(b.get("test", "p_a", &result, size) == 0);
    CHECK(result == 42);
    core_os::native::clearFaults(a);
    core_os::native::tearNextWrite(a);
    const std::array<uint8_t, 4> full{1, 2, 3, 4};
    REQUIRE(a.set("test", "p_b", full.data(), full.size()) == 0);
    core_os::KvStore torn;
    REQUIRE(core_os::native::openFileStore(torn, dir) == 0);
    std::array<uint8_t, 4> shortened{};
    size = shortened.size();
    REQUIRE(torn.get("test", "p_b", shortened.data(), size) == 0);
    CHECK(size == 2);
    CHECK(shortened[0] == 1);
    CHECK(shortened[1] == 2);
    CHECK(b.set("../bad", "x", &result, 1) != 0);
    CHECK(torn.eraseNamespace("test") == 0);
    core_os::KvStore c;
    REQUIRE(core_os::native::openFileStore(c, dir) == 0);
    CHECK(c.get("test", "p_a", &result, size) != 0);
    CHECK(c.get("test", "p_b", &result, size) != 0);
    std::filesystem::remove_all(dir);
}
TEST_CASE("session matches family and seq through wrap and bounds its event queue")
{
    FakeDevice device(
        [](Socket &socket)
        {
            for (unsigned i = 0; i < 300; ++i)
            {
                auto request = readFrame(socket);
                Bytes wrong = request;
                wrong[2] = 2;
                answer(socket, wrong, {99});
                sendFrame(socket, 128, {1});
                sendFrame(socket, 128, {2});
                sendFrame(socket, 128, {3});
                answer(socket, request, {static_cast<uint8_t>(i)});
            }
        });
    SessionOptions opts;
    opts.event_capacity = 2;
    opts.read_retries = 0;
    Session session(device.endpoint(), opts);
    for (unsigned i = 0; i < 300; ++i)
    {
        auto reply = session.request(3, 1);
        REQUIRE(reply.body.size() == 1);
        CHECK(reply.body[0] == static_cast<uint8_t>(i));
    }
    CHECK(session.droppedEvents() >= 898);
    Frame frame;
    unsigned n = 0;
    while (session.nextEvent(frame, std::chrono::milliseconds(0)))
    {
        ++n;
    }
    CHECK(n == 2);
}
TEST_CASE("read retries retire timed out seqs, writes do not retry, close cancels a pending call")
{
    std::promise<void> pending;
    FakeDevice device(
        [&](Socket &socket)
        {
            auto first = readFrame(socket);
            auto retry = readFrame(socket);
            if (first[1] == retry[1])
            {
                throw std::runtime_error("timed out seq reused");
            }
            answer(socket, first, {99});
            answer(socket, retry, {42});
            auto write = readFrame(socket); // deliberately never acknowledged
            auto read = readFrame(socket);
            if (read[2] != 3 || write[2] != 4 || read[1] == write[1])
            {
                throw std::runtime_error("write retried or ID reused");
            }
            pending.set_value();
            std::array<uint8_t, 1> ignored{};
            asio::error_code ec;
            socket.read_some(asio::buffer(ignored), ec);
        });
    SessionOptions opts;
    opts.timeout = std::chrono::milliseconds(100);
    opts.read_retries = 1;
    Session session(device.endpoint(), opts);
    CHECK(session.request(3, 1).body == Bytes{42});
    try
    {
        session.request(4, 2);
        FAIL("write should time out");
    }
    catch (const Error &e)
    {
        CHECK(e.kind == ErrorKind::Timeout);
    }
    auto future = std::async(std::launch::async,
                             [&]
                             {
                                 try
                                 {
                                     session.request(3, 1);
                                     return false;
                                 }
                                 catch (const Error &e)
                                 {
                                     return e.kind == ErrorKind::Connection;
                                 }
                             });
    REQUIRE(pending.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    session.close();
    session.close();
    CHECK(future.get());
}
TEST_CASE("client rejects truncated replies, nonprogress pages and malformed streams")
{
    FakeDevice device(
        [](Socket &socket)
        {
            answer(socket, readFrame(socket), {1});
            answer(socket, readFrame(socket), {1, 0, 0, 0, 0});
        });
    Session session(device.endpoint());
    Client client(session);
    CHECK_THROWS_AS(client.info(), Error);
    CHECK_THROWS_AS(client.types(), Error);
    CHECK_THROWS_AS(client.event({128, Bytes(25, 0)}), Error);
    CHECK_THROWS_AS(client.event({192, {3, 0, 0, 2, 0, 0}}), Error);
}
TEST_CASE("protocol mismatch is an explicit connection error rather than a timeout")
{
    FakeDevice device(
        [](Socket &socket)
        {
            auto request = readFrame(socket);
            sendFrame(socket, 0x41, {1, request[1], 1, 1, 0, 0});
        });
    Session session(device.endpoint());
    try
    {
        session.request(1, 1);
        FAIL("version mismatch accepted");
    }
    catch (const Error &e)
    {
        CHECK(e.kind == ErrorKind::Protocol);
    }
}
TEST_CASE("diagnostic pages cannot end before their advertised total")
{
    FakeDevice device(
        [](Socket &socket)
        {
            auto request = readFrame(socket);
            sendFrame(socket, 0x63, {3, request[1], 2, 0, 1, 255, 0});
        });
    Session session(device.endpoint());
    Client client(session);
    CHECK_THROWS_AS(client.diagEntries(), Error);
}
TEST_CASE("diagnostic event decoding preserves instance identity and exact lengths")
{
    FakeDevice device([](Socket &) {});
    Session session(device.endpoint());
    Client client(session);
    std::array<uint8_t, 1024> buffer{};
    diag::protocol::RecordEvent report{123456789, 2, 112, 7, 42, -1, 99, 101};
    auto n = diag::protocol::encodeRecordEvent(65535, 0, report, buffer.data(), buffer.size());
    Frame frame{192, Bytes(buffer.begin(), buffer.begin() + n)};
    auto decoded = client.event(frame);
    CHECK(decoded["instance"] == 7);
    CHECK(decoded["code"] == 42);
    CHECK(decoded["error"] == -1);
    CHECK(decoded["timestamp_us"] == 123456789);
    CHECK(decoded["sequence"] == 65535);
    frame.payload.push_back(0);
    CHECK_THROWS_AS(client.event(frame), Error);
    diag::protocol::CounterEntry entry{112, 42, 3, 9, 7};
    n = diag::protocol::encodeCounterDeltaEvent(1, 0, 123, &entry, 1, buffer.data(), buffer.size());
    decoded = client.event({192, Bytes(buffer.begin(), buffer.begin() + n)});
    CHECK(decoded["entries"][0]["instance"] == 7);
    CHECK(decoded["entries"][0]["delta"] == 3);
    diag::protocol::BootEvent boot{1, 0, 3, 1, 1};
    n = diag::protocol::encodeBootEvent(2, 0, boot, buffer.data(), buffer.size());
    CHECK(client.event({192, Bytes(buffer.begin(), buffer.begin() + n)})["safe_mode"] == 1);
    diag::protocol::ClearedEvent clear{22, 112, 7, 42};
    n = diag::protocol::encodeClearedEvent(3, 0, clear, buffer.data(), buffer.size());
    CHECK(client.event({192, Bytes(buffer.begin(), buffer.begin() + n)})["instance"] == 7);
}

TEST_CASE("status v3 carries sparse loop ids and rejects duplicate ids and bad counts")
{
    FakeDevice device([](Socket &socket) {
        std::array<uint8_t,1> byte{};asio::error_code ec;
        socket.read_some(asio::buffer(byte),ec);
    });
    Session session(device.endpoint()); Client client(session);
    Bytes payload(18,0); payload[0]=3; payload[17]=2;
    payload.insert(payload.end(), {3,1,0,0,0, 19,9,0,0,0, 0});
    auto status=client.event({128,payload});
    CHECK(status["overruns"][0]["loop_id"]==3);
    CHECK(status["overruns"][1]["loop_id"]==19);
    CHECK(status["overruns"][1]["overruns"]==9);
    payload[23]=3;CHECK_THROWS_AS(client.event({128,payload}),Error);
    payload[23]=19;payload[17]=9;CHECK_THROWS_AS(client.event({128,payload}),Error);
    payload[17]=2;payload.push_back(0);CHECK_THROWS_AS(client.event({128,payload}),Error);
}

TEST_CASE("GET_INFO v3 validates sparse ids, loop flags and duplicate loop identities")
{
    auto body=[](bool duplicate, bool bad_flags) {
        Bytes bytes(256);protocol::wire::Writer w(bytes.data(),bytes.size());
        w.appendU8(3);protocol::core_wire::string(w,"test");protocol::core_wire::string(w,"build");
        w.appendU32Le(1);w.appendU32Le(0);w.appendU16Le(1024);w.appendU8(2);w.appendU8(0);w.appendU8(2);
        w.appendU8(3);protocol::core_wire::string(w,"first");w.appendU32Le(1000);w.appendU8(1);
        w.appendU8(duplicate?3:19);protocol::core_wire::string(w,"second");w.appendU32Le(100);w.appendU8(bad_flags?2:0);
        bytes.resize(w.size());return bytes;
    };
    FakeDevice device([&](Socket &socket) {
        answer(socket,readFrame(socket),body(false,false));
        answer(socket,readFrame(socket),body(true,false));
        answer(socket,readFrame(socket),body(false,true));
    });
    Session session(device.endpoint());Client client(session);
    const auto info=client.info();CHECK(info["loops"][0]["realtime"]==true);
    CHECK(info["loops"][1]["id"]==19);CHECK(info["loops"][1]["realtime"]==false);
    CHECK_THROWS_AS(client.info(),Error);CHECK_THROWS_AS(client.info(),Error);
}

TEST_CASE("command payloads and subscription decoding preserve types and reject malformed streams")
{
    const std::array<uint32_t,2> values{std::bit_cast<uint32_t>(1.5F), 42};
    const auto payload=commandPayload(255,65535,values);
    CHECK(payload==Bytes{3,255,255,255,0,0,192,63,42,0,0,0});
    CHECK(commandPayload(1,0,{}).size()==4);
    std::array<uint32_t,65> too_many{};CHECK_THROWS_AS(commandPayload(1,0,too_many),Error);
    Json group={{"channel",129},{"id",0},{"epoch",255},{"kind","subscription"},{"configured",true},
        {"refs",Json::array({Json::object(),Json::object()})}};
    Json descriptors=Json::array({{{"value_type",5}},{{"value_type",3}}});
    Frame frame{129,Bytes{3,255,255,255,1,0,0,0,0,0,0,0,0,0,192,63,42,0,0,0}};
    const auto decoded=decodeSubscription(frame,group,descriptors);
    CHECK(decoded["counter"]==65535);CHECK(decoded["timestamp_us"]==1);
    CHECK(decoded["typed_values"]==Json::array({1.5,42}));
    frame.payload[1]=254;CHECK(decodeSubscription(frame,group,descriptors).is_null());
    frame.payload[1]=255;frame.type=130;CHECK(decodeSubscription(frame,group,descriptors).is_null());frame.type=129;
    for(size_t n=0;n<20;++n) {
        Frame truncated{129,Bytes(frame.payload.begin(),frame.payload.begin()+n)};
        CHECK_THROWS_AS(decodeSubscription(truncated,group,descriptors),Error);
    }
    frame.payload.push_back(0);CHECK_THROWS_AS(decodeSubscription(frame,group,descriptors),Error);
    frame.payload.pop_back();frame.payload[0]=2;CHECK_THROWS_AS(decodeSubscription(frame,group,descriptors),Error);
}

TEST_CASE("host groups validate pages and mutations and send stream frames on the same session")
{
    FakeDevice device([](Socket &socket) {
        // Two pages, one slot each; a configured subscription then empty command.
        answer(socket,readFrame(socket),{1,1,64,2,0,1,0,1, 0,0,3,19,7,2,0,1,112,0,0,1});
        answer(socket,readFrame(socket),{1,1,64,2,0,255,255,1, 1,0,0,255,0,1,0,0});
        answer(socket,readFrame(socket),{8,129});
        answer(socket,readFrame(socket),{9,130}); // wrong channel for group zero
        const auto command=readFrame(socket);
        if(command!=Bytes{3,8,99,0,42,0,0,0}) { throw std::runtime_error("bad command payload"); }
        answer(socket,readFrame(socket)); // drain barrier
        // A non-progressing page must fail rather than loop forever.
        answer(socket,readFrame(socket),{1,1,64,2,0,0,0,0});
    });
    Session session(device.endpoint());Client client(session);
    const auto listing=client.groups();CHECK(listing["groups"].size()==2);
    CHECK(listing["groups"][0]["loop"]==19);CHECK(listing["groups"][0]["refs"][0]["item"]==256);
    CHECK(client.subEnable(0,true)["epoch"]==8);
    CHECK_THROWS_AS(client.subEnable(0,false),Error);
    const std::array<uint32_t,1> values{42};REQUIRE(client.command(0,8,99,values));
    client.system(5);
    CHECK_THROWS_AS(client.groups(),Error);
}
