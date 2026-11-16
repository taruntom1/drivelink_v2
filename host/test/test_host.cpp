#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core_os/native_file.hpp"
#include "core_os/native_kv.hpp"
#include "diag_protocol.hpp"
#include "doctest.h"
#include "test_options.hpp"
#include "drivelink/client.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/time_codec.hpp"
#include <asio.hpp>
#include <bit>
#include <filesystem>
#include <future>
#include <expected>
#include <limits>
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
    Bytes data{protocol::core_wire::kVersion, request[1], request[2], request[3], 0, 0};
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
TEST_CASE("device refusal metadata survives value-based error propagation") {
    FakeDevice device([](Socket &socket) {
        const auto request=readFrame(socket);
        sendFrame(socket,0x41,{protocol::core_wire::kVersion,request[1],request[2],request[3],6,8});
    });
    Session session(device.endpoint(),testOptions());Client client(session);
    try { client.system(5);FAIL("accepted refusal"); }
    catch(const DeviceError &e) {
        CHECK(e.kind==ErrorKind::Rejected);CHECK(e.result==6);CHECK(e.detail==8);
        CHECK(e.has_request_context);CHECK(e.family==1);CHECK(e.op==5);CHECK_FALSE(e.outcome_unknown);
        std::expected<void,Error> completion(std::unexpect,e);
        CHECK(completion.error().result==6);CHECK(completion.error().detail==8);
        CHECK(completion.error().family==1);CHECK(completion.error().op==5);
    }
}
TEST_CASE("an incompatible peer reports its wire version") {
    FakeDevice device([](Socket &socket) {
        const auto request=readFrame(socket);
        sendFrame(socket,0x41,{uint8_t(protocol::core_wire::kVersion-1),request[1],request[2],request[3],0,0});
    });
    SessionOptions options=testOptions();options.read_retries=0;
    Session session(device.endpoint(),options);Client client(session);
    try { client.info();FAIL("accepted an incompatible reply"); }
    catch(const Error &e) {
        CHECK(e.kind==ErrorKind::Protocol);CHECK(e.peer_version==protocol::core_wire::kVersion-1);
        CHECK(std::string(e.what())=="incompatible protocol version or truncated response header");
    }
}
TEST_CASE("submitted mutation failures report uncertainty while reads and preflight do not") {
    struct Request { uint8_t family,op;bool mutation; };
    for(const auto request:{Request{1,1,false},Request{1,5,true},Request{7,3,false}}) {
        FakeDevice device([](Socket &socket) {
            readFrame(socket);
            // Keep the peer alive until Session teardown. A fixed sleep races
            // timeout delivery on a descheduled runner and can produce EOF first.
            std::array<uint8_t,1> byte{};asio::error_code error;
            CHECK(socket.read_some(asio::buffer(byte),error)==0);
            CHECK((error==asio::error::eof || error==asio::error::connection_reset));
        });
        SessionOptions options=testOptions();options.timeout=std::chrono::milliseconds(40);options.read_retries=0;
        Session session(device.endpoint(),options);
        try { session.request(request.family,request.op);FAIL("missing timeout"); }
        catch(const Error &e) {
            CHECK(e.kind==ErrorKind::Timeout);CHECK(e.has_request_context);CHECK(e.family==request.family);
            CHECK(e.op==request.op);CHECK(e.outcome_unknown==request.mutation);
        }
    }
    SUBCASE("oversized request is local") {
        std::promise<size_t> received;
        FakeDevice device([&](Socket &socket) {
            std::array<uint8_t,1> byte{};asio::error_code error;
            received.set_value(socket.read_some(asio::buffer(byte),error));
        });
        Session session(device.endpoint(),testOptions());session.setMaxPayload(16);
        try { session.request(1,5,Bytes(17));FAIL("accepted oversized mutation"); }
        catch(const Error &e) { CHECK(e.kind==ErrorKind::Usage);CHECK_FALSE(e.outcome_unknown);CHECK(e.op==5); }
        session.close();CHECK(received.get_future().get()==0);
    }
    SUBCASE("peer closes after mutation") {
        FakeDevice device([](Socket &socket) { readFrame(socket); });
        Session session(device.endpoint(),testOptions());
        try { session.request(1,5);FAIL("missing disconnect"); }
        catch(const Error &e) { CHECK(e.kind==ErrorKind::Connection);CHECK(e.outcome_unknown);CHECK(e.op==5); }
    }
    SUBCASE("closed session never submits") {
        FakeDevice device([](Socket &socket) { std::array<uint8_t,1> b{};asio::error_code ec;socket.read_some(asio::buffer(b),ec); });
        Session session(device.endpoint(),testOptions());session.close();
        try { session.request(1,5);FAIL("accepted closed session"); }
        catch(const Error &e) { CHECK(e.kind==ErrorKind::Connection);CHECK_FALSE(e.outcome_unknown); }
    }
}
TEST_CASE("scalar variants preserve distinct types and exact bits") {
    for(auto pair:{std::pair{ValueType::Bool,1U}, {ValueType::U8,255U}, {ValueType::U16,65535U},
                  {ValueType::U32,UINT32_MAX}, {ValueType::I32,0x80000000U},
                  {ValueType::F32,std::bit_cast<uint32_t>(-0.0F)}, {ValueType::Enum,255U}}) {
        CHECK(valueBits(decodeValue(pair.first,pair.second))==pair.second);
    }
    CHECK(std::holds_alternative<EnumValue>(decodeValue(ValueType::Enum,1)));
    CHECK(std::holds_alternative<uint8_t>(decodeValue(ValueType::U8,1)));
    for(auto pair:{std::pair{ValueType::Bool,2U},{ValueType::U8,256U},{ValueType::U16,65536U},
                  {ValueType::Enum,256U},{ValueType::None,0U},{ValueType::F32,0x7f800000U}}) {
        CHECK_THROWS_AS(decodeValue(pair.first,pair.second),Error);
    }
    CHECK_THROWS_AS(valueBits(Value(std::numeric_limits<float>::infinity())),Error);
}
TEST_CASE("malformed successful service replies keep request context and mutation uncertainty") {
    struct Case { uint8_t family,op;bool mutation; };
    const std::array cases{Case{1,1,false},Case{4,2,true},Case{8,1,true},Case{5,2,true},
                           Case{7,1,true},Case{7,3,false},Case{9,1,true}};
    for(const auto &test:cases) {
        FakeDevice device([&](Socket &socket) {
            const auto req=readFrame(socket);
            answer(socket,req,Bytes{test.family==1?protocol::core_wire::kVersion:uint8_t{1}});
        });
        Session session(device.endpoint(),testOptions());Client client(session);
        try {
            const std::array<Ref,1> refs{{{0,0,4}}};const std::array<uint32_t,1> values{200};
            switch(test.family) {
            case 1: client.info();break;
            case 4: client.set(refs,values);break;
            case 5: client.subEnable(0,true);break;
            case 7: if(test.op==1) { client.testgenStart({}); }else { client.testgenStatus(0); }break;
            case 8: client.action({0,0,1});break;
            case 9: client.otaBegin(512,{});break;
            }
            FAIL("accepted malformed service result");
        } catch(const Error &e) {
            CHECK(e.kind==ErrorKind::Protocol);CHECK(e.has_request_context);
            CHECK(e.family==test.family);CHECK(e.op==test.op);CHECK(e.outcome_unknown==test.mutation);
        }
    }
}
TEST_CASE("time pings correlate u16 IDs, reject stale replies and timestamp on I/O") {
    FakeDevice device([](Socket &socket) {
        namespace tw=protocol::time_wire;
        auto req=readFrame(socket);uint16_t id=0;REQUIRE(tw::decodeRequest(req.data(),req.size(),id));
        std::this_thread::sleep_for(std::chrono::milliseconds(80)); // first ping times out
        std::array<uint8_t,19> p{};tw::response(p.data(),p.size(),{id,10,20});
        sendFrame(socket,protocol::defs::msg::kTimeResponse,Bytes(p.begin(),p.end()));
        req=readFrame(socket);uint16_t second=0;REQUIRE(tw::decodeRequest(req.data(),req.size(),second));
        CHECK(second!=id);
        // Repeated stale reply cannot complete the second request.
        sendFrame(socket,protocol::defs::msg::kTimeResponse,Bytes(p.begin(),p.end()));
        tw::response(p.data(),p.size(),{second,30,40});
        sendFrame(socket,protocol::defs::msg::kTimeResponse,Bytes(p.begin(),p.end()));
    });
    SessionOptions o=testOptions();o.timeout=std::chrono::milliseconds(60);o.read_retries=0;
    Session session(device.endpoint(),o);
    CHECK_THROWS_AS(session.timePing(),Error);
    const auto before=monotonicUs();const auto s=session.timePing();const auto after=monotonicUs();
    CHECK(s.esp_rx_us==30);CHECK(s.esp_tx_us==40);
    CHECK(s.host_tx_us>=before);CHECK(s.host_rx_us>=s.host_tx_us);CHECK(s.host_rx_us<=after);
}
TEST_CASE("matching malformed time reply is an explicit protocol failure") {
    for(bool bad_version:{false,true}) {
        FakeDevice device([bad_version](Socket &socket) {
            auto req=readFrame(socket);uint16_t id=0;REQUIRE(protocol::time_wire::decodeRequest(req.data(),req.size(),id));
            std::array<uint8_t,19> p{};protocol::time_wire::response(p.data(),p.size(),{id,1,2});
            if(bad_version){p[0]=2;}
            sendFrame(socket,protocol::defs::msg::kTimeResponse,Bytes(p.begin(),bad_version?p.end():p.end()-1));
        });
        Session session(device.endpoint(),testOptions());
        try {session.timePing();FAIL("accepted malformed ping");} catch(const Error &e){CHECK(e.kind==ErrorKind::Protocol);}
    }
}
TEST_CASE("closing session wakes a pending time ping") {
    std::promise<void> received;auto got=received.get_future();
    FakeDevice device([&](Socket &socket) {readFrame(socket);received.set_value();std::this_thread::sleep_for(std::chrono::milliseconds(80));});
    Session session(device.endpoint(),testOptions());
    auto pending=std::async(std::launch::async,[&] {try{session.timePing();return false;}catch(const Error &e){return e.kind==ErrorKind::Connection;}});
    got.wait();session.close();CHECK(pending.get());
}
TEST_CASE("Boot rejects a ping spanning reset and changes the next sample generation") {
    FakeDevice device([](Socket &socket) {
        auto answer_ping=[&] {
            auto req=readFrame(socket);uint16_t id=0;REQUIRE(protocol::time_wire::decodeRequest(req.data(),req.size(),id));
            std::array<uint8_t,19> p{};protocol::time_wire::response(p.data(),p.size(),{id,100,101});
            sendFrame(socket,protocol::defs::msg::kTimeResponse,Bytes(p.begin(),p.end()));
        };
        answer_ping();readFrame(socket);
        diag::protocol::BootEvent boot{};std::array<uint8_t,32> event{};
        const auto n=diag::protocol::encodeBootEvent(0,0,boot,event.data(),event.size());
        sendFrame(socket,protocol::defs::msg::kDiagEvent,Bytes(event.begin(),event.begin()+static_cast<ptrdiff_t>(n)));
        answer_ping();
    });
    Session session(device.endpoint(),testOptions());const auto first=session.timePing();
    try {session.timePing();FAIL("ping spanned Boot");}catch(const Error &e){CHECK(e.kind==ErrorKind::Protocol);}
    const auto next=session.timePing();CHECK(first.boot_generation!=next.boot_generation);
}
TEST_CASE("typed values reject malformed inputs instead of narrowing")
{
    auto d=ValueType::U8;
    CHECK(parseValue(d, "255") == 255);
    for (auto s : {"256", "-1", "1junk", "", "0x"})
    {
        CHECK_THROWS_AS(parseValue(d, s), Error);
    }
    d=ValueType::F32;
    CHECK(parseValue(d, "0.5") == std::bit_cast<uint32_t>(0.5f));
    for (auto s : {"NaN", "inf", "1e100", "2x"})
    {
        CHECK_THROWS_AS(parseValue(d, s), Error);
    }
    d=ValueType::Bool;
    CHECK(parseValue(d, "true") == 1);
    CHECK_THROWS_AS(parseValue(d, "2"), Error);
    d=ValueType::I32;
    CHECK(std::get<int32_t>(decodeValue(ValueType::I32, parseValue(d, "-2147483648"))) == -2147483648LL);
    CHECK_THROWS_AS(parseValue(d, "2147483648"), Error);
    d=ValueType::Enum;
    const std::array<EnumEntry,1> enums{{{1,"on"}}};
    CHECK(parseValue(d, "on", enums) == 1);
    CHECK_THROWS_AS(parseValue(d, "0", enums), Error);
    CHECK_THROWS_AS(decodeValue(ValueType::F32, 0x7fc00000), Error);
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
    SessionOptions opts=testOptions();
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
    SessionOptions opts=testOptions();
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
    Session session(device.endpoint(),testOptions());
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
    Session session(device.endpoint(),testOptions());
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
    Session session(device.endpoint(),testOptions());
    Client client(session);
    CHECK_THROWS_AS(client.diagEntries(), Error);
}
TEST_CASE("diagnostic event decoding preserves instance identity and exact lengths")
{
    FakeDevice device([](Socket &) {});
    Session session(device.endpoint(),testOptions());
    Client client(session);
    std::array<uint8_t, 1024> buffer{};
    diag::protocol::RecordEvent report{123456789, 2, 112, 7, 42, -1, 99, 101};
    auto n = diag::protocol::encodeRecordEvent(65535, 0, report, buffer.data(), buffer.size());
    Frame frame{192, Bytes(buffer.begin(), buffer.begin() + n)};
    auto decoded = std::get<DiagRecord>(client.event(frame));
    CHECK(decoded.instance == 7);
    CHECK(decoded.code == 42);
    CHECK(decoded.error == -1);
    CHECK(decoded.timestamp_us == 123456789);
    CHECK(decoded.sequence == 65535);
    frame.payload.push_back(0);
    CHECK_THROWS_AS(client.event(frame), Error);
    diag::protocol::CounterEntry entry{112, 42, 3, 9, 7};
    n = diag::protocol::encodeCounterDeltaEvent(1, 0, 123, &entry, 1, buffer.data(), buffer.size());
    const auto counters=std::get<DiagCounters>(client.event({192, Bytes(buffer.begin(), buffer.begin() + n)}));
    CHECK(counters.entries[0].instance == 7);
    CHECK(counters.entries[0].delta == 3);
    diag::protocol::BootEvent boot{1, 0, 3, 1, 1};
    n = diag::protocol::encodeBootEvent(2, 0, boot, buffer.data(), buffer.size());
    CHECK(std::get<BootEvent>(client.event({192, Bytes(buffer.begin(), buffer.begin() + n)})).safe_mode == 1);
    diag::protocol::ClearedEvent clear{22, 112, 7, 42};
    n = diag::protocol::encodeClearedEvent(3, 0, clear, buffer.data(), buffer.size());
    CHECK(std::get<DiagCleared>(client.event({192, Bytes(buffer.begin(), buffer.begin() + n)})).instance == 7);
}

