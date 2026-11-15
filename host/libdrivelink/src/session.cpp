#include "drivelink/session.hpp"
#include "drivelink/buffers.hpp"
#include "drivelink/command.hpp"
#include "drivelink/protocol/core_codec.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/scanner.hpp"
#include "drivelink/protocol/time_codec.hpp"
#include <bitset>
#include <atomic>
#include <array>
#include <asio.hpp>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <future>
#include <map>
#include <optional>
#include <algorithm>

namespace drivelink
{
namespace p = protocol;
namespace msg = protocol::defs::msg;
namespace {
std::atomic<uint64_t> next_clock_generation{1};
thread_local std::shared_ptr<detail::Cancellation> service_context;

// A shared state lets a default-dispatch callback destroy its Session without
// leaving the executor's run loop referring to the destroyed owner.
class Executor {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Task> queue;
        bool stopped = false;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    std::thread thread;
  public:
    Executor() : thread([s=state] {
        for (;;) {
            Task task;
            { std::unique_lock lock(s->mutex); s->cv.wait(lock,[&]{return s->stopped || !s->queue.empty();});
              if (s->queue.empty()) { return; } task=std::move(s->queue.front());s->queue.pop_front(); }
            try { task(); } catch (...) { /* An application callback must not kill delivery. */ }
        }
    }) {}
    void post(Task task) {
        { std::lock_guard lock(state->mutex);state->queue.push_back(std::move(task)); }
        state->cv.notify_one();
    }
    void stop() {
        { std::lock_guard lock(state->mutex);state->stopped=true; } state->cv.notify_all();
        if (thread.joinable()) {
            if(thread.get_id()==std::this_thread::get_id()) { thread.detach(); } else { thread.join(); }
        }
    }
    ~Executor() { stop(); }
};
bool retryable(uint8_t family,uint8_t op) {
    return (family==1 && op==1) || family==3 || (family==2 && op==1) ||
        (family==4 && (op==1 || op==3)) || (family==5 && op==6) ||
        (family==6 && (op==4 || op==5)) || (family==9 && op==6);
}
Error contextError(ErrorKind kind,const std::string &why,uint8_t family,uint8_t op,bool uncertain) {
    Error e(kind,why);e.family=family;e.op=op;e.has_request_context=true;e.outcome_unknown=uncertain;return e;
}
ClockModel aged(ClockModel model) {
    const auto now=monotonicUs();model.age_us=now>=model.updated_us?now-model.updated_us:0;
    if(model.age_us>5000000) {model.offset_valid=false;model.drift_valid=false;}return model;
}
}
void RequestHandle::cancel() const {
    if(!m_state) { return; }
    std::function<void()> interrupt;
    std::function<void(Error)> terminate;
    { std::lock_guard lock(m_state->mutex);
      if(m_state->done || m_state->cancelled.exchange(true)) { return; }
      interrupt=m_state->interrupt;terminate=m_state->terminate; }
    if(interrupt) { interrupt(); }
    if(terminate) {
        Error e(ErrorKind::Cancelled,"operation cancelled");
        {std::lock_guard lock(m_state->mutex);e.outcome_unknown=m_state->mutation;e.family=m_state->family;e.op=m_state->op;e.has_request_context=m_state->has_context;}
        terminate(e);
    }
}

struct Session::Impl
{
    SessionOptions options;
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    asio::ip::tcp::socket tcp{io};
    asio::serial_port serial{io};
    bool use_tcp=false;
    p::Scanner scanner{p::kMaxPayload};
    asio::steady_timer stale{io}, periodic{io}, command_tick{io};
    std::array<uint8_t,2048> input{};
    std::thread thread;
    Executor callbacks, services;
    std::mutex close_mutex;
    std::recursive_mutex sync_mutex;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> closed{false};
    uint16_t max_payload=p::kMaxPayload;
    std::string failure="session closed";
    size_t admitted=0, service_count=0, pending_streams=0;
    std::shared_ptr<std::atomic<size_t>> callback_count=std::make_shared<std::atomic<size_t>>(0);
    std::mutex delivery_mutex;
    std::deque<std::shared_ptr<detail::DeliverySlot>> deliveries;
    std::vector<std::weak_ptr<detail::Cancellation>> service_states;
    std::deque<Frame> events;
    uint64_t dropped=0;
    struct Observer { std::recursive_mutex mutex;bool active=true;EventHandler handler; };
    struct Mailbox {
        std::mutex mutex;
        std::deque<Frame> queue;
        std::map<uint64_t,std::shared_ptr<Observer>> observers;
        uint64_t dropped=0,sequence=0;
        bool scheduled=false;
    };
    std::shared_ptr<Mailbox> mailbox=std::make_shared<Mailbox>();
    struct Pending {
        uint8_t kind=0,family=0,op=0,seq=0;
        bool read_only=false,mutation=false,sent=false,completed=false;
        unsigned attempts=0;
        Bytes body;
        std::shared_ptr<Bytes> bytes;
        asio::steady_timer timer;
        Completion<Bytes> finish;
        std::shared_ptr<detail::Cancellation> cancellation;
        std::shared_ptr<detail::Cancellation> owner;
        std::shared_ptr<size_t> batch_remaining;
        explicit Pending(asio::io_context &io_) : timer(io_) {}
    };
    std::map<uint8_t,std::shared_ptr<Pending>> pending;
    std::map<uint8_t,std::shared_ptr<GroupBuffer>> buffers;
    std::map<uint8_t,std::weak_ptr<CommandGroup>> commands;
    std::array<bool,256> retired{};
    unsigned sequence=0;
    struct Write { std::shared_ptr<Bytes> bytes;std::shared_ptr<Pending> request;bool stream=false;bool ping=false;std::weak_ptr<CommandGroup> command={};bool managed=false;uint64_t command_generation=0; };
    std::deque<Write> writes;
    struct Ping {
        asio::steady_timer timer;
        TimeSample sample;
        std::shared_ptr<Bytes> bytes;
        Completion<TimeSample> finish;
        bool sent=false;
        std::vector<Completion<TimeSample>> waiters;
        explicit Ping(asio::io_context &i) : timer(i) {}
    };
    std::shared_ptr<Ping> ping;
    std::bitset<65536> ping_retired;
    uint32_t ping_sequence=0;
    uint64_t boot_generation=next_clock_generation.fetch_add(1), started_us=monotonicUs();
    ClockEstimator estimator;
    ClockModel clock;

