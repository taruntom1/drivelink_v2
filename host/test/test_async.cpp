#include "doctest.h"
#include "test_options.hpp"
#include "drivelink/buffers.hpp"
#include "drivelink/command.hpp"
#include "drivelink/client.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/time_codec.hpp"
#include <asio.hpp>
#include <future>
#include <thread>
#include <deque>
#include <bit>
#include <limits>
#include <cmath>

using namespace drivelink;
namespace {
namespace p=drivelink::protocol;
using Socket=asio::ip::tcp::socket;
struct Packet {uint8_t type;Bytes payload;};
Packet read(Socket &socket) {
    std::array<uint8_t,7> h{};asio::read(socket,asio::buffer(h));
    const auto size=size_t(h[4])+(size_t(h[5])<<8);if(size>1024) {throw std::runtime_error("bad frame");}
    Bytes b(size+2);asio::read(socket,asio::buffer(b));b.resize(size);return {h[3],std::move(b)};
}
void send(Socket &socket,uint8_t type,const Bytes &payload) {
    Bytes b(p::frameSize(payload.size()));p::encodeFrame(b.data(),b.size(),type,payload.data(),payload.size());asio::write(socket,asio::buffer(b));
}
void answer(Socket &socket,const Packet &packet,const Bytes &body={}) {
    const auto &r=packet.payload;Bytes b{p::core_wire::kVersion,r[1],r[2],r[3],0,0};b.insert(b.end(),body.begin(),body.end());send(socket,0x41,b);
}
void untilClosed(Socket &socket) {try {for(;;) {(void)read(socket);}}catch(const asio::system_error &) {}}
struct Peer {
    asio::io_context io;asio::ip::tcp::acceptor acceptor{io,{asio::ip::address_v4::loopback(),0}};
    std::thread worker;std::exception_ptr error;
    explicit Peer(std::function<void(Socket &)> fn):worker([this,fn=std::move(fn)] {
        try {Socket s(io);acceptor.accept(s);s.set_option(asio::ip::tcp::no_delay(true));fn(s);}
        catch(...) {error=std::current_exception();}
    }) {}
    ~Peer() {worker.join();CHECK(error==nullptr);}
    std::string endpoint() const {return "tcp://127.0.0.1:"+std::to_string(acceptor.local_endpoint().port());}
};
struct ManualDispatcher {
    std::mutex mutex;std::condition_variable cv;std::deque<Task> queue;
    Dispatcher dispatcher() {return [this](Task task) {{std::lock_guard lock(mutex);queue.push_back(std::move(task));}cv.notify_one();};}
    bool run() {
        Task task;{std::unique_lock lock(mutex);if(!cv.wait_for(lock,std::chrono::seconds(2),[&]{return !queue.empty();})) {return false;}
        task=std::move(queue.front());queue.pop_front();}task();return true;
    }
};
Bytes boot() {Bytes b(17);p::wire::Writer w(b.data(),b.size());w.appendU8(3);w.appendU16Le(1);w.appendU8(4);w.appendU8(0);w.appendU64Le(1);w.appendU8(0);w.appendU8(0);w.appendU8(0);w.appendU8(0);return b;}
Bytes sample(uint32_t n,size_t width=64,uint8_t epoch=4) {
    Bytes b(12+4*width);p::wire::Writer w(b.data(),b.size());w.appendU8(p::core_wire::kVersion);w.appendU8(epoch);
    w.appendU16Le(static_cast<uint16_t>(n));w.appendU64Le(n);for(size_t i=0;i<width;++i) {w.appendU32Le(n);}return b;
}
}