TEST_CASE("status v5 carries sparse loop ids and rejects duplicate ids and bad counts")
{
    FakeDevice device([](Socket &socket) {
        std::array<uint8_t,1> byte{};asio::error_code ec;
        socket.read_some(asio::buffer(byte),ec);
    });
    Session session(device.endpoint(),testOptions()); Client client(session);
    Bytes payload(18,0); payload[0]=protocol::core_wire::kVersion; payload[17]=2;
    payload.insert(payload.end(), {3,1,0,0,0, 19,9,0,0,0, 0});
    auto status=std::get<StatusFrame>(client.event({128,payload}));
    CHECK(status.overruns[0].loop_id==3);
    CHECK(status.overruns[1].loop_id==19);
    CHECK(status.overruns[1].overruns==9);
    payload[23]=3;CHECK_THROWS_AS(client.event({128,payload}),Error);
    payload[23]=19;payload[17]=9;CHECK_THROWS_AS(client.event({128,payload}),Error);
    payload[17]=2;payload.push_back(0);CHECK_THROWS_AS(client.event({128,payload}),Error);
}

TEST_CASE("GET_INFO v5 validates sparse ids, loop flags and duplicate loop identities")
{
    auto body=[](bool duplicate, bool bad_flags) {
        Bytes bytes(256);protocol::wire::Writer w(bytes.data(),bytes.size());
        w.appendU8(protocol::core_wire::kVersion);protocol::core_wire::string(w,"test");protocol::core_wire::string(w,"build");
        w.appendU32Le(1);w.appendU32Le(0);w.appendU16Le(1024);w.appendU8(2);w.appendU8(0);w.appendU8(2);
        w.appendU8(3);protocol::core_wire::string(w,"first");w.appendU32Le(1000);w.appendU8(1);
        w.appendU8(duplicate?3:19);protocol::core_wire::string(w,"second");w.appendU32Le(100);w.appendU8(bad_flags?2:0);
        const uint8_t id[6]={2,0x44,0x4c,0,0,1};w.appendBytes(id,6);w.appendU8(0);
        bytes.resize(w.size());return bytes;
    };
    FakeDevice device([&](Socket &socket) {
        answer(socket,readFrame(socket),body(false,false));
        answer(socket,readFrame(socket),body(true,false));
        answer(socket,readFrame(socket),body(false,true));
    });
    Session session(device.endpoint(),testOptions());Client client(session);
    const auto info=client.info();CHECK(info.loops[0].realtime()==true);
    CHECK(info.loops[1].id==19);CHECK(info.loops[1].realtime()==false);
    CHECK_THROWS_AS(client.info(),Error);CHECK_THROWS_AS(client.info(),Error);
}

