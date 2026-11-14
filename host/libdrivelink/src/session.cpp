#include "drivelink/session.hpp"
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

namespace drivelink
{
namespace p = protocol;
namespace msg = protocol::defs::msg;
namespace { std::atomic<uint64_t> next_clock_generation{1}; }
struct Session::Impl
{
    SessionOptions options;
    asio::io_context io;
    asio::ip::tcp::socket tcp{io};
    asio::serial_port serial{io};
    bool use_tcp = false;
    p::Scanner scanner{p::kMaxPayload};
    asio::steady_timer stale{io};
    std::array<uint8_t, 2048> input{};
    std::thread thread;
    std::mutex call_mutex, close_mutex;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Frame> events;
    std::deque<std::shared_ptr<Bytes>> writes;
    uint64_t dropped = 0;
    size_t pending_streams = 0; // guarded by mutex, including posted writes
    uint16_t max_payload = p::kMaxPayload;
    std::array<bool, 256> retired{};
    unsigned sequence = 0;
    bool closed = false, waiting = false, ready = false, protocol_error = false;
    uint8_t expected_type = 0, expected_seq = 0, expected_family = 0, expected_op = 0;
    Bytes response;
    struct OtaPending { uint8_t seq=0;bool ready=false;Bytes response; };
    std::array<OtaPending,4> ota_pending{};
    size_t ota_count=0;
    bool ota_error=false;
    std::bitset<65536> ping_retired;
    uint32_t ping_sequence = 0;
    uint64_t boot_generation = next_clock_generation.fetch_add(1);
    bool ping_waiting = false, ping_ready = false, ping_error = false, ping_written = false;
    TimeSample ping_sample;
    std::shared_ptr<Bytes> ping_bytes;
    std::string failure;

