#include "doctest.h"
#include "test_options.hpp"
#include "drivelink/client.hpp"
#include "drivelink/ota.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "core/core.hpp"
#include "core/command_service.hpp"
#include "mod_mock/modules.hpp"
#include "../../protocol/test/ota_fixture.hpp"
#include <asio.hpp>
#include <atomic>
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
    Session session(device.endpoint(),testOptions());std::array<Bytes,4> bodies{body(0),body(1),body(2),body(3)};
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
    SessionOptions options=testOptions();options.timeout=std::chrono::milliseconds(150);options.read_retries=0;
    Session session(device.endpoint(),options);std::array<Bytes,2> bodies{body(0),body(1)};
    CHECK_THROWS_AS(session.otaWriteBatch(bodies),Error);
    const std::array<Bytes,1> fresh{body(2)};const auto replies=session.otaWriteBatch(fresh);
    CHECK(replies[0].body==Bytes{3,0,0,0});
}
TEST_CASE("OTA matching malformed response and explicit close wake all waiters") {
    SUBCASE("malformed") {
        Device device([](Socket &s) {const auto r=readFrame(s);send(s,{7,r[1],9,2,0,0});});
        Session session(device.endpoint(),testOptions());const std::array<Bytes,1> bodies{body(0)};
        try { session.otaWriteBatch(bodies);FAIL("accepted wrong version"); }catch (const Error &e) { CHECK(e.kind==ErrorKind::Protocol); }
    }
    SUBCASE("close") {
        std::promise<void> received;auto ready=received.get_future();
        Device device([&](Socket &s) {readFrame(s);received.set_value();std::this_thread::sleep_for(std::chrono::milliseconds(100));});
        Session session(device.endpoint(),testOptions());auto pending=std::async(std::launch::async,[&] {
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
    Session session(device.endpoint(),testOptions());Client client(session);CHECK(client.otaStatus().state==0);
    for (unsigned i=0;i<4;++i) { CHECK_THROWS_AS(client.otaStatus(),Error); }
}
TEST_CASE("SDK reconciles applied BEGIN WRITE and FINISH after lost replies") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok());
        core::CommandService commands(core,{"native","native",1024,false,4});
        std::array<bool,7> dropped{};std::array<uint8_t,1024> response{};
        for (;;) {
            const auto req=readFrame(s);const auto n=commands.handle(req,response);
            if (req[2]==9 && (req[3]==1 || req[3]==2 || req[3]==3) && !dropped[req[3]]) { dropped[req[3]]=true;continue; }
            send(s,Bytes(response.begin(),response.begin()+n));
        }
    });
    SessionOptions options=testOptions();options.timeout=std::chrono::milliseconds(150);options.read_retries=0;
    Session session(device.endpoint(),options);Client client(session);const auto image=otaFixture();
    CHECK(client.otaUpload(image).state==2);CHECK(client.otaStatus().offset==image.size());
    CHECK(client.otaAbort().ok==true);CHECK(client.otaStatus().state==0);
}
TEST_CASE("SDK cancellation attempts ABORT and clears update state") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok());
        core::CommandService commands(core,{});std::array<uint8_t,1024> response{};
        for (;;) { const auto req=readFrame(s);const auto n=commands.handle(req,response);send(s,Bytes(response.begin(),response.begin()+n)); }
    });
    Session session(device.endpoint(),testOptions());Client client(session);const auto image=otaFixture();
    CHECK_THROWS_AS(client.otaUpload(image,1,[](const OtaProgress &){return false;}),Error);
    CHECK(client.otaStatus().state==0);
}
TEST_CASE("SDK OTA cleanup preserves DeviceError context and dynamic type") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok());
        core::CommandService commands(core,{});std::array<uint8_t,1024> response{};
        for (;;) {
            const auto req=readFrame(s);const auto n=commands.handle(req,response);
            if(req[2]==9 && req[3]==1) { send(s,{p::core_wire::kVersion,req[1],9,1,1,10}); }
            else { send(s,Bytes(response.begin(),response.begin()+n)); }
        }
    });
    Session session(device.endpoint(),testOptions());Client client(session);
    try { client.otaUpload(otaFixture());FAIL("accepted BEGIN refusal"); }
    catch(const DeviceError &e) {
        CHECK(e.kind==ErrorKind::Rejected);CHECK(e.result==1);CHECK(e.detail==10);
        CHECK(e.family==9);CHECK(e.op==1);CHECK(e.has_request_context);CHECK_FALSE(e.outcome_unknown);
        CHECK(std::string(e.what()).find("update aborted")!=std::string::npos);
    }
}
TEST_CASE("SDK OTA failed reconciliation retains the lost mutation's uncertainty") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok());
        core::CommandService commands(core,{});std::array<uint8_t,1024> response{};
        bool dropped=false;
        for (;;) {
            const auto req=readFrame(s);const auto n=commands.handle(req,response);
            if(req[2]==9 && req[3]==1) { dropped=true;continue; }
            if(dropped && req[2]==9 && req[3]==6) { return; }
            send(s,Bytes(response.begin(),response.begin()+n));
        }
    });
    SessionOptions options=testOptions();options.timeout=std::chrono::milliseconds(60);options.read_retries=0;
    Session session(device.endpoint(),options);Client client(session);
    try { client.otaUpload(otaFixture());FAIL("accepted missing reconciliation"); }
    catch(const Error &e) {
        CHECK(e.kind==ErrorKind::Connection);CHECK(e.family==9);CHECK(e.op==6);CHECK(e.outcome_unknown);
        CHECK(std::string(e.what()).find("cleanup uncertain")!=std::string::npos);
    }
}
TEST_CASE("SDK pipelined OTA malformed WRITE bodies retain mutation context") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok());
        core::CommandService commands(core,{});std::array<uint8_t,1024> response{};
        bool malformed=false;
        for (;;) {
            const auto req=readFrame(s);const auto n=commands.handle(req,response);
            if(req[2]==9 && req[3]==2 && !malformed) {
                malformed=true;send(s,{p::core_wire::kVersion,req[1],9,2,0,0,0});
            } else { send(s,Bytes(response.begin(),response.begin()+n)); }
        }
    });
    Session session(device.endpoint(),testOptions());Client client(session);
    try { client.otaUpload(otaFixture());FAIL("accepted malformed WRITE"); }
    catch(const Error &e) {
        CHECK(e.kind==ErrorKind::Protocol);CHECK(e.has_request_context);
        CHECK(e.family==9);CHECK(e.op==2);CHECK(e.outcome_unknown);
        CHECK(std::string(e.what()).find("update aborted")!=std::string::npos);
    }
}
TEST_CASE("SDK OTA honors reduced payload limits including batch acknowledgements") {
    Device device([](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok());
        core::CommandService commands(core,{"native","native",512,false,4});std::array<uint8_t,1024> response{};
        for (;;) { const auto req=readFrame(s);CHECK(req.size()<=512);const auto n=commands.handle(req,response);send(s,Bytes(response.begin(),response.begin()+n)); }
    });
    Session session(device.endpoint(),testOptions());Client client(session);client.info();
    CHECK(client.otaStatus().chunk_bytes==502);
    const auto image=otaFixture();CHECK(client.otaUpload(image).state==2);client.otaAbort();
}
TEST_CASE("OTA image description reads the app descriptor and both digests") {
    auto bytes=otaFixture(4096,9);
    std::copy_n("1.2.3-rc",9,bytes.data()+48);std::copy_n("drivelink_v2",13,bytes.data()+80);
    std::copy_n("12:00:00",9,bytes.data()+112);std::copy_n("Nov 17 2026",12,bytes.data()+128);
    // Re-seal the edited descriptor: checksum over the segment, then the appended SHA.
    const size_t end=bytes.size()-32;uint8_t checksum=0xef;
    for (size_t i=32;i<32+4096;++i) { checksum^=bytes[i]; }bytes[end-1]=checksum;
    const auto digest=p::sha256(std::span(bytes).first(end));std::copy(digest.begin(),digest.end(),bytes.begin()+end);
    const auto image=describeOtaImage(bytes);
    CHECK(image.version=="1.2.3-rc");CHECK(image.project=="drivelink_v2");
    CHECK(image.time=="12:00:00");CHECK(image.date=="Nov 17 2026");
    CHECK(image.elf_sha256[0]==9);CHECK(image.sha256==p::sha256(bytes));
    CHECK(image.buildId()==hex(image.elf_sha256).substr(0,16));CHECK(image.bytes==bytes);
    bytes.push_back(0);CHECK_THROWS_AS(describeOtaImage(bytes),Error);
}
TEST_CASE("OTA boot classification confirms only the uploaded build in the new slot") {
    const auto image=describeOtaImage(otaFixture(4096,5));
    const OtaCandidate candidate{image.elf_sha256,0};
    DeviceInfo info;info.build_id=image.buildId();
    OtaStatus s;s.running_slot=1;s.boot_slot=1;s.pending_confirm=true;s.elf_sha256=image.elf_sha256;
    CHECK(classifyOtaBoot(candidate,info,s)==OtaBoot::Confirmable);
    s.pending_confirm=false;CHECK(classifyOtaBoot(candidate,info,s)==OtaBoot::Confirmed);
    CHECK(otaBootProblem(OtaBoot::Confirmed).empty());
    auto wrongBuild=info;wrongBuild.build_id="0000000000000000";
    CHECK(classifyOtaBoot(candidate,wrongBuild,s)==OtaBoot::Mismatch);
    auto other=s;other.elf_sha256[0]^=1;CHECK(classifyOtaBoot(candidate,info,other)==OtaBoot::Mismatch);
    auto split=s;split.boot_slot=0;CHECK(classifyOtaBoot(candidate,info,split)==OtaBoot::Mismatch);
    auto busy=s;busy.state=1;CHECK(classifyOtaBoot(candidate,info,busy)==OtaBoot::Mismatch);
    // The old slot runs with the prepared (or failed) image: REBOOT did not commit.
    OtaStatus old;old.running_slot=0;old.boot_slot=0;old.state=2;
    CHECK(classifyOtaBoot(candidate,info,old)==OtaBoot::NotCommitted);
    old.state=3;CHECK(classifyOtaBoot(candidate,info,old)==OtaBoot::NotCommitted);
    CHECK(otaBootProblem(OtaBoot::NotCommitted).find("commit did not complete")!=std::string::npos);
    // The old slot runs again, idle and confirmed: the bootloader rolled back.
    old.state=0;CHECK(classifyOtaBoot(candidate,info,old)==OtaBoot::RolledBack);
    CHECK(otaBootProblem(OtaBoot::RolledBack)=="running image/slot does not match uploaded candidate; not confirmed");
    old.pending_confirm=true;CHECK(classifyOtaBoot(candidate,info,old)==OtaBoot::Mismatch);
}
TEST_CASE("OTA reboot outcome: a lost reply is uncertain and a refusal throws") {
    // The peer answers GET_INFO, then refuses or ignores REBOOT.
    for (const bool refuse : {false,true}) {
        Device device([refuse](Socket &s) {
            core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok());
            core::CommandService commands(core,{});std::array<uint8_t,1024> response{};
            for (;;) {
                const auto req=readFrame(s);
                if (req[2]==1 && req[3]==8) {
                    if (!refuse) { continue; }
                    send(s,Bytes{p::core_wire::kVersion,req[1],req[2],req[3],
                                 uint8_t(p::defs::Result::Armed),uint8_t(p::defs::Detail::LockedWhileArmed)});continue;
                }
                const auto n=commands.handle(req,response);send(s,Bytes(response.begin(),response.begin()+n));
            }
        });
        SessionOptions options=testOptions();options.timeout=std::chrono::milliseconds(150);
        Session session(device.endpoint(),options);Client client(session);client.info();
        if (refuse) { CHECK_THROWS_AS(requestOtaReboot(client),DeviceError); }
        else { CHECK(requestOtaReboot(client)==OtaReboot::Uncertain); }
        session.close();
    }
}
TEST_CASE("OTA confirmation sends nothing for an idle device that never ran the candidate") {
    std::atomic<int> confirms{0};
    Device device([&confirms](Socket &s) {
        core_os::KvStore store;core::Core core({});REQUIRE(core.init(mod_mock::kTypes).ok());REQUIRE(core.boot(core::prepareBoot(store)).ok());
        core::CommandService commands(core,{});std::array<uint8_t,1024> response{};
        for (;;) {
            const auto req=readFrame(s);if (req[2]==9 && req[3]==5) { ++confirms; }
            const auto n=commands.handle(req,response);send(s,Bytes(response.begin(),response.begin()+n));
        }
    });
    Session session(device.endpoint(),testOptions());Client client(session);
    const auto running=client.otaStatus();
    const auto result=confirmOtaCandidate(client,{describeOtaImage(otaFixture(4096,3)).elf_sha256,running.running_slot},true);
    CHECK(result.verdict==OtaBoot::RolledBack);CHECK_FALSE(result.confirmed);CHECK(confirms==0);
    session.close();
}