TEST_CASE("async completions use the supplied dispatcher exactly once in reply order") {
    Peer peer([](Socket &s) {
        std::array<Packet,3> requests;for(auto &r:requests) {r=read(s);}
        for(size_t i=3;i>0;--i) {answer(s,requests[i-1],{uint8_t(i)});answer(s,requests[i-1],{99});}untilClosed(s);
    });
    ManualDispatcher dispatcher;auto options=testOptions();options.dispatcher=dispatcher.dispatcher();
    Session session(peer.endpoint(),options);std::vector<int> order;const auto caller=std::this_thread::get_id();
    std::array<RequestHandle,3> handles;
    for(size_t i=0;i<3;++i) {handles[i]=session.asyncRequest(3,1,{},[&](auto result) {
        CHECK(std::this_thread::get_id()==caller);REQUIRE(result);order.push_back(result->body.at(0));
    });}
    CHECK(order.empty());for(int i=0;i<3;++i) {REQUIRE(dispatcher.run());}
    CHECK(order==std::vector<int>{3,2,1});for(auto &h:handles) {h.cancel();}session.close();
    CHECK(order.size()==3);
}
TEST_CASE("async cancel and close deliver errors without waiting for a stalled dispatcher") {
    std::promise<void> received;auto got=received.get_future();
    Peer peer([&](Socket &s) {(void)read(s);received.set_value();untilClosed(s);});
    ManualDispatcher dispatcher;auto options=testOptions();options.dispatcher=dispatcher.dispatcher();
    Session session(peer.endpoint(),options);std::vector<Error> errors;
    auto handle=session.asyncRequest(1,5,{},[&](auto r){REQUIRE_FALSE(r);errors.push_back(r.error());});
    REQUIRE(got.wait_for(std::chrono::seconds(2))==std::future_status::ready);handle.cancel();handle.cancel();
    session.close();REQUIRE(dispatcher.run());REQUIRE(errors.size()==1);
    CHECK(errors[0].kind==ErrorKind::Cancelled);CHECK(errors[0].outcome_unknown);CHECK(errors[0].family==1);CHECK(errors[0].op==5);
}
TEST_CASE("completion admission stays bounded while the dispatcher is paused") {
    Peer peer([](Socket &s){for(int i=0;i<2;++i) {auto r=read(s);answer(s,r);}untilClosed(s);});
    ManualDispatcher dispatcher;auto options=testOptions();options.request_capacity=2;options.dispatcher=dispatcher.dispatcher();
    Session session(peer.endpoint(),options);std::atomic<int> calls=0;
    for(int i=0;i<2;++i) {session.asyncRequest(3,1,{},[&](auto r){CHECK(r.has_value());++calls;});}
    CHECK_THROWS_AS(session.asyncRequest(3,1,{},[](auto){}),Error);
    REQUIRE(dispatcher.run());REQUIRE(dispatcher.run());CHECK(calls==2);session.close();
}
TEST_CASE("typed asynchronous operations own inputs and propagate typed errors") {
    Peer peer([](Socket &s) {auto req=read(s);CHECK(req.payload[2]==4);CHECK(req.payload[3]==1);answer(s,req,{1,42,0,0,0});untilClosed(s);});
    Session session(peer.endpoint(),testOptions());Client client(session);
    std::promise<std::expected<ParamResult,Error>> promise;auto future=promise.get_future();
    client.async([refs=std::vector<Ref>{{0,0,4}}](Client &c){return c.get(refs);},
                 [&promise](std::expected<ParamResult,Error> result){promise.set_value(std::move(result));});
    REQUIRE(future.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    auto result=future.get();REQUIRE(result);CHECK(result->values==std::vector<uint32_t>{42});
}
TEST_CASE("typed and raw completions preserve reply order while a typed parser finishes") {
    std::promise<void> first,answered,decoded,release;
    auto got_first=first.get_future(),both_answered=answered.get_future(),parsed=decoded.get_future();auto allowed=release.get_future().share();
    Peer peer([&](Socket &s) {auto a=read(s);first.set_value();auto b=read(s);answer(s,a,{1,42,0,0,0});answer(s,b);answered.set_value();untilClosed(s);});
    ManualDispatcher dispatcher;auto options=testOptions();options.dispatcher=dispatcher.dispatcher();
    Session session(peer.endpoint(),options);Client client(session);std::vector<int> order;
    client.async([&](Client &c) {const Ref ref{0,0,4};auto value=c.get({&ref,1});decoded.set_value();allowed.wait();return value;},
                 [&](auto r) {REQUIRE(r);order.push_back(1);});
    REQUIRE(got_first.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    session.asyncRequest(3,1,{},[&](auto r) {REQUIRE(r);order.push_back(2);});
    REQUIRE(both_answered.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    REQUIRE(parsed.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {std::lock_guard lock(dispatcher.mutex);CHECK(dispatcher.queue.empty());}
    release.set_value();REQUIRE(dispatcher.run());REQUIRE(dispatcher.run());CHECK(order==std::vector<int>{1,2});
}
TEST_CASE("DISARM bypasses a stalled service and reserves admission when ordinary requests are full") {
    std::promise<void> received;auto ready=received.get_future();
    Peer peer([&](Socket &s) {
        (void)read(s);received.set_value();
        auto disarm=read(s);CHECK(disarm.type==3);CHECK(disarm.payload[2]==1);CHECK(disarm.payload[3]==3);
        answer(s,disarm);untilClosed(s);
    });
    auto options=testOptions();options.request_capacity=1;Session session(peer.endpoint(),options);Client client(session);
    std::promise<void> closed;auto pending=closed.get_future();
    client.async([](Client &c){return c.info();},[&](auto r){CHECK_FALSE(r);closed.set_value();});
    REQUIRE(ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    std::promise<std::expected<SystemResult,Error>> promise;auto done=promise.get_future();
    client.disarmAsync([&](auto r){promise.set_value(std::move(r));});
    REQUIRE(done.wait_for(std::chrono::seconds(2))==std::future_status::ready);CHECK(done.get().has_value());
    session.close();REQUIRE(pending.wait_for(std::chrono::seconds(2))==std::future_status::ready);
}
TEST_CASE("close completes queued typed operations without sending their mutations") {
    std::promise<void> received;auto ready=received.get_future();std::atomic<unsigned> requests=0;
    Peer peer([&](Socket &s) {(void)read(s);++requests;received.set_value();try {for(;;) {(void)read(s);++requests;}}catch(const asio::system_error &) {}});
    Session session(peer.endpoint(),testOptions());Client client(session);std::atomic<unsigned> callbacks=0;
    for(int i=0;i<3;++i) {client.async([](Client &c){return c.system(5);},[&](auto r){CHECK_FALSE(r);CHECK(r.error().kind==ErrorKind::Connection);++callbacks;});}
    REQUIRE(ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);session.close();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(callbacks<3 && std::chrono::steady_clock::now()<deadline) {std::this_thread::yield();}
    CHECK(callbacks==3);CHECK(requests==1);
}
TEST_CASE("requests submitted after close still dispatch one terminal completion") {
    Peer peer([](Socket &s) {untilClosed(s);});ManualDispatcher dispatcher;
    auto options=testOptions();options.dispatcher=dispatcher.dispatcher();Session session(peer.endpoint(),options);session.close();
    unsigned callbacks=0;
    session.asyncRequest(1,5,{},[&](auto r){REQUIRE_FALSE(r);CHECK_FALSE(r.error().outcome_unknown);++callbacks;});
    session.async<int>([]{return 7;},[&](auto r){REQUIRE_FALSE(r);CHECK(r.error().kind==ErrorKind::Connection);++callbacks;});
    CHECK(callbacks==0);REQUIRE(dispatcher.run());REQUIRE(dispatcher.run());CHECK(callbacks==2);
}
TEST_CASE("a default-dispatch completion can destroy the session") {
    Peer peer([](Socket &s) {auto request=read(s);answer(s,request);untilClosed(s);});
    auto session=std::make_unique<Session>(peer.endpoint(),testOptions());
    std::promise<void> submitted;auto allowed=submitted.get_future().share();std::promise<void> done;auto finished=done.get_future();
    auto handle=session->asyncRequest(3,1,{},[&](auto r) {CHECK(r.has_value());allowed.wait();session.reset();done.set_value();});
    submitted.set_value();REQUIRE(finished.wait_for(std::chrono::seconds(2))==std::future_status::ready);handle.cancel();
}
TEST_CASE("cancelled composite operations never advance to another mutation") {
    std::promise<void> received;auto got=received.get_future();std::atomic<int> frames=0;
    Peer peer([&](Socket &s) {(void)read(s);++frames;received.set_value();try {for(;;) {(void)read(s);++frames;}}catch(const asio::system_error &) {}});
    Session session(peer.endpoint(),testOptions());Client client(session);
    std::promise<std::expected<SystemResult,Error>> done;auto future=done.get_future();
    auto handle=client.async([](Client &c){c.system(5);return c.system(6);},[&](auto result){done.set_value(std::move(result));});
    REQUIRE(got.wait_for(std::chrono::seconds(2))==std::future_status::ready);handle.cancel();
    REQUIRE(future.wait_for(std::chrono::seconds(2))==std::future_status::ready);auto result=future.get();
    REQUIRE_FALSE(result);CHECK(result.error().kind==ErrorKind::Cancelled);CHECK(result.error().outcome_unknown);
    session.close();CHECK(frames==1);
}
TEST_CASE("group seqlock and sample ring reject torn values under concurrent overwrite") {
    constexpr uint32_t count=12000;std::atomic<bool> finished=false;std::atomic<uint64_t> reads=0,ring_reads=0;
    Peer peer([](Socket &s) {auto fence=read(s);for(uint32_t n=1;n<=count;++n) {send(s,0x81,sample(n));}answer(s,fence);untilClosed(s);});
    Session session(peer.endpoint(),testOptions());auto buffer=session.subscribeBuffer(0,4,64,32);
    auto check=[](const GroupSample &s) {CHECK(s.count==64);for(auto value:s.values) {CHECK(value==s.device_us);}CHECK(s.epoch==4);};
    std::thread latest([&] {GroupSample s;while(!finished) {if(buffer->latest(s)) {check(s);++reads;}}});
    std::thread ring([&] {GroupSample s;uint64_t previous=0;while(!finished) {if(buffer->pop(s)) {check(s);CHECK(s.device_us>previous);previous=s.device_us;++ring_reads;}}});
    (void)session.request(3,1);finished=true;latest.join();ring.join();
    GroupSample s;REQUIRE(buffer->latest(s));CHECK(s.device_us==count);CHECK(reads>0);CHECK(ring_reads>0);
    buffer->invalidate();CHECK_FALSE(buffer->latest(s));CHECK_FALSE(buffer->pop(s));
}
TEST_CASE("group rings drop oldest and stale epochs cannot enter the buffer") {
    Peer peer([](Socket &s) {auto fence=read(s);send(s,0x81,sample(1,1));send(s,0x81,sample(9,1,3));
        send(s,0x81,sample(2,1));send(s,0x81,sample(3,1));answer(s,fence);untilClosed(s);});
    Session session(peer.endpoint(),testOptions());auto buffer=session.subscribeBuffer(0,4,1,2);(void)session.request(3,1);
    CHECK(buffer->dropped()==1);GroupSample s;REQUIRE(buffer->pop(s));CHECK(s.device_us==2);REQUIRE(buffer->pop(s));CHECK(s.device_us==3);CHECK_FALSE(buffer->pop(s));
}
TEST_CASE("Boot invalidates group buffers before subsequent samples") {
    Peer peer([](Socket &s) {auto fence=read(s);send(s,0x81,sample(1,1));send(s,0xc0,boot());send(s,0x81,sample(2,1));answer(s,fence);untilClosed(s);});
    Session session(peer.endpoint(),testOptions());auto buffer=session.subscribeBuffer(0,4,1);const auto generation=session.generation();
    CHECK_THROWS_AS(session.request(3,1),Error);GroupSample s;CHECK_FALSE(buffer->latest(s));CHECK(session.generation()!=generation);
}
TEST_CASE("a sync request queued across Boot receives its actual reply generation") {
    std::promise<void> first,release,second_started;auto received=first.get_future();auto allowed=release.get_future().share();auto started=second_started.get_future();
    Peer peer([&](Socket &s) {(void)read(s);first.set_value();allowed.wait();send(s,0xc0,boot());auto request=read(s);answer(s,request,{42});untilClosed(s);});
    Session session(peer.endpoint(),testOptions());const auto original=session.generation();
    auto first_call=std::async(std::launch::async,[&] {
        try {(void)session.request(3,1);return false;}catch(const Error &e) {return e.kind==ErrorKind::Protocol;}
    });
    REQUIRE(received.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    auto second_call=std::async(std::launch::async,[&] {second_started.set_value();return session.request(3,1);});
    REQUIRE(started.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));release.set_value();
    CHECK(first_call.get());const auto reply=second_call.get();CHECK(reply.body==Bytes{42});
    CHECK(reply.generation!=original);CHECK(reply.generation==session.generation());
}
TEST_CASE("managed command groups map NaN to finite hold and keepalives stay prefix only") {
    std::promise<void> observed;auto ready=observed.get_future();std::atomic<bool> value_seen=false;
    Peer peer([&](Socket &s) {
        bool keepalive=false;
        while(!keepalive || !value_seen) {
            auto packet=read(s);CHECK(packet.type==0xa0);const auto &b=packet.payload;CHECK(b[1]==7);
            if(b.size()==4) {keepalive=true;}
            else {REQUIRE(b.size()==8);p::wire::Reader r(b.data()+4,4);CHECK(r.readU32Le()==std::bit_cast<uint32_t>(2.5f));value_seen=true;}
        }observed.set_value();untilClosed(s);
    });
    Session session(peer.endpoint(),testOptions());ItemDescriptor descriptor;descriptor.kind=ItemKind::Input;descriptor.value_type=ValueType::F32;
    const uint32_t hold=std::bit_cast<uint32_t>(2.5f);auto command=session.manageCommand(0,7,{descriptor},{&hold,1},60);
    const double nan=std::numeric_limits<double>::quiet_NaN();REQUIRE(command->write({&nan,1}));
    const double inf=std::numeric_limits<double>::infinity();CHECK_FALSE(command->write({&inf,1}));
    REQUIRE(ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    command->stop();CHECK_FALSE(command->write({&nan,1}));CHECK(command->rejected()>=2);
}
TEST_CASE("managed command queue stays coherent across producer backpressure and flushes") {
    std::promise<void> last;auto done=last.get_future();
    Peer peer([&](Socket &s) {
        uint32_t previous=0;
        for(;;) {
            const auto packet=read(s);if(packet.payload.size()==4) {continue;}
            REQUIRE(packet.payload.size()==260);p::wire::Reader r(packet.payload.data()+4,256);
            const auto value=r.readU32Le();CHECK(value>previous);previous=value;
            for(unsigned i=1;i<64;++i) {CHECK(r.readU32Le()==value);}
            if(value==100) {last.set_value();break;}
        }untilClosed(s);
    });
    Session session(peer.endpoint(),testOptions());std::vector<ItemDescriptor> descriptors(64);
    for(auto &d:descriptors) {d.kind=ItemKind::Input;d.value_type=ValueType::U32;}
    std::array<uint32_t,64> values{};auto command=session.manageCommand(0,3,std::move(descriptors),values,200);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    for(uint32_t n=1;n<=100;++n) {
        if(n%10==1) {command->discardQueued();}values.fill(n);
        while(!command->writeBits(values) && std::chrono::steady_clock::now()<deadline) {std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    }
    REQUIRE(done.wait_for(std::chrono::seconds(8))==std::future_status::ready);command->stop();
}
TEST_CASE("periodic ping ownership and mapping reset across Boot") {
    std::atomic<unsigned> pings=0;
    Peer peer([&](Socket &s) {
        const auto origin=monotonicUs()-1000000;
        try {for(;;) {
            const auto packet=read(s);if(packet.type!=2) {continue;}
            p::wire::Reader r(packet.payload.data()+1,2);const auto id=r.readU16Le();
            Bytes payload(19);p::wire::Writer w(payload.data(),payload.size());w.appendU8(1);w.appendU16Le(id);
            const auto device=monotonicUs()-origin;w.appendU64Le(device);w.appendU64Le(device);
            send(s,0x42,payload);if(++pings==2) {send(s,0xc0,boot());}
        }}catch(const asio::system_error &) {}
    });
    auto options=testOptions();options.periodic_time=true;Session session(peer.endpoint(),options);const auto original=session.generation();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while((pings<3 || !session.clockModel().offset_valid) && std::chrono::steady_clock::now()<deadline) {std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    CHECK(pings>=3);CHECK(session.generation()!=original);const auto model=session.clockModel();REQUIRE(model.offset_valid);
    CHECK(std::isfinite(model.map(model.device_reference_us)));CHECK(model.uncertainty(model.device_reference_us)>=model.min_rtt_us/2);
}