    Impl(const std::string &endpoint, SessionOptions opts) : options(opts)
    {
        if (options.timeout.count() <= 0 || options.timeout > std::chrono::seconds(60) ||
            options.read_retries > 5 || !options.event_capacity || options.event_capacity > 65536)
        {
            throw Error(ErrorKind::Usage, "invalid session limits");
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
        read();
        thread = std::thread([this] { io.run(); });
    }
    void fail(const std::string &why)
    {
        {
            std::lock_guard lock(mutex);
            closed = true;
            failure = why;
        }
        cv.notify_all();
        asio::error_code ec;
        tcp.close(ec);
        serial.close(ec);
        stale.cancel();
        // In-flight buffers are captured by their handlers.
        writes.clear();
    }
    void receive(const p::Scanner::Frame &f)
    {
        std::lock_guard lock(mutex);
        if (f.msg_type==msg::kDiagEvent && f.len==17 && f.payload[0]==3 && f.payload[3]==4) {
            boot_generation=next_clock_generation.fetch_add(1);
            // An in-flight ping must not span a device reboot.
            if (ping_waiting && ping_written) { ping_error=true;cv.notify_all(); }
            if (ota_count) { ota_error=true;cv.notify_all(); }
        }
        if (ota_count && f.msg_type==msg::kCoreResponse && f.len>=2) {
            for (size_t i=0;i<ota_count;++i) {
                auto &pending=ota_pending[i];
                if (f.payload[1]!=pending.seq || pending.ready) { continue; }
                if (f.len<6 || f.payload[0]!=p::core_wire::kVersion) { ota_error=true;cv.notify_all();return; }
                if (f.payload[2]!=static_cast<uint8_t>(p::defs::Family::Ota) ||
                    f.payload[3]!=static_cast<uint8_t>(p::defs::OtaOp::Write)) { return; }
                pending.response.assign(f.payload,f.payload+f.len);pending.ready=true;cv.notify_all();return;
            }
        }
        if (f.msg_type==msg::kTimeResponse) {
            if (!ping_waiting || !ping_written || ping_ready || f.len<3) { return; }
            p::wire::Reader r(f.payload+1,2);const auto id=r.readU16Le();
            if (id!=ping_sample.id) { return; }
            p::time_wire::Response reply;
            if (!p::time_wire::decodeResponse(f.payload,f.len,reply)) { ping_error=true; }
            else {
                ping_sample.esp_rx_us=reply.rx_us;ping_sample.esp_tx_us=reply.tx_us;
                ping_sample.host_rx_us=monotonicUs();ping_ready=true;
            }
            cv.notify_all();return;
        }
        if (waiting && f.msg_type == expected_type && f.len >= 2 && f.payload[1] == expected_seq)
        {
            const bool core = expected_type == msg::kCoreResponse;
            if (f.payload[0] != (core ? p::core_wire::kVersion : 3) || f.len < (core ? 6U : 4U))
            {
                protocol_error = true;
                cv.notify_all();
                return;
            }
            if ((core && (f.payload[2] != expected_family || f.payload[3] != expected_op)) ||
                (!core && f.payload[2] != expected_op))
            {
                return;
            }
            if (!ready)
            {
                response.assign(f.payload, f.payload + f.len);
                ready = true;
                cv.notify_all();
            }
        }
        else if (p::isStream(f.msg_type) || p::isEvent(f.msg_type))
        {
            if (events.size() == options.event_capacity)
            {
                events.pop_front();
                ++dropped;
            }
            events.push_back({f.msg_type, Bytes(f.payload, f.payload + f.len)});
            cv.notify_all();
        }
    }
    void read()
    {
        auto handler = [this](auto ec, size_t n)
        {
            if (ec)
            {
                fail(ec.message());
                return;
            }
            size_t offset = 0;
            while (offset < n)
            {
                size_t space = 0;
                auto *dst = scanner.writable(space);
                const auto count = std::min(space, n - offset);
                std::copy_n(input.data() + offset, count, dst);
                scanner.commit(count);
                offset += count;
                p::Scanner::Frame f;
                while (scanner.next(f))
                {
                    receive(f);
                }
                if (!count)
                {
                    fail("scanner made no progress");
                    return;
                }
            }
            stale.expires_after(std::chrono::milliseconds(250));
            stale.async_wait(
                [this](auto error)
                {
                    if (!error)
                    {
                        scanner.dropStale();
                        p::Scanner::Frame f;
                        while (scanner.next(f))
                        {
                            receive(f);
                        }
                    }
                });
            read();
        };
        if (use_tcp)
        {
            tcp.async_read_some(asio::buffer(input), handler);
        }
        else
        {
            serial.async_read_some(asio::buffer(input), handler);
        }
    }
    void write()
    {
        auto bytes = writes.front();
        if (bytes->at(3)==msg::kTimeRequest) {
            std::lock_guard guard(mutex);
            if (ping_waiting && bytes==ping_bytes) {
                ping_sample.host_tx_us=monotonicUs();ping_sample.boot_generation=boot_generation;ping_written=true;
            }
        }
        auto handler = [this, bytes](auto ec, size_t)
        {
            {
                std::lock_guard lock(mutex);
                if (closed)
                {
                    return;
                }
            }
            if (ec)
            {
                fail(ec.message());
                return;
            }
            if (bytes->at(3) >= p::defs::kCmdGroupChannelFirst &&
                bytes->at(3) < p::defs::kCmdGroupChannelFirst + p::defs::kCmdGroupChannelCount) {
                std::lock_guard guard(mutex);
                --pending_streams;
            }
            writes.pop_front();
            if (!writes.empty())
            {
                write();
            }
        };
        if (use_tcp)
        {
            asio::async_write(tcp, asio::buffer(*bytes), handler);
        }
        else
        {
            asio::async_write(serial, asio::buffer(*bytes), handler);
        }
    }
    Bytes exchange(uint8_t kind, uint8_t family, uint8_t op, std::span<const uint8_t> body, bool read_only)
    {
        std::lock_guard calls(call_mutex);
        for (unsigned attempt = 0;; ++attempt)
        {
            std::unique_lock lock(mutex);
            if (closed)
            {
                throw Error(ErrorKind::Connection, failure);
            }
            unsigned tried = 0;
            while (tried < 256 && retired[sequence % 256])
            {
                ++sequence;
                ++tried;
            }
            if (tried == 256)
            {
                throw Error(ErrorKind::Connection, "sequence IDs exhausted after timeouts; reconnect");
            }
            const auto seq = static_cast<uint8_t>(sequence++ % 256);
            const bool core_kind = kind != msg::kDiagQuery;
            Bytes payload{static_cast<uint8_t>(core_kind ? p::core_wire::kVersion : 3), seq};
            if (core_kind)
            {
                payload.push_back(family);
            }
            payload.push_back(op);
            payload.insert(payload.end(), body.begin(), body.end());
            if (payload.size() > max_payload)
            {
                throw Error(ErrorKind::Usage, "request exceeds device payload cap");
            }
            auto bytes = std::make_shared<Bytes>(p::frameSize(payload.size()));
            p::encodeFrame(bytes->data(), bytes->size(), kind, payload.data(), payload.size());
            waiting = true;
            ready = false;
            protocol_error = false;
            expected_type = core_kind ? msg::kCoreResponse : msg::kDiagQueryResponse;
            expected_seq = seq;
            expected_family = family;
            expected_op = op;
            asio::post(io,
                       [this, bytes]
                       {
                           {
                               std::lock_guard guard(mutex);
                               if (closed)
                               {
                                   return;
                               }
                           }
                           if (writes.size() >= 16)
                           {
                               fail("TX queue full");
                               return;
                           }
                           writes.push_back(bytes);
                           if (writes.size() == 1)
                           {
                               write();
                           }
                       });
            cv.wait_for(lock, options.timeout, [this] { return closed || ready || protocol_error; });
            waiting = false;
            if (protocol_error)
            {
                retired[seq] = true;
                throw Error(ErrorKind::Protocol,
                            "incompatible protocol version or truncated response header");
            }
            if (ready)
            {
                return std::move(response);
            }
            retired[seq] = true;
            if (closed)
            {
                throw Error(ErrorKind::Connection, failure);
            }
            if (!read_only || attempt >= options.read_retries)
            {
                throw Error(ErrorKind::Timeout, "request timed out; outcome may be unknown for writes");
            }
        }
    }
};
Session::Session(const std::string &endpoint, SessionOptions options)
    : m_impl(std::make_unique<Impl>(endpoint, options))
{
}
Session::~Session()
{
    close();
}
void Session::close()
{
    auto &d = *m_impl;
    std::lock_guard closing(d.close_mutex);
    if (!d.thread.joinable())
    {
        return;
    }
    {
        std::lock_guard lock(d.mutex);
        d.closed = true;
        d.failure = "session closed";
    }
    d.cv.notify_all();
    asio::post(d.io, [&d] { d.fail("session closed"); });
    d.thread.join();
}
Reply Session::request(uint8_t family, uint8_t op, std::span<const uint8_t> body)
{
    const bool read_only = (family == 1 && op == 1) || family == 3 || (family == 2 && op == 1) ||
                           (family == 4 && (op == 1 || op == 3)) || (family == 5 && op == 6) ||
                           (family == 6 && (op == 4 || op == 5)) || (family==9 && op==6);
    auto data = m_impl->exchange(family == 1 && op == 3 ? msg::kDisarm : msg::kCoreCommand, family, op, body, read_only);
    return {data[4], data[5], Bytes(data.begin() + 6, data.end())};
}
std::vector<Reply> Session::otaWriteBatch(std::span<const Bytes> bodies)
{
    auto &d=*m_impl;
    if (bodies.empty() || bodies.size()>d.ota_pending.size()) { throw Error(ErrorKind::Usage,"OTA window must be 1..4"); }
    std::lock_guard calls(d.call_mutex);
    std::unique_lock lock(d.mutex);
    if (d.closed) { throw Error(ErrorKind::Connection,d.failure); }
    // Preflight the entire batch before posting any bytes or claiming IDs.
    size_t available=0;for (bool retired:d.retired) { available+=!retired; }
    if (available<bodies.size()) { throw Error(ErrorKind::Connection,"sequence IDs exhausted after timeouts; reconnect"); }
    for (const auto &body:bodies) {
        p::wire::Reader r(body.data(),body.size());(void)r.readU32Le();const auto n=r.readU16Le();
        if (!r.ok() || !n || n>512 || n!=r.remaining() || body.size()+4>d.max_payload) {
            throw Error(ErrorKind::Usage,"invalid OTA WRITE body");
        }
    }
    std::array<std::shared_ptr<Bytes>,4> frames;
    for (size_t i=0;i<bodies.size();++i) {
        while (d.retired[d.sequence%256]) { ++d.sequence; }
        auto &pending=d.ota_pending[i];pending={static_cast<uint8_t>(d.sequence++%256),false,{}};
        const auto &body=bodies[i];Bytes payload{p::core_wire::kVersion,pending.seq,9,2};
        payload.insert(payload.end(),body.begin(),body.end());
        frames[i]=std::make_shared<Bytes>(p::frameSize(payload.size()));
        p::encodeFrame(frames[i]->data(),frames[i]->size(),msg::kCoreCommand,payload.data(),payload.size());
    }
    d.ota_count=bodies.size();d.ota_error=false;
    asio::post(d.io,[&d,frames,count=bodies.size()] {
        { std::lock_guard guard(d.mutex);if (d.closed) { return; } }
        if (d.writes.size()+count>16) { d.fail("TX queue full");return; }
        const bool start=d.writes.empty();
        for (size_t i=0;i<count;++i) { d.writes.push_back(frames[i]); }
        if (start) { d.write(); }
    });
    d.cv.wait_for(lock,d.options.timeout,[&d] {
        return d.closed || d.ota_error || std::all_of(d.ota_pending.begin(),d.ota_pending.begin()+d.ota_count,
                                                    [](const auto &v){return v.ready;});
    });
    const bool complete=std::all_of(d.ota_pending.begin(),d.ota_pending.begin()+d.ota_count,[](const auto &v){return v.ready;});
    std::vector<Reply> replies;replies.reserve(bodies.size());
    for (size_t i=0;i<d.ota_count;++i) {
        const auto &pending=d.ota_pending[i];
        if (!complete || d.ota_error) { d.retired[pending.seq]=true; }
        if (pending.ready) {
            const auto &p=pending.response;replies.push_back({p[4],p[5],Bytes(p.begin()+6,p.end())});
        }
    }
    d.ota_count=0;
    if (d.ota_error) { throw Error(ErrorKind::Protocol,"invalid OTA response or device reboot during upload"); }
    if (d.closed && !complete) { throw Error(ErrorKind::Connection,d.failure); }
    if (!complete) { throw Error(ErrorKind::Timeout,"OTA batch timed out; outcome uncertain; query ota-status"); }
    return replies;
}
TimeSample Session::timePing()
{
    auto &d=*m_impl;
    std::lock_guard calls(d.call_mutex);
    std::unique_lock lock(d.mutex);
    if(d.closed) { throw Error(ErrorKind::Connection,d.failure); }
    unsigned tried=0;
    while(tried<65536 && d.ping_retired[d.ping_sequence%65536]) { ++tried;++d.ping_sequence; }
    if(tried==65536) { throw Error(ErrorKind::Connection,"time ping IDs exhausted; reconnect"); }
    const auto id=static_cast<uint16_t>(d.ping_sequence++%65536);
    std::array<uint8_t,p::time_wire::kRequestBytes> payload{};
    p::time_wire::request(payload.data(),payload.size(),id);
    auto bytes=std::make_shared<Bytes>(p::frameSize(payload.size()));
    p::encodeFrame(bytes->data(),bytes->size(),msg::kTimeRequest,payload.data(),payload.size());
    d.ping_sample={};d.ping_sample.id=id;
    d.ping_waiting=true;d.ping_ready=false;d.ping_error=false;d.ping_written=false;d.ping_bytes=bytes;
    asio::post(d.io,[&d,bytes] {
        {
            std::lock_guard guard(d.mutex);
            if(d.closed || !d.ping_waiting || d.ping_bytes!=bytes) { return; }
        }
        if(d.writes.size()>=16) { d.fail("TX queue full");return; }
        d.writes.push_back(bytes);if(d.writes.size()==1) { d.write(); }
    });
    d.cv.wait_for(lock,d.options.timeout,[&d]{return d.closed || d.ping_ready || d.ping_error;});
    d.ping_waiting=false;d.ping_bytes.reset();
    if(d.ping_error) { d.ping_retired[id]=true;throw Error(ErrorKind::Protocol,"invalid time reply or device reboot during ping"); }
    if(d.ping_ready) { return d.ping_sample; }
    d.ping_retired[id]=true;
    if(d.closed) { throw Error(ErrorKind::Connection,d.failure); }
    throw Error(ErrorKind::Timeout,"time ping timed out");
}
bool Session::sendCommandGroup(uint8_t group, std::span<const uint8_t> payload)
{
    auto &d = *m_impl;
    std::lock_guard lock(d.mutex);
    if (d.closed) { throw Error(ErrorKind::Connection, d.failure); }
    if (group >= protocol::defs::kCmdGroupChannelCount || payload.size() < 4 ||
        payload.size() > d.max_payload || payload.size() > 260 || (payload.size() - 4) % 4) {
        throw Error(ErrorKind::Usage, "invalid command group payload");
    }
    if (d.pending_streams >= 8) { return false; }
    auto bytes = std::make_shared<Bytes>(protocol::frameSize(payload.size()));
    protocol::encodeFrame(bytes->data(), bytes->size(),
        static_cast<uint8_t>(protocol::defs::kCmdGroupChannelFirst + group), payload.data(), payload.size());
    ++d.pending_streams;
    asio::post(d.io, [&d, bytes] {
        {
            std::lock_guard guard(d.mutex);
            if (d.closed) { return; }
        }
        if (d.writes.size() >= 16) {
            std::lock_guard guard(d.mutex); --d.pending_streams; return;
        }
        d.writes.push_back(bytes);
        if (d.writes.size() == 1) { d.write(); }
    });
    return true;
}
Bytes Session::diagRequest(uint8_t op, std::span<const uint8_t> body)
{
    auto data = m_impl->exchange(msg::kDiagQuery, 0, op, body, op == 1 || op == 2);
    if (data[3])
    {
        throw Error(ErrorKind::Rejected, "diagnostic query rejected", data[3]);
    }
    return Bytes(data.begin() + 4, data.end());
}
bool Session::nextEvent(Frame &frame, std::chrono::milliseconds timeout)
{
    auto &d = *m_impl;
    std::unique_lock lock(d.mutex);
    d.cv.wait_for(lock, timeout, [&d] { return d.closed || !d.events.empty(); });
    if (d.events.empty())
    {
        return false;
    }
    frame = std::move(d.events.front());
    d.events.pop_front();
    return true;
}
uint64_t Session::droppedEvents() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->dropped;
}
void Session::setMaxPayload(uint16_t cap)
{
    if (cap < 6 || cap > p::kMaxPayload)
    {
        throw Error(ErrorKind::Protocol, "invalid device payload cap");
    }
    std::lock_guard lock(m_impl->mutex);
    m_impl->max_payload = cap;
}
} // namespace drivelink