TEST_CASE("command payloads and subscription decoding preserve types and reject malformed streams")
{
    const std::array<uint32_t,2> values{std::bit_cast<uint32_t>(1.5F), 42};
    const auto payload=commandPayload(255,65535,values);
    CHECK(payload==Bytes{protocol::core_wire::kVersion,255,255,255,0,0,192,63,42,0,0,0});
    CHECK(commandPayload(1,0,{}).size()==4);
    std::array<uint32_t,65> too_many{};CHECK_THROWS_AS(commandPayload(1,0,too_many),Error);
    Group group;group.channel=129;group.epoch=255;group.configured=true;group.refs.resize(2);
    std::array<ItemDescriptor,2> descriptors{};descriptors[0].value_type=ValueType::F32;descriptors[1].value_type=ValueType::U32;
    Frame frame{129,Bytes{protocol::core_wire::kVersion,255,255,255,1,0,0,0,0,0,0,0,0,0,192,63,42,0,0,0}};
    const auto decoded=decodeSubscription(frame,group,descriptors);
    REQUIRE(decoded);CHECK(decoded->counter==65535);CHECK(decoded->timestamp_us==1);
    REQUIRE(decoded);CHECK(std::get<float>(decoded->typed_values[0])==1.5F);CHECK(std::get<uint32_t>(decoded->typed_values[1])==42);
    frame.payload[1]=254;CHECK(!decodeSubscription(frame,group,descriptors));
    frame.payload[1]=255;frame.type=130;CHECK(!decodeSubscription(frame,group,descriptors));frame.type=129;
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
        // Limits are GET params; GROUP LIST only carries the page header.
        answer(socket,readFrame(socket),{3,1,0,0,0,1,0,0,0,64,0,0,0});
        answer(socket,readFrame(socket),{2,0,1,0,1, 0,0,3,19,7,2,0,1,112,0,0,1});
        answer(socket,readFrame(socket),{2,0,255,255,1, 1,0,0,255,0,1,0,0});
        answer(socket,readFrame(socket),{8,129});
        answer(socket,readFrame(socket),{9,130}); // wrong channel for group zero
        const auto command=readFrame(socket);
        if(command!=Bytes{protocol::core_wire::kVersion,8,99,0,42,0,0,0}) { throw std::runtime_error("bad command payload"); }
        answer(socket,readFrame(socket)); // drain barrier
        // A non-progressing page must fail rather than loop forever.
        answer(socket,readFrame(socket),{3,1,0,0,0,1,0,0,0,64,0,0,0});
        answer(socket,readFrame(socket),{2,0,0,0,0});
    });
    Session session(device.endpoint(),testOptions());Client client(session);
    const auto listing=client.groups();CHECK(listing.groups.size()==2);
    CHECK(listing.groups[0].loop==19);CHECK(listing.groups[0].refs[0].item==256);
    CHECK(client.subEnable(0,true).epoch==8);
    CHECK_THROWS_AS(client.subEnable(0,false),Error);
    const std::array<uint32_t,1> values{42};REQUIRE(client.command(0,8,99,values));
    client.system(5);
    CHECK_THROWS_AS(client.groups(),Error);
}

