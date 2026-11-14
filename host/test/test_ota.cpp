#include "doctest.h"
#include "drivelink/client.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "core/core.hpp"
#include "core/command_service.hpp"
#include "mod_mock/modules.hpp"
#include "../../protocol/test/ota_fixture.hpp"
#include <asio.hpp>
#include <future>
#include <thread>

namespace {
using namespace drivelink;
namespace p=drivelink::protocol;
using Socket=asio::ip::tcp::socket;
Bytes readFrame(Socket &s) {
    std::array<uint8_t,7> h{};asio::read(s,asio::buffer(h));
    const size_t n=size_t(h[4])+(size_t(h[5])<<8);if (n>1024) { throw std::runtime_error("bad request"); }
    Bytes b(n+2);asio::read(s,asio::buffer(b));b.resize(n);return b;
}
void send(Socket &s,const Bytes &payload) {
    Bytes frame(p::frameSize(payload.size()));p::encodeFrame(frame.data(),frame.size(),p::defs::msg::kCoreResponse,payload.data(),payload.size());
    asio::write(s,asio::buffer(frame));
}
void answer(Socket &s,const Bytes &req,uint32_t offset) {
    Bytes out{p::core_wire::kVersion,req[1],req[2],req[3],0,0};
    for (unsigned i=0;i<4;++i) { out.push_back(static_cast<uint8_t>(offset>>(8*i))); }send(s,out);
}
struct Device {
    asio::io_context io;asio::ip::tcp::acceptor listener{io,{asio::ip::address_v4::loopback(),0}};
    std::thread worker;
    explicit Device(std::function<void(Socket &)> fn) : worker([this,fn] {
        try { Socket socket(io);listener.accept(socket);socket.set_option(asio::ip::tcp::no_delay(true));fn(socket); }
        catch (const asio::system_error &e) { CHECK((e.code()==asio::error::eof || e.code()==asio::error::connection_reset)); }
    }) {}
    ~Device() { worker.join(); }
    std::string endpoint() const { return "tcp://127.0.0.1:"+std::to_string(listener.local_endpoint().port()); }
};
Bytes body(uint32_t offset) {
    Bytes bytes(7);p::wire::Writer w(bytes.data(),bytes.size());w.appendU32Le(offset);w.appendU16Le(1);w.appendU8(42);return bytes;
}
}
TEST_CASE("OTA writes actually pipeline and correlate reversed and unrelated replies") {
    Device device([](Socket &s) {
        std::array<Bytes,4> requests;for (auto &r:requests) { r=readFrame(s); }
        auto wrong=requests[0];wrong[2]=1;answer(s,wrong,99); // same seq, other family ignored
        for (size_t i=4;i>0;--i) { answer(s,requests[i-1],static_cast<uint32_t>(i)); }
    });
    Session session(device.endpoint());std::array<Bytes,4> bodies{body(0),body(1),body(2),body(3)};
    const auto replies=session.otaWriteBatch(bodies);REQUIRE(replies.size()==4);
    for (size_t i=0;i<4;++i) { CHECK(replies[i].result==0);CHECK(replies[i].body==Bytes{static_cast<uint8_t>(i+1),0,0,0}); }
}
TEST_CASE("OTA missing replies retire IDs and stale replies cannot complete later batches") {
    Device device([](Socket &s) {
        const auto first=readFrame(s);const auto second=readFrame(s);answer(s,second,2);
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        const auto fresh=readFrame(s);CHECK(fresh[1]!=first[1]);CHECK(fresh[1]!=second[1]);
        answer(s,first,99);answer(s,fresh,3);
    });
    SessionOptions options;options.timeout=std::chrono::milliseconds(150);options.read_retries=0;
    Session session(device.endpoint(),options);std::array<Bytes,2> bodies{body(0),body(1)};
    CHECK_THROWS_AS(session.otaWriteBatch(bodies),Error);
    const std::array<Bytes,1> fresh{body(2)};const auto replies=session.otaWriteBatch(fresh);
    CHECK(replies[0].body==Bytes{3,0,0,0});
}
TEST_CASE("OTA matching malformed response and explicit close wake all waiters") {
    SUBCASE("malformed") {
        Device device([](Socket &s) {const auto r=readFrame(s);send(s,{7,r[1],9,2,0,0});});
        Session session(device.endpoint());const std::array<Bytes,1> bodies{body(0)};
        try { session.otaWriteBatch(bodies);FAIL("accepted wrong version"); }catch (const Error &e) { CHECK(e.kind==ErrorKind::Protocol); }
    }
    SUBCASE("close") {
        std::promise<void> received;auto ready=received.get_future();
        Device device([&](Socket &s) {readFrame(s);received.set_value();std::this_thread::sleep_for(std::chrono::milliseconds(100));});
        Session session(device.endpoint());auto pending=std::async(std::launch::async,[&] {
            try { const std::array<Bytes,1> bodies{body(0)};session.otaWriteBatch(bodies);return false; }
            catch (const Error &e) { return e.kind==ErrorKind::Connection; }
        });
        ready.wait();session.close();CHECK(pending.get());
    }
}
TEST_CASE("OTA status parser rejects invalid states limits and truncated payloads") {
    Device device([](Socket &s) {
        for (unsigned mode=0;mode<5;++mode) {
            const auto req=readFrame(s);Bytes out{p::core_wire::kVersion,req[1],9,6,0,0};Bytes b(88);
            p::wire::Writer w(b.data(),b.size());
            w.appendU8(mode==1?9:0);w.appendU8(0);w.appendU8(0);w.appendU8(mode==2?2:0);
            w.appendU32Le(0);w.appendU32Le(0);w.appendU32Le(p::kOtaSlotBytes);w.appendU16Le(512);w.appendU8(mode==3?5:4);
            if (mode==4) { b.pop_back(); }out.insert(out.end(),b.begin(),b.end());send(s,out);
        }
    });
    Session session(device.endpoint());Client client(session);CHECK(client.otaStatus()["state"]==0);
    for (unsigned i=0;i<4;++i) { CHECK_THROWS_AS(client.otaStatus(),Error); }
}
TEST_CASE("SDK reconciles applied BEGIN WRITE and FINISH after lost replies") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(store).ok());
        core::CommandService commands(core,{"native","native",1024,false,4});
        std::array<bool,7> dropped{};std::array<uint8_t,1024> response{};
        for (;;) {
            const auto req=readFrame(s);const auto n=commands.handle(req,response);
            if (req[2]==9 && (req[3]==1 || req[3]==2 || req[3]==3) && !dropped[req[3]]) { dropped[req[3]]=true;continue; }
            send(s,Bytes(response.begin(),response.begin()+n));
        }
    });
    SessionOptions options;options.timeout=std::chrono::milliseconds(150);options.read_retries=0;
    Session session(device.endpoint(),options);Client client(session);const auto image=otaFixture();
    CHECK(client.otaUpload(image)["state"]==2);CHECK(client.otaStatus()["offset"]==image.size());
    CHECK(client.otaAbort()["aborted"]==true);CHECK(client.otaStatus()["state"]==0);
}
TEST_CASE("SDK cancellation attempts ABORT and clears update state") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(store).ok());
        core::CommandService commands(core,{});std::array<uint8_t,1024> response{};
        for (;;) { const auto req=readFrame(s);const auto n=commands.handle(req,response);send(s,Bytes(response.begin(),response.begin()+n)); }
    });
    Session session(device.endpoint());Client client(session);const auto image=otaFixture();
    CHECK_THROWS_AS(client.otaUpload(image,1,[](uint32_t,uint32_t){return false;}),Error);
    CHECK(client.otaStatus()["state"]==0);
}
TEST_CASE("SDK OTA honors reduced payload limits including batch acknowledgements") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(store).ok());
        core::CommandService commands(core,{"native","native",512,false,4});std::array<uint8_t,1024> response{};
        for (;;) { const auto req=readFrame(s);CHECK(req.size()<=512);const auto n=commands.handle(req,response);send(s,Bytes(response.begin(),response.begin()+n)); }
    });
    Session session(device.endpoint());Client client(session);client.info();
    CHECK(client.otaStatus()["chunk_bytes"]==502);
    const auto image=otaFixture();CHECK(client.otaUpload(image)["state"]==2);client.otaAbort();
}