    Impl(const std::string &endpoint,SessionOptions opts) : options(std::move(opts)) {
        if(options.timeout.count()<=0 || options.timeout>std::chrono::seconds(60) ||
           options.read_retries>5 || !options.event_capacity || options.event_capacity>65536 ||
           !options.request_capacity || options.request_capacity>128) {
            throw Error(ErrorKind::Usage,"invalid session limits");
        }
        asio::error_code ec;
        if (endpoint.starts_with("tcp://"))
        {
            use_tcp = true;
            const auto colon = endpoint.rfind(':');
            if (colon == std::string::npos || colon <= 6)
            {
                throw Error(ErrorKind::Usage, "expected tcp://numeric-ip:port");
            }
            auto address = asio::ip::make_address(endpoint.substr(6, colon - 6), ec);
            if (ec)
            {
                throw Error(ErrorKind::Usage, "TCP address must be numeric");
            }
            const auto port = endpoint.substr(colon + 1);
            size_t used = 0;
            unsigned long number = 0;
            try
            {
                number = std::stoul(port, &used);
            }
            catch (...)
            {
                throw Error(ErrorKind::Usage, "invalid TCP port");
            }
            if (used != port.size() || !number || number > 65535)
            {
                throw Error(ErrorKind::Usage, "invalid TCP port");
            }
            // A timer bounds connect as well as subsequent requests.
            asio::steady_timer deadline(io, options.timeout);
            deadline.async_wait(
                [&](auto error)
                {
                    if (!error)
                    {
                        tcp.cancel();
                    }
                });
            tcp.async_connect({address, static_cast<uint16_t>(number)},
                              [&](auto error)
                              {
                                  ec = error;
                                  deadline.cancel();
                              });
            io.run();
            io.restart();
        }
        else if (endpoint.starts_with("serial://") && endpoint.size() > 9)
        {
            serial.open(endpoint.substr(9), ec);
#ifdef _WIN32
            // Some USB-UART drivers briefly retain exclusive ownership after
            // the preceding CLI exits. Retrying open is safe: no request has
            // been sent, and this wait is bounded by the session timeout.
            const auto open_deadline = std::chrono::steady_clock::now() + options.timeout;
            auto transient_open_error = [&]
            {
                // Serial CreateFile errors are Win32 codes, not Asio's
                // WSAEACCES socket error (asio::error::access_denied).
                return ec.category() == asio::error::get_system_category() &&
                       (ec.value() == ERROR_ACCESS_DENIED || ec.value() == ERROR_FILE_NOT_FOUND);
            };
            while (transient_open_error() && std::chrono::steady_clock::now() < open_deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                serial.open(endpoint.substr(9), ec);
            }
#endif
            if (!ec)
            {
                serial.set_option(asio::serial_port::baud_rate(options.baud), ec);
            }
            if (!ec)
            {
                serial.set_option(asio::serial_port::character_size(8), ec);
            }
            if (!ec)
            {
                serial.set_option(asio::serial_port::parity(asio::serial_port::parity::none), ec);
            }
            if (!ec)
            {
                serial.set_option(asio::serial_port::stop_bits(asio::serial_port::stop_bits::one), ec);
            }
#ifndef _WIN32
            if (!ec)
            {
                serial.set_option(asio::serial_port::flow_control(asio::serial_port::flow_control::none), ec);
            }
#else
            // Asio's Windows open() already disables flow control, DTR and
            // RTS. Its flow_control(none) option re-enables both lines, which
            // toggles ESP32 auto-reset circuitry on every CLI invocation.
            if (!ec)
            {
                // Asio 1.30.2 waits for a 1 ms inter-byte gap or a full read
                // buffer. Continuous streams can therefore hide a ping reply
                // in a 2048-byte read for tens of ms. Return on the first byte
                // instead (Win32 COMMTIMEOUTS documented special mode).
                // The constant only bounds idle reads; bytes return at once.
                COMMTIMEOUTS timeouts{};
                if (!::GetCommTimeouts(serial.native_handle(), &timeouts))
                {
                    ec = asio::error_code(static_cast<int>(::GetLastError()), asio::error::get_system_category());
                }
                else
                {
                    timeouts.ReadIntervalTimeout = MAXDWORD;
                    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
                    timeouts.ReadTotalTimeoutConstant = 1000;
                    if (!::SetCommTimeouts(serial.native_handle(), &timeouts))
                    {
                        ec = asio::error_code(static_cast<int>(::GetLastError()), asio::error::get_system_category());
                    }
                }
            }
#endif
        }
        else
        {
            throw Error(ErrorKind::Usage, "expected tcp://numeric-ip:port or serial://device");
        }
        if (ec)
        {
            throw Error(ErrorKind::Connection, "open: " + ec.message());
        }

        work.emplace(asio::make_work_guard(io));
        read();
        if(options.periodic_time) { schedulePing(std::chrono::milliseconds(100)); }
        thread=std::thread([this]{io.run();});
    }
    void dispatch(Task task) {
        auto dispatcher=options.dispatcher;
        callbacks.post([dispatcher=std::move(dispatcher),task=std::move(task)]() mutable {
            if(dispatcher) { dispatcher(std::move(task)); } else { task(); }
        });
    }
    void drainDeliveries() {
        // delivery_mutex is held while posting, preserving order even when a
        // service parser and I/O finish on different threads.
        while(!deliveries.empty() && deliveries.front()->ready) {
            auto slot=std::move(deliveries.front());deliveries.pop_front();
            if(slot->task) {dispatch(std::move(slot->task));}
        }
    }
    void reserveReply(const std::shared_ptr<detail::Cancellation> &owner) {
        if(!owner) {return;}std::lock_guard lock(owner->mutex);if(owner->done) {return;}
        auto slot=std::make_shared<detail::DeliverySlot>();
        std::lock_guard delivery(delivery_mutex);deliveries.push_back(slot);owner->reply_slot=std::move(slot);
    }
    void releaseReply(const std::shared_ptr<detail::Cancellation> &owner) {
        if(!owner) {return;}std::lock_guard lock(owner->mutex);
        if(auto slot=std::move(owner->reply_slot)) {std::lock_guard delivery(delivery_mutex);slot->ready=true;drainDeliveries();}
    }
    void deliver(Task task,const std::shared_ptr<detail::Cancellation> &owner={}) {
        std::shared_ptr<detail::DeliverySlot> slot;
        if(owner) {std::lock_guard lock(owner->mutex);slot=std::move(owner->reply_slot);}
        std::lock_guard delivery(delivery_mutex);
        if(!slot) {slot=std::make_shared<detail::DeliverySlot>();deliveries.push_back(slot);}
        slot->task=std::move(task);slot->ready=true;drainDeliveries();
    }
    void complete(const std::shared_ptr<Pending> &r,std::expected<Bytes,Error> result,bool retire=false) {
        if(r->completed) { return; }
        r->completed=true;r->timer.cancel();
        {std::lock_guard lock(r->cancellation->mutex);r->cancellation->done=true;r->cancellation->interrupt={};}
        if(retire) { retired[r->seq]=true; }
        if(auto it=pending.find(r->seq);it!=pending.end() && it->second==r) { pending.erase(it); }
        { std::lock_guard lock(mutex);--admitted; }
        if(!r->batch_remaining || --*r->batch_remaining==0) {reserveReply(r->owner);}
        r->finish(std::move(result));
    }
    Error error(const std::shared_ptr<Pending> &r,ErrorKind kind,const std::string &why) {
        return contextError(kind,why,r->family,r->op,r->mutation && r->sent);
    }
    void fail(const std::string &why) {
        { std::lock_guard lock(mutex);closed=true;failure=why; }
        cv.notify_all();
        for(auto &[id,buffer]:buffers) {(void)id;buffer->invalidate();}buffers.clear();
        for(auto &[id,weak]:commands) {(void)id;if(auto command=weak.lock()) {command->stop();}}commands.clear();
        Frame lost{0,{}};{std::lock_guard lock(mutex);lost.generation=boot_generation;}notify(std::move(lost));
        // Completing removes entries, so retain ownership of the first one.
        while(!pending.empty()) { auto r=pending.begin()->second;complete(r,std::unexpected(error(r,ErrorKind::Connection,why)),true); }
        if(ping) { finishPing(std::unexpected(Error(ErrorKind::Connection,why)),true); }
        asio::error_code ec;tcp.close(ec);serial.close(ec);stale.cancel();periodic.cancel();command_tick.cancel();writes.clear();
        if(work) {work->reset();}
    }
    void begin(const std::shared_ptr<Pending> &r) {
        if(closed) { complete(r,std::unexpected(error(r,ErrorKind::Connection,failure)));return; }
        if(r->cancellation->cancelled) { complete(r,std::unexpected(error(r,ErrorKind::Cancelled,"request cancelled")));return; }
        unsigned tried=0;
        while(tried<256 && (retired[sequence%256] || pending.contains(static_cast<uint8_t>(sequence%256)))) { ++sequence;++tried; }
        if(tried==256) { complete(r,std::unexpected(error(r,ErrorKind::Connection,"sequence IDs exhausted; reconnect")));return; }
        r->seq=static_cast<uint8_t>(sequence++%256);
        const bool core=r->kind!=msg::kDiagQuery;
        Bytes payload{static_cast<uint8_t>(core?p::core_wire::kVersion:3),r->seq};
        if(core) { payload.push_back(r->family); }payload.push_back(r->op);
        payload.insert(payload.end(),r->body.begin(),r->body.end());
        if(payload.size()>maxPayload()) { complete(r,std::unexpected(error(r,ErrorKind::Usage,"request exceeds device payload cap")));return; }
        r->bytes=std::make_shared<Bytes>(p::frameSize(payload.size()));
        p::encodeFrame(r->bytes->data(),r->bytes->size(),r->kind,payload.data(),payload.size());
        pending.emplace(r->seq,r);
        if(writes.size()>=options.request_capacity+8) { complete(r,std::unexpected(error(r,ErrorKind::Connection,"TX queue full")));return; }
        Write queued{r->bytes,r,false,false};
        if(r->kind==msg::kDisarm && !writes.empty()) {writes.insert(std::next(writes.begin()),std::move(queued));}
        else {writes.push_back(std::move(queued));}
        if(writes.size()==1) { write(); }
        r->timer.expires_after(options.timeout);
        r->timer.async_wait([this,r](auto ec) {
            if(ec || r->completed) { return; }
            retired[r->seq]=true;pending.erase(r->seq);
            // A timed-out frame still queued for TX must never be sent later.
            if(r->read_only && r->attempts++<options.read_retries && !r->cancellation->cancelled) {
                r->bytes.reset();begin(r);
            } else { complete(r,std::unexpected(error(r,ErrorKind::Timeout,"request timed out; query device after an uncertain mutation")),true); }
        });
    }
    uint16_t maxPayload() const { std::lock_guard lock(mutex);return max_payload; }
    RequestHandle submit(uint8_t kind,uint8_t family,uint8_t op,Bytes body,Completion<Bytes> finish,
                         std::shared_ptr<detail::Cancellation> owner={}) {
        auto r=std::make_shared<Pending>(io);r->kind=kind;r->family=family;r->op=op;r->body=std::move(body);
        r->read_only=kind==msg::kDiagQuery?(op==1 || op==2):retryable(family,op);
        r->mutation=!r->read_only && !(family==7 && op==3);r->finish=std::move(finish);
        r->owner=std::move(owner);
        r->cancellation=std::make_shared<detail::Cancellation>();
        { std::lock_guard lock(mutex);
          if(admitted>=options.request_capacity+(kind==msg::kDisarm?1U:0U)) { throw Error(ErrorKind::Usage,"request capacity exhausted"); }
          ++admitted; }
        if(closed) {
            { std::lock_guard lock(mutex);--admitted; }
            r->finish(std::unexpected(contextError(ErrorKind::Connection,"session closed",family,op,false)));
        } else {
            // Serialize cancellation submission with close, so no handler is
            // posted to an io_context which has already finished.
            std::lock_guard closing(close_mutex);
            if(closed) {
                { std::lock_guard lock(mutex);--admitted; }
                r->finish(std::unexpected(contextError(ErrorKind::Connection,"session closed",family,op,false)));
            } else {
                r->cancellation->interrupt=[this,weak=std::weak_ptr<Pending>(r)] {
                    std::lock_guard guard(close_mutex);if(closed) { return; }
                    asio::post(io,[this,weak] { if(auto r_=weak.lock();r_ && !r_->completed) {
                        complete(r_,std::unexpected(error(r_,ErrorKind::Cancelled,"request cancelled")),true);
                    }});
                };
                asio::post(io,[this,r]{begin(r);});
            }
        }
        return RequestHandle(r->cancellation);
    }
    Bytes exchange(uint8_t kind,uint8_t family,uint8_t op,std::span<const uint8_t> body) {
        std::unique_lock calls(sync_mutex,std::defer_lock);if(kind!=msg::kDisarm) {calls.lock();}
        if(thread.get_id()==std::this_thread::get_id()) { throw Error(ErrorKind::Usage,"sync request on I/O thread"); }
        if(service_context && service_context->cancelled) { throw Error(ErrorKind::Cancelled,"operation cancelled"); }
        releaseReply(service_context);
        if(service_context) {
            std::lock_guard lock(service_context->mutex);
            service_context->family=family;service_context->op=op;service_context->has_context=true;
            if(!(kind==msg::kDiagQuery?(op==1 || op==2):retryable(family,op)) && !(family==7 && op==3)) {service_context->mutation=true;}
        }
        auto promise=std::make_shared<std::promise<std::expected<Bytes,Error>>>();auto future=promise->get_future();
        auto handle=submit(kind,family,op,Bytes(body.begin(),body.end()),[promise](auto result){promise->set_value(std::move(result));},service_context);
        if(service_context) {
            std::lock_guard lock(service_context->mutex);
            service_context->interrupt=[handle]{handle.cancel();};
            if(service_context->cancelled) { handle.cancel(); }
            // Queued mutations can still reach the device; conservatively
            // retain uncertainty across the entire composite operation.
            if(!(kind==msg::kDiagQuery?(op==1 || op==2):retryable(family,op)) && !(family==7 && op==3)) {
                service_context->mutation=true;
            }
        }
        auto result=future.get();
        if(service_context) { std::lock_guard lock(service_context->mutex);service_context->interrupt={}; }
        if(!result) { throw result.error(); }return std::move(*result);
    }
    void finishPing(std::expected<TimeSample,Error> result,bool retire=false) {
        auto current=std::move(ping);if(!current) { return; }current->timer.cancel();
        if(retire) { ping_retired[current->sample.id]=true; }
        current->finish(result);
        for(auto &waiter:current->waiters) {waiter(result);}
    }
    void startPing(Completion<TimeSample> finish) {
        if(closed) { finish(std::unexpected(Error(ErrorKind::Connection,"session closed")));return; }
        if(ping) {ping->waiters.push_back(std::move(finish));return;}
        unsigned tried=0;while(tried<65536 && ping_retired[ping_sequence%65536]) {++ping_sequence;++tried;}
        if(tried==65536) { finish(std::unexpected(Error(ErrorKind::Connection,"time ping IDs exhausted; reconnect")));return; }
        auto current=std::make_shared<Ping>(io);current->sample.id=static_cast<uint16_t>(ping_sequence++%65536);current->finish=std::move(finish);
        std::array<uint8_t,p::time_wire::kRequestBytes> payload{};p::time_wire::request(payload.data(),payload.size(),current->sample.id);
        current->bytes=std::make_shared<Bytes>(p::frameSize(payload.size()));
        p::encodeFrame(current->bytes->data(),current->bytes->size(),msg::kTimeRequest,payload.data(),payload.size());
        ping=current;
        if(writes.size()>=options.request_capacity+8) { finishPing(std::unexpected(Error(ErrorKind::Connection,"TX queue full")));return; }
        writes.push_back({current->bytes,{},false,true});if(writes.size()==1) {write();}
        current->timer.expires_after(options.timeout);current->timer.async_wait([this,current](auto ec) {
            if(!ec && ping==current) { finishPing(std::unexpected(Error(ErrorKind::Timeout,"time ping timed out")),true); }
        });
    }
    void schedulePing(std::chrono::milliseconds delay) {
        periodic.expires_after(delay);periodic.async_wait([this](auto ec) {
            if(ec || closed) {return;}
            if(!ping) {startPing([](auto){});}
            schedulePing(monotonicUs()-started_us<3000000?std::chrono::milliseconds(200):std::chrono::milliseconds(1000));
        });
    }
    void notify(Frame frame) {
        bool schedule=false;
        if(frame.type) {std::lock_guard lock(mutex);
          if(events.size()==options.event_capacity) {events.pop_front();++dropped;}events.push_back(frame);}
        if(frame.type>=p::defs::kSubGroupChannelFirst && frame.type<p::defs::kSubGroupChannelFirst+p::defs::kSubGroupChannelCount) {cv.notify_all();return;}
        {std::lock_guard lock(mailbox->mutex);
          if(!mailbox->observers.empty()) {
              if(mailbox->queue.size()==options.event_capacity) {mailbox->queue.pop_front();++mailbox->dropped;}
              mailbox->queue.push_back(std::move(frame));
              if(!mailbox->scheduled) {mailbox->scheduled=true;schedule=true;}
          } }
        cv.notify_all();
        if(schedule) { dispatch([box=mailbox]{drainNotifications(box);}); }
    }
    static void drainNotifications(const std::shared_ptr<Mailbox> &box) {
        std::deque<Frame> batch;std::vector<std::shared_ptr<Observer>> targets;
        {std::lock_guard lock(box->mutex);batch.swap(box->queue);box->scheduled=false;
         for(auto &[id,observer]:box->observers) {(void)id;targets.push_back(observer);} }
        for(const auto &frame:batch) {
            for(auto &observer:targets) {std::lock_guard lock(observer->mutex);if(observer->active) {try {observer->handler(frame);}catch(...) {}}}
        }
    }
    void commandTick() {
        if(closed || commands.empty()) {return;}
        command_tick.expires_after(std::chrono::milliseconds(1));command_tick.async_wait([this](auto ec) {
            if(ec || closed) {return;}
            for(auto it=commands.begin();it!=commands.end();) {
                auto command=it->second.lock();
                if(!command || !command->active()) {it=commands.erase(it);continue;}
                Bytes payload;uint64_t generation=0;
                if(writes.size()<options.request_capacity+8 && command->poll(monotonicUs(),payload,generation)) {
                    auto bytes=std::make_shared<Bytes>(p::frameSize(payload.size()));
                    p::encodeFrame(bytes->data(),bytes->size(),static_cast<uint8_t>(p::defs::kCmdGroupChannelFirst+it->first),payload.data(),payload.size());
                    Write queued{bytes,{},false,false,command,true,generation};
                    if(writes.empty()) {writes.push_back(std::move(queued));write();}
                    else {
                        auto position=std::next(writes.begin());
                        while(position!=writes.end() && (position->managed || (position->request && position->request->kind==msg::kDisarm))) {++position;}
                        writes.insert(position,std::move(queued));
                    }
                }++it;
            }commandTick();
        });
    }
    void receive(const p::Scanner::Frame &f) {
        if(f.msg_type==msg::kDiagEvent && f.len==17 && f.payload[0]==3 && f.payload[3]==4) {
            {std::lock_guard lock(mutex);boot_generation=next_clock_generation.fetch_add(1);clock={};}
            estimator.reset();started_us=monotonicUs();
            for(auto &[id,buffer]:buffers) {(void)id;buffer->invalidate();}buffers.clear();
            for(auto &[id,weak]:commands) {(void)id;if(auto command=weak.lock()) {command->stop();}}commands.clear();
            if(ping) {finishPing(std::unexpected(Error(ErrorKind::Protocol,"device reboot during ping")),true);}
            while(!pending.empty()) {auto r=pending.begin()->second;complete(r,std::unexpected(error(r,ErrorKind::Protocol,"device reboot during request")),true);}
        }
        if(f.msg_type==msg::kTimeResponse) {
            if(!ping || !ping->sent || f.len<3) {return;}
            p::wire::Reader reader(f.payload+1,2);if(reader.readU16Le()!=ping->sample.id) {return;}
            p::time_wire::Response reply;
            if(!p::time_wire::decodeResponse(f.payload,f.len,reply)) {finishPing(std::unexpected(Error(ErrorKind::Protocol,"invalid time response")),true);return;}
            auto sample=ping->sample;sample.esp_rx_us=reply.rx_us;sample.esp_tx_us=reply.tx_us;sample.host_rx_us=monotonicUs();
            estimator.add(sample);{std::lock_guard lock(mutex);clock=estimator.model(sample.host_rx_us);}
            finishPing(sample);return;
        }
        if((f.msg_type==msg::kCoreResponse || f.msg_type==msg::kDiagQueryResponse) && f.len>=2) {
            auto it=pending.find(f.payload[1]);if(it==pending.end()) {return;}auto r=it->second;
            const bool core=r->kind!=msg::kDiagQuery;
            if(f.msg_type!=(core?msg::kCoreResponse:msg::kDiagQueryResponse)) {return;}
            if(f.len<(core?6U:4U) || f.payload[0]!=(core?p::core_wire::kVersion:3)) {
                complete(r,std::unexpected(error(r,ErrorKind::Protocol,"incompatible protocol version or truncated response header")),true);return;
            }
            if(core?(f.payload[2]!=r->family || f.payload[3]!=r->op):f.payload[2]!=r->op) {return;}
            complete(r,Bytes(f.payload,f.payload+f.len));return;
        }
        if(p::isStream(f.msg_type) || p::isEvent(f.msg_type)) {
            Frame frame{f.msg_type,Bytes(f.payload,f.payload+f.len)};
            {std::lock_guard lock(mutex);frame.clock=clock;frame.generation=boot_generation;}
            const auto now=monotonicUs();frame.clock.age_us=now>=frame.clock.updated_us?now-frame.clock.updated_us:0;
            if(frame.clock.age_us>5000000) {frame.clock.offset_valid=false;frame.clock.drift_valid=false;}
            if(frame.type>=p::defs::kSubGroupChannelFirst && frame.type<p::defs::kSubGroupChannelFirst+p::defs::kSubGroupChannelCount) {
                const auto id=static_cast<uint8_t>(frame.type-p::defs::kSubGroupChannelFirst);
                if(auto it=buffers.find(id);it!=buffers.end()) {it->second->receive(frame);}
            }
            notify(std::move(frame));
        }
    }
    void read() {
        auto handler=[this](auto ec,size_t n) {
            if(ec) {if(!closed) {fail(ec.message());}return;}
            size_t offset=0;
            while(offset<n) {
                size_t space=0;auto *dst=scanner.writable(space);const auto count=std::min(space,n-offset);
                std::copy_n(input.data()+offset,count,dst);scanner.commit(count);offset+=count;
                p::Scanner::Frame f;while(scanner.next(f)) {receive(f);}
                if(!count) {fail("scanner made no progress");return;}
            }
            stale.expires_after(std::chrono::milliseconds(250));stale.async_wait([this](auto error_) {
                if(!error_) {scanner.dropStale();p::Scanner::Frame f;while(scanner.next(f)) {receive(f);}}
            });
            if(!closed) {read();}
        };
        if(use_tcp) {tcp.async_read_some(asio::buffer(input),handler);}else {serial.async_read_some(asio::buffer(input),handler);}
    }
    void write() {
        while(!writes.empty()) {
            auto &w=writes.front();
            if(w.managed) {auto command=w.command.lock();if(!command || !command->active() || command->queueGeneration()!=w.command_generation) {writes.pop_front();continue;}}
            if((w.request && (w.request->completed || w.request->bytes!=w.bytes)) ||
               (w.ping && (!ping || ping->bytes!=w.bytes))) {writes.pop_front();continue;}
            if(w.request) {w.request->sent=true;}
            if(w.ping) {ping->sent=true;ping->sample.host_tx_us=monotonicUs();ping->sample.boot_generation=boot_generation;}
            auto bytes=w.bytes;
            auto handler=[this,bytes](auto ec,size_t) {
                if(closed) {return;}if(ec) {fail(ec.message());return;}
                if(writes.front().stream) {std::lock_guard lock(mutex);--pending_streams;}
                writes.pop_front();if(!writes.empty()) {write();}
            };
            if(use_tcp) {asio::async_write(tcp,asio::buffer(*bytes),handler);}else {asio::async_write(serial,asio::buffer(*bytes),handler);}return;
        }
    }
};

Session::Session(const std::string &endpoint,SessionOptions options) : m_impl(std::make_unique<Impl>(endpoint,std::move(options))) {}
Session::~Session() {
    close();m_impl->services.stop();
    auto box=m_impl->mailbox;std::vector<uint64_t> ids;
    {std::lock_guard lock(box->mutex);for(auto &[id,observer]:box->observers) {(void)observer;ids.push_back(id);}}
    for(auto id:ids) {unobserve(id);}m_impl->callbacks.stop();
}
void Session::close() {
    auto &d=*m_impl;
    std::vector<std::shared_ptr<detail::Cancellation>> jobs;
    {std::lock_guard closing(d.close_mutex);
     if(!d.closed.exchange(true)) {asio::post(d.io,[&d]{d.fail("session closed");});}
     {std::lock_guard lock(d.mutex);for(auto &weak:d.service_states) {if(auto s=weak.lock()) {jobs.push_back(s);}}} }
    for(auto &state:jobs) {
        std::function<void(Error)> terminate;
        {std::lock_guard lock(state->mutex);if(state->done) {continue;}state->cancelled=true;terminate=state->terminate;}
        Error e(ErrorKind::Connection,"session closed");
        {std::lock_guard lock(state->mutex);e.outcome_unknown=state->mutation;e.family=state->family;e.op=state->op;e.has_request_context=state->has_context;}
        if(terminate) {terminate(e);}
    }
    // Only one closer joins; the I/O thread never takes close_mutex.
    std::lock_guard closing(d.close_mutex);if(d.thread.joinable()) {d.thread.join();}
    d.cv.notify_all();
}
Reply Session::request(uint8_t family,uint8_t op,std::span<const uint8_t> body) {
    const auto before=generation();
    auto data=m_impl->exchange(family==1 && op==3?msg::kDisarm:msg::kCoreCommand,family,op,body);
    std::lock_guard lock(m_impl->mutex);
    if(m_impl->boot_generation!=before) {throw contextError(ErrorKind::Protocol,"boot changed before reply was consumed",family,op,!retryable(family,op) && !(family==7 && op==3));}
    return {data[4],data[5],Bytes(data.begin()+6,data.end()),aged(m_impl->clock),before};
}
RequestHandle Session::asyncRequest(uint8_t family,uint8_t op,Bytes body,Completion<Reply> completion) {
    auto reservation=callbackSlot(family==1 && op==3);
    return m_impl->submit(family==1 && op==3?msg::kDisarm:msg::kCoreCommand,family,op,std::move(body),
        [this,reservation=std::move(reservation),completion=std::move(completion)](std::expected<Bytes,Error> data) mutable {
            std::expected<Reply,Error> result=data?std::expected<Reply,Error>(Reply{(*data)[4],(*data)[5],Bytes(data->begin()+6,data->end())}):std::unexpected(data.error());
            if(result) {std::lock_guard lock(m_impl->mutex);result->clock=aged(m_impl->clock);result->generation=m_impl->boot_generation;}
            m_impl->deliver([reservation=std::move(reservation),completion=std::move(completion),result=std::move(result)]() mutable {completion(std::move(result));});
        });
}
Bytes Session::diagRequest(uint8_t op,std::span<const uint8_t> body) {
    auto data=m_impl->exchange(msg::kDiagQuery,0,op,body);
    if(data[3]) {throw DeviceError("diagnostic query rejected",data[3],0,0,op);}return Bytes(data.begin()+4,data.end());
}
RequestHandle Session::asyncDiagRequest(uint8_t op,Bytes body,Completion<Bytes> completion) {
    auto reservation=callbackSlot();
    return m_impl->submit(msg::kDiagQuery,0,op,std::move(body),[this,op,reservation=std::move(reservation),completion=std::move(completion)](std::expected<Bytes,Error> data) mutable {
        if(data) {
            if((*data)[3]) {data=std::unexpected(DeviceError("diagnostic query rejected",(*data)[3],0,0,op));}
            else {data=Bytes(data->begin()+4,data->end());}
        }
        m_impl->deliver([reservation=std::move(reservation),completion=std::move(completion),data=std::move(data)]() mutable {completion(std::move(data));});
    });
}
TimeSample Session::timePing() {
    std::lock_guard calls(m_impl->sync_mutex);
    if(m_impl->thread.get_id()==std::this_thread::get_id()) {throw Error(ErrorKind::Usage,"sync ping on I/O thread");}
    auto promise=std::make_shared<std::promise<std::expected<TimeSample,Error>>>();auto future=promise->get_future();
    m_impl->releaseReply(service_context);
    {std::lock_guard closing(m_impl->close_mutex);
     if(m_impl->closed) {throw Error(ErrorKind::Connection,"session closed");}
     asio::post(m_impl->io,[d=m_impl.get(),promise,owner=service_context]{d->startPing([d,promise,owner](auto result){d->reserveReply(owner);promise->set_value(std::move(result));});});}
    auto result=future.get();if(!result) {throw result.error();}return *result;
}
RequestHandle Session::asyncTimePing(Completion<TimeSample> completion) {
    return async<TimeSample>([this]{return timePing();},std::move(completion));
}
std::vector<Reply> Session::otaWriteBatch(std::span<const Bytes> bodies) {
    auto &d=*m_impl;
    std::lock_guard calls(d.sync_mutex);
    if(bodies.empty() || bodies.size()>4) {throw Error(ErrorKind::Usage,"OTA window must be 1..4");}
    for(const auto &body:bodies) {
        p::wire::Reader r(body.data(),body.size());(void)r.readU32Le();const auto n=r.readU16Le();
        if(!r.ok() || !n || n>512 || n!=r.remaining() || body.size()+4>maxPayload()) {
            throw contextError(ErrorKind::Usage,"invalid OTA WRITE body",9,2,false);
        }
    }

    return requestBatch(9,2,bodies);
}
std::vector<Reply> Session::requestBatch(uint8_t family,uint8_t op,std::span<const Bytes> bodies) {
    auto &d=*m_impl;std::lock_guard calls(d.sync_mutex);
    if(bodies.empty() || bodies.size()>4 || (!retryable(family,op) && !(family==9 && op==2))) {
        throw contextError(ErrorKind::Usage,"batch requires 1..4 read pages or OTA WRITE chunks",family,op,false);
    }
    for(const auto &body:bodies) {if(body.size()+4>maxPayload()) {throw contextError(ErrorKind::Usage,"batch page exceeds payload",family,op,false);}}
    if(service_context && service_context->cancelled) {throw Error(ErrorKind::Cancelled,"operation cancelled");}
    d.releaseReply(service_context);
    std::vector<std::future<std::expected<Bytes,Error>>> futures;
    std::vector<std::shared_ptr<Impl::Pending>> requests;
    std::vector<RequestHandle> handles;
    auto remaining=std::make_shared<size_t>(bodies.size());
    for(const auto &body:bodies) {
        auto r=std::make_shared<Impl::Pending>(d.io);r->kind=msg::kCoreCommand;r->family=family;r->op=op;r->body=body;
        r->read_only=retryable(family,op);r->mutation=!r->read_only;r->cancellation=std::make_shared<detail::Cancellation>();
        r->owner=service_context;r->batch_remaining=remaining;
        auto promise=std::make_shared<std::promise<std::expected<Bytes,Error>>>();futures.push_back(promise->get_future());
        r->finish=[promise](auto result){promise->set_value(std::move(result));};
        r->cancellation->interrupt=[&d,weak=std::weak_ptr<Impl::Pending>(r)] {
            std::lock_guard closing(d.close_mutex);if(d.closed) {return;}
            asio::post(d.io,[&d,weak] {if(auto request=weak.lock();request && !request->completed) {
                d.complete(request,std::unexpected(d.error(request,ErrorKind::Cancelled,"batch cancelled")),true);
            }});
        };
        handles.emplace_back(r->cancellation);requests.push_back(std::move(r));
    }
    {std::lock_guard closing(d.close_mutex);
     if(d.closed) {throw contextError(ErrorKind::Connection,"session closed",family,op,false);}
     {std::lock_guard lock(d.mutex);if(d.admitted+bodies.size()>d.options.request_capacity) {throw Error(ErrorKind::Usage,"batch request capacity exhausted");}d.admitted+=bodies.size();}
     asio::post(d.io,[&d,requests] {
        size_t available=0;for(size_t i=0;i<256;++i) {available+=!d.retired[i] && !d.pending.contains(static_cast<uint8_t>(i));}
        if(available<requests.size() || d.writes.size()+requests.size()>d.options.request_capacity+8) {
            for(const auto &r:requests) {d.complete(r,std::unexpected(d.error(r,ErrorKind::Connection,"batch resources exhausted; reconnect")));}return;
        }
        for(const auto &r:requests) {d.begin(r);}
     });}
    if(service_context) {
        std::lock_guard lock(service_context->mutex);service_context->interrupt=[handles]{for(const auto &h:handles) {h.cancel();}};
        service_context->family=family;service_context->op=op;service_context->has_context=true;
        if(!retryable(family,op)) {service_context->mutation=true;}
        if(service_context->cancelled) {for(const auto &h:handles) {h.cancel();}}
    }
    std::vector<Reply> replies;std::optional<Error> first;
    for(auto &future:futures) {
        auto data=future.get();if(!data) {if(!first) {first=data.error();}continue;}
        replies.push_back({(*data)[4],(*data)[5],Bytes(data->begin()+6,data->end())});
    }
    if(service_context) {std::lock_guard lock(service_context->mutex);service_context->interrupt={};}
    if(first) {throw *first;}return replies;
}

bool Session::sendCommandGroup(uint8_t group,std::span<const uint8_t> payload) {
    auto &d=*m_impl;
    if(group>=p::defs::kCmdGroupChannelCount || payload.size()<4 || payload.size()>maxPayload() || payload.size()>260 || (payload.size()-4)%4) {
        throw Error(ErrorKind::Usage,"invalid command group payload");
    }
    std::lock_guard closing(d.close_mutex);
    {std::lock_guard lock(d.mutex);if(d.closed) {throw Error(ErrorKind::Connection,d.failure);}if(d.pending_streams>=8) {return false;}++d.pending_streams;}
    auto bytes=std::make_shared<Bytes>(p::frameSize(payload.size()));p::encodeFrame(bytes->data(),bytes->size(),static_cast<uint8_t>(p::defs::kCmdGroupChannelFirst+group),payload.data(),payload.size());
    asio::post(d.io,[&d,bytes] {
        if(d.closed || d.writes.size()>=d.options.request_capacity+8) {std::lock_guard lock(d.mutex);--d.pending_streams;return;}
        d.writes.push_back({bytes,{},true,false});if(d.writes.size()==1) {d.write();}
    });return true;
}
bool Session::nextEvent(Frame &frame,std::chrono::milliseconds timeout) {
    auto &d=*m_impl;std::unique_lock lock(d.mutex);d.cv.wait_for(lock,timeout,[&]{return d.closed || !d.events.empty();});
    if(d.events.empty()) {return false;}frame=std::move(d.events.front());d.events.pop_front();return true;
}
uint64_t Session::droppedEvents() const {std::lock_guard lock(m_impl->mutex);return m_impl->dropped;}
uint64_t Session::droppedNotifications() const {auto box=m_impl->mailbox;std::lock_guard lock(box->mutex);return box->dropped;}
std::shared_ptr<GroupBuffer> Session::subscribeBuffer(uint8_t group,uint8_t epoch,size_t width,size_t capacity,uint64_t configured_generation) {
    auto buffer=std::make_shared<GroupBuffer>(group,epoch,width,capacity);auto &d=*m_impl;
    const auto expected=configured_generation?configured_generation:generation();
    std::lock_guard closing(d.close_mutex);if(d.closed) {throw Error(ErrorKind::Connection,"session closed");}
    asio::post(d.io,[&d,group,buffer,expected] {
        if(d.closed || d.boot_generation!=expected) {buffer->invalidate();return;}
        if(auto it=d.buffers.find(group);it!=d.buffers.end()) {it->second->invalidate();}
        d.buffers[group]=buffer;
    });return buffer;
}
void Session::removeBuffer(uint8_t group) {
    auto &d=*m_impl;std::lock_guard closing(d.close_mutex);if(d.closed) {return;}
    asio::post(d.io,[&d,group] {if(auto it=d.buffers.find(group);it!=d.buffers.end()) {it->second->invalidate();d.buffers.erase(it);}});
}
std::shared_ptr<CommandGroup> Session::manageCommand(uint8_t group,uint8_t epoch,std::vector<ItemDescriptor> descriptors,
                                                   std::span<const uint32_t> hold,uint16_t watchdog_ms,uint64_t configured_generation) {
    auto command=std::shared_ptr<CommandGroup>(new CommandGroup(group,epoch,std::move(descriptors),hold,watchdog_ms));
    const auto expected=configured_generation?configured_generation:generation();
    auto &d=*m_impl;std::lock_guard closing(d.close_mutex);if(d.closed) {throw Error(ErrorKind::Connection,"session closed");}
    asio::post(d.io,[&d,command,expected] {
        if(d.closed || d.boot_generation!=expected) {command->stop();return;}
        const bool start=d.commands.empty();
        if(auto it=d.commands.find(command->id());it!=d.commands.end()) {if(auto old=it->second.lock()) {old->stop();}}
        d.commands[command->id()]=command;if(start) {d.commandTick();}
    });return command;
}
void Session::stopCommand(uint8_t group) {
    auto &d=*m_impl;std::lock_guard closing(d.close_mutex);if(d.closed) {return;}
    asio::post(d.io,[&d,group] {if(auto it=d.commands.find(group);it!=d.commands.end()) {if(auto command=it->second.lock()) {command->stop();}d.commands.erase(it);}});
}
void Session::setMaxPayload(uint16_t cap) {
    if(cap<6 || cap>p::kMaxPayload) {throw Error(ErrorKind::Protocol,"invalid device payload cap");}
    std::lock_guard lock(m_impl->mutex);m_impl->max_payload=cap;
}
uint16_t Session::maxPayload() const {return m_impl->maxPayload();}
uint64_t Session::generation() const {std::lock_guard lock(m_impl->mutex);return m_impl->boot_generation;}
ClockModel Session::clockModel() const {
    std::lock_guard lock(m_impl->mutex);return aged(m_impl->clock);
}
uint64_t Session::observe(EventHandler handler) {
    auto observer=std::make_shared<Impl::Observer>();observer->handler=std::move(handler);
    auto box=m_impl->mailbox;std::lock_guard lock(box->mutex);const auto id=++box->sequence;box->observers.emplace(id,std::move(observer));return id;
}
void Session::unobserve(uint64_t id) {
    std::shared_ptr<Impl::Observer> observer;
    auto box=m_impl->mailbox;
    {std::lock_guard lock(box->mutex);auto it=box->observers.find(id);if(it==box->observers.end()) {return;}observer=it->second;box->observers.erase(it);}
    std::lock_guard lock(observer->mutex);observer->active=false;
}
void Session::dispatchCompletion(const std::shared_ptr<detail::Cancellation> &state,Task task) {m_impl->deliver(std::move(task),state);}
void Session::enqueueService(const std::shared_ptr<detail::Cancellation> &state,Task task) {
    auto &d=*m_impl;
    state->reservation=callbackSlot();
    {std::lock_guard lock(d.mutex);
     if(d.service_count>=d.options.request_capacity) {throw Error(ErrorKind::Usage,"service capacity exhausted");}
     ++d.service_count;
     std::erase_if(d.service_states,[](const auto &weak){auto s=weak.lock();if(!s) {return true;}std::lock_guard guard(s->mutex);return s->done;});d.service_states.push_back(state);}
    if(d.closed) {
        std::function<void(Error)> terminate;
        {std::lock_guard lock(state->mutex);state->cancelled=true;terminate=state->terminate;}
        if(terminate) {terminate(Error(ErrorKind::Connection,"session closed"));}
    }
    d.services.post([&d,state,task=std::move(task)]() mutable {
        service_context=state;task();service_context.reset();
        std::lock_guard lock(d.mutex);--d.service_count;
    });
}
std::shared_ptr<void> Session::callbackSlot(bool disarm) {
    auto count=m_impl->callback_count;const auto previous=count->fetch_add(1);
    if(previous>=m_impl->options.request_capacity+(disarm?1U:0U)) {--*count;throw Error(ErrorKind::Usage,"completion capacity exhausted; run the dispatcher");}
    return std::shared_ptr<void>(count.get(),[count](void *){--*count;});
}
} // namespace drivelink