TEST_CASE("host capture decodes STATUS, pages READ within the payload and decodes the done event")
{
    // STATUS: done, configured, loop 3, divider 2, 3 samples, pre 1, threshold
    // on 32:0:256 falling at 0, recorded 3, trigger 1, period 1000, ts 77, two refs.
    auto status=[](uint8_t refs, uint8_t flags) {
        Bytes b(64);protocol::wire::Writer w(b.data(),b.size());
        w.appendU8(3);w.appendU8(flags);w.appendU8(3);w.appendU16Le(2);w.appendU32Le(3);w.appendU32Le(1);
        w.appendU8(3);w.appendU8(32);w.appendU8(0);w.appendU16Le(256);w.appendU8(1);w.appendU32Le(0);
        w.appendU32Le(3);w.appendU32Le(1);w.appendU32Le(1000);w.appendU64Le(77);w.appendU8(refs);
        for(uint8_t i=0;i<refs;++i) { w.appendU8(32);w.appendU8(0);w.appendU16Le(static_cast<uint16_t>(256+i)); }
        b.resize(w.size());return b;
    };
    auto page=[](uint32_t first,uint16_t count) {
        Bytes b(6+8U*count);protocol::wire::Writer w(b.data(),b.size());
        w.appendU32Le(first);w.appendU16Le(count);
        for(uint32_t i=0;i<count;++i) { w.appendU32Le(first+i);w.appendU32Le(100+first+i); }
        return b;
    };
    FakeDevice device([&](Socket &socket) {
        answer(socket,readFrame(socket),status(2,3));
        // GET_INFO advertises a 28-byte payload: two samples of two values per READ.
        Bytes info(64);protocol::wire::Writer iw(info.data(),info.size());
        iw.appendU8(protocol::core_wire::kVersion);protocol::core_wire::string(iw,"t");protocol::core_wire::string(iw,"b");
        iw.appendU32Le(1);iw.appendU32Le(65536);iw.appendU16Le(28);iw.appendU8(0);iw.appendU8(0);iw.appendU8(0);
        const uint8_t id[6]={2,0x44,0x4c,0,0,1};iw.appendBytes(id,6);iw.appendU8(0);
        info.resize(iw.size());
        answer(socket,readFrame(socket),info);
        answer(socket,readFrame(socket),status(2,1));
        for(uint32_t first:{0U,2U}) {
            const auto request=readFrame(socket);
            if(request.size()!=10 || request[4]!=first || request[8]!=(first==0?2:1)) { throw std::runtime_error("bad read"); }
            answer(socket,request,page(first,first==0?2:1));
        }
        answer(socket,readFrame(socket),status(1,1)); // a valid one-ref status
        answer(socket,readFrame(socket),page(0,1));    // two values for a one-ref capture
        answer(socket,readFrame(socket),status(0,1));   // configured without refs
    });
    Session session(device.endpoint(),testOptions());Client client(session);
    const auto s=client.captureStatus();
    CHECK(s.state==CaptureState::Done);CHECK(s.irregular==true);CHECK(s.divider==2);
    CHECK(s.trigger.kind==3);CHECK(s.trigger.edge==1);
    CHECK(s.trigger_index==1);CHECK(s.sample_period_us==2000);CHECK(s.refs.size()==2);
    CHECK(client.info().max_payload==28);
    const auto all=client.captureData(); // pages 0..1, then 2
    CHECK(all.status.irregular==false);
    CHECK(all.data==std::vector<std::vector<uint32_t>>{{0,100},{1,101},{2,102}});
    CHECK(client.captureStatus().refs.size()==1);
    CHECK_THROWS_AS(client.captureRead(0,1,1),Error);
    CHECK_THROWS_AS(client.captureRead(0,1,65),Error);
    CHECK_THROWS_AS(client.captureStatus(),Error);
    Bytes event{protocol::core_wire::kVersion,1,3,0,0,0,1,0,0,0,2};
    const auto done=std::get<CaptureDone>(client.event({protocol::defs::msg::kCoreEvent,event}));
    CHECK(done.recorded==3);CHECK(done.irregular==true);
    event[1]=2;CHECK_THROWS_AS(client.event({protocol::defs::msg::kCoreEvent,event}),Error);
    event[1]=1;event.push_back(0);CHECK_THROWS_AS(client.event({protocol::defs::msg::kCoreEvent,event}),Error);
    CaptureTrigger bad;bad.kind=4;const std::array<Ref,1> refs{{{32,0,256}}};
    CHECK_THROWS_AS(client.captureConfig(0,1,10,0,bad,refs),Error);
    CHECK_THROWS_AS(client.captureConfig(0,1,10,10,{},refs),Error);
}

TEST_CASE("testgen SDK encodes requests and rejects malformed status and events")
{
    auto status=[] {
        Bytes b(48); protocol::wire::Writer w(b.data(),b.size());
        w.appendU8(3); w.appendU8(2); w.appendU8(1); // loop 3, done/completed
        w.appendU32Le(42); w.appendU32Le(2000); w.appendU32Le(1); w.appendU32Le(protocol::defs::f32Bits(.1F));
        w.appendU8(32); w.appendU8(0); w.appendU16Le(514); w.appendU8(4);
        for (float x : {0.F,.1F,.5F,3.F}) { w.appendU32Le(protocol::defs::f32Bits(x)); }
        w.appendU32Le(2000); w.appendU32Le(1); return b;
    };
    FakeDevice device([&](Socket &socket) {
        const auto start=readFrame(socket);
        if (start.size()!=34 || start[2]!=7 || start[3]!=1 || start[4]!=3 || start[9]!=4) {
            throw std::runtime_error("wrong START encoding");
        }
        answer(socket,start,Bytes{42,0,0,0});
        answer(socket,readFrame(socket),status());
        answer(socket,readFrame(socket)); // STOP
        auto malformed=status(); malformed.push_back(0);
        answer(socket,readFrame(socket),malformed);
        malformed=status(); malformed[1]=9; answer(socket,readFrame(socket),malformed);
        answer(socket,readFrame(socket),Bytes{1}); // truncated START reply
    });
    Session session(device.endpoint(),testOptions()); Client client(session);
    TestgenConfig c; c.loop=3; c.target={32,0,514}; c.waveform=4;
    c.amplitude=.1F; c.frequency_start=.5F; c.frequency_end=3; c.duration_ms=2000;
    CHECK(client.testgenStart(c).run==42);
    const auto s=client.testgenStatus(3); CHECK(s.state==TestgenState::Done); CHECK(s.reason==TestgenReason::Completed);
    CHECK(s.waveform==4); CHECK(s.target.item==514);
    CHECK(client.testgenStop(3).ok==true);
    CHECK_THROWS_AS(client.testgenStatus(3),Error); CHECK_THROWS_AS(client.testgenStatus(3),Error);
    CHECK_THROWS_AS(client.testgenStart(c),Error);
    Bytes event{protocol::core_wire::kVersion,2,3,42,0,0,0,2};
    CHECK(std::get<TestgenDone>(client.event({protocol::defs::msg::kCoreEvent,event})).reason==TestgenReason::Stopped);
    event[7]=0; CHECK_THROWS_AS(client.event({protocol::defs::msg::kCoreEvent,event}),Error);
    event[7]=3; event.push_back(0); CHECK_THROWS_AS(client.event({protocol::defs::msg::kCoreEvent,event}),Error);
    CHECK(parseTestgenWaveform("chirp_log")==5); CHECK_THROWS_AS(parseTestgenWaveform("invalid"),Error);
}


TEST_CASE("SET_NAME owns valid UTF-8 bytes and reports uncertain malformed mutation replies")
{
    FakeDevice device([](Socket &socket) {
        const auto req=readFrame(socket);
        if (req.size()!=7 || req[2]!=1 || req[3]!=10 || req[4]!=2 || req[5]!=0xC3 || req[6]!=0xA9) {
            throw std::runtime_error("bad SET_NAME encoding");
        }
        answer(socket,req,{});
        answer(socket,readFrame(socket),{42}); // a success with an impossible body
    });
    Session session(device.endpoint(),testOptions()); Client client(session);
    try {client.setName(std::string(17,'a')); FAIL("accepted oversized name");}
    catch(const Error &e) {CHECK(e.kind==ErrorKind::Usage);CHECK_FALSE(e.outcome_unknown);}
    CHECK(client.setName("\xC3\xA9").ok);
    try {client.setName(""); FAIL("accepted malformed reply");}
    catch(const Error &e) {CHECK(e.kind==ErrorKind::Protocol);CHECK(e.outcome_unknown);CHECK(e.family==1);CHECK(e.op==10);}
}

TEST_CASE("GET_INFO requires complete identity and validates name bytes")
{
    auto info=[] {
        Bytes b(128); protocol::wire::Writer w(b.data(),b.size());
        w.appendU8(protocol::core_wire::kVersion);
        protocol::core_wire::string(w,"test");protocol::core_wire::string(w,"id");
        w.appendU32Le(1);w.appendU32Le(0);w.appendU16Le(1024);
        w.appendU8(0);w.appendU8(8);w.appendU8(0);
        const uint8_t id[]{2,4,6,8,10,12};w.appendBytes(id,6);
        (void)protocol::writeDeviceName(w,"robot");b.resize(w.size());return b;
    };
    FakeDevice device([&](Socket &socket) {
        auto good=info();answer(socket,readFrame(socket),good);
        auto b=good;b.pop_back();answer(socket,readFrame(socket),b);
        b=good;b.back()=0;answer(socket,readFrame(socket),b);
        b=good;b.push_back(0);answer(socket,readFrame(socket),b);
        b=good;b.resize(b.size()-12);answer(socket,readFrame(socket),b);
    });
    Session session(device.endpoint(),testOptions());Client client(session);
    const auto result=client.info();
    CHECK(hex(result.device_id)=="020406080a0c");CHECK(result.name=="robot");
    for(int i=0;i<4;++i) {CHECK_THROWS_AS(client.info(),Error);}
}
