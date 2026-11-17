#include "core/command_service.hpp"
#include "core_os/boot_info.hpp"
#include "core_os/clock.hpp"
#include "core_os/diag_sink.hpp"
#include "core_os/realtime.hpp"
#include "core_os/native_boot.hpp"
#include "core_os/native_diag.hpp"
#include "core_os/native_file.hpp"
#include "core_os/native_kv.hpp"
#include "diag_table.hpp"
#include "drivelink/client.hpp"
#include "core_os/native_ota.hpp"
#include "drivelink/protocol/framing.hpp"
#include "drivelink/protocol/time_codec.hpp"
#include "drivelink/protocol/scanner.hpp"
#include "mod_mock/modules.hpp"
#include "mod_motor/modules.hpp"
#include <array>
#include <asio.hpp>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace
{
namespace p = drivelink::protocol;
namespace msg = p::defs::msg;
namespace dp = diag::protocol;
using drivelink::Bytes;
#ifdef _WIN32
// Native loops wait on steady-clock deadlines. Request process-local 1 ms
// waits rather than relying on the desktop/runner's coarse default quantum.
// Keep this alive outside Server so all core threads stop before release.
struct TimerResolution
{
    TimerResolution()
    {
        if (timeBeginPeriod(1) != TIMERR_NOERROR)
        {
            throw std::runtime_error("cannot request 1 ms Windows timer resolution");
        }
    }
    ~TimerResolution() { timeEndPeriod(1); }
    TimerResolution(const TimerResolution &) = delete;
    TimerResolution &operator=(const TimerResolution &) = delete;
};
#endif
struct DirectoryLock
{
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit DirectoryLock(const std::filesystem::path &p)
    {
        handle = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("state directory already in use or inaccessible");
        }
    }
    ~DirectoryLock()
    {
        CloseHandle(handle);
    }
#else
    int handle = -1;
    explicit DirectoryLock(const std::filesystem::path &p)
    {
        handle = ::open(p.c_str(), O_CREAT | O_RDWR, 0600);
        if (handle < 0 || flock(handle, LOCK_EX | LOCK_NB) != 0)
        {
            if (handle >= 0)
            {
                ::close(handle);
            }
            throw std::runtime_error("state directory already in use or inaccessible");
        }
    }
    ~DirectoryLock()
    {
        ::close(handle);
    }
#endif
};
struct Diagnostics
{
    diag::detail::SlotTable &slots = core_os::native::diagTable();
    std::mutex mutex;
    struct Record
    {
        core_os::native::RecordedReport report;
        uint64_t timestamp;
    };
    std::array<Record, 128> records{};
    size_t head = 0, count = 0;
    uint32_t dropped = 0, tx_dropped = 0;
    std::array<uint32_t, 254> last{};
    uint16_t sequence = 0;
    void report(const core_os::native::RecordedReport &r)
    {
        const auto slot = slots.resolve(r.code.slot, r.instance);
        if (slot >= 254)
        {
            ++dropped;
            return;
        }
        std::lock_guard lock(mutex);
        if (count == records.size())
        {
            ++dropped;
            return;
        }
        records[(head + count) % records.size()] = {r, static_cast<uint64_t>(core_os::nowUs())};
        ++count;
    }
    uint8_t activeErrors() const
    {
        unsigned n = 0;
        for (size_t i = 0; i < 254; ++i)
        {
            if (slots.allocated(i) && (slots.read(i).state & 2) &&
                diag::kCodeTable[slots.codeSlot(i)].severity == diag::Severity::Error)
            {
                ++n;
            }
        }
        return static_cast<uint8_t>(n);
    }
};
struct Server;
struct Peer : std::enable_shared_from_this<Peer>
{
    Server &server;
    asio::ip::tcp::socket socket;
#ifndef _WIN32
    std::unique_ptr<asio::posix::stream_descriptor> pty;
    int slave = -1;
#endif
    p::Scanner scanner{1024};
    asio::steady_timer stale;
    asio::steady_timer write_deadline;
    asio::steady_timer drain;
    std::array<uint8_t, 2048> input{};
    std::array<uint8_t, 256> discard{};
    struct Output
    {
        std::shared_ptr<Bytes> bytes;
        core::AfterReply after;
        bool stream;
        bool critical;
        std::chrono::steady_clock::time_point expires;
    };
    std::deque<Output> output;
    bool closed = false, reset_pending = false, draining = false;
    uint64_t generation = 0;
    unsigned delayed = 0;
    explicit Peer(Server &s, asio::io_context &io) : server(s), socket(io), stale(io), write_deadline(io), drain(io) {}
    ~Peer()
    {
#ifndef _WIN32
        if (slave >= 0)
        {
            ::close(slave);
        }
#endif
    }
    bool send(uint8_t type, const uint8_t *data, size_t n, core::AfterReply after = core::AfterReply::None,
              bool stream = false);
    void read();
    void write();
    void close();
    void closeAfterReply();
    void drainRead();
    void finishDrain();
    void armDeadline();
    void recoverPty();
};
struct Server
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor{io};
    asio::steady_timer timer{io};
    asio::signal_set signals{io, SIGINT, SIGTERM};
    std::shared_ptr<Peer> peer;
    core_os::KvStore store;
    std::unique_ptr<Diagnostics> diag;
    std::unique_ptr<core::Core> core;
    std::unique_ptr<core::CommandService> commands;
    std::array<uint8_t, 1024> reply{};
    uint16_t counter = 0;
    uint64_t last_rt_violations = 0;
    int64_t next_status = 0;
    p::DeviceId device_id{0x02, 0x44, 0x4C, 0, 0, 1};
    bool crash_enabled = false, stopping = false;
    // Opt-in, local test controls. No device wire operation is added.
    std::filesystem::path scenario_file;
    uint64_t scenario_sequence=0;
    int64_t scenario_poll=0;
    unsigned slow_ms=0;
    unsigned drop_config_reply=0;
    unsigned drop_disarm=0,drop_arm_reply=0,drop_reboot=0;
    uint64_t disarm_requests=0,reboot_requests=0;
    bool link_stalled=false;
    uint64_t prefix_commands=0,value_commands=0;
    std::array<char,17> ota_build_id{};
    std::string ota_version;
    explicit Server(bool bench) : crash_enabled(bench) {}
    ~Server()
    {
        commands.reset();
        core.reset();
        core_os::native::setDiagSinks(nullptr, nullptr, nullptr);
    }
    void boot(bool first = false)
    {
        commands.reset();
        core.reset();
        core_os::native::setDiagSinks(nullptr, nullptr, nullptr);
        if (!first)
        {
            core_os::native::setResetWasCrash(false);
        }
        core_os::native::resetBootClock();
        core_os::native::resetRecordedDiag();
        diag = std::make_unique<Diagnostics>();
        core_os::native::setDiagSinks(
            diag.get(), [](void *ctx, const auto &r) { static_cast<Diagnostics *>(ctx)->report(r); },
            [](void *ctx, core_os::DiagCode code, uint32_t n, uint8_t inst)
            {
                (void)ctx; (void)code; (void)n; (void)inst;
            });
        core::CoreConfig config;
        config.gpio_allowed_mask &= ~((1ULL << 43) | (1ULL << 44));
        core = std::make_unique<core::Core>(config);
        static const std::array<const core::ModuleType *, 5> types{
            mod_mock::kTypes[0], mod_mock::kTypes[1], mod_mock::kTypes[2], mod_mock::kTypes[3], mod_motor::kTypes[0]};
        if (!core->init(types).ok() || !core->boot(store).ok() || !core->start().ok())
        {
            throw std::runtime_error("core boot failed");
        }
        const auto &boot_info=core->ota().bootInfo();
        const auto build=drivelink::hex(boot_info.elf);
        std::copy_n(build.data(),16,ota_build_id.data());
        ota_version=boot_info.version.data();
        if (!ota_version.ends_with("-vdev")) { ota_version+="-vdev"; }
        commands = std::make_unique<core::CommandService>(
            *core, core::DeviceInfo{ota_version.c_str(), ota_build_id.data(), 1024, crash_enabled, p::kOtaWindow, device_id});
        counter = 0;
        next_status = 0;
        if (peer && !peer->closed)
        {
            dp::BootEvent event{0, static_cast<uint8_t>(core_os::resetWasCrash() ? 1 : 0),
                                static_cast<uint8_t>(core->instanceCount()),
                                static_cast<uint8_t>(core->instancesFailed()),
                                static_cast<uint8_t>(core->safeMode())};
            const auto n = dp::encodeBootEvent(diag->sequence++, 0, event, reply.data(), reply.size());
            peer->send(msg::kDiagEvent, reply.data(), n);
        }
    }
    void receive(Peer &from, const p::Scanner::Frame &frame,bool delay=true)
    {
        if(link_stalled) {return;}
        if(delay && slow_ms && (frame.msg_type==msg::kCoreCommand || frame.msg_type==msg::kDiagQuery)) {
            if(from.delayed>=4) {from.close();return;}
            ++from.delayed;
            auto owner=from.shared_from_this();auto timer=std::make_shared<asio::steady_timer>(io);
            timer->expires_after(std::chrono::milliseconds(slow_ms));
            Bytes payload(frame.payload,frame.payload+frame.len);const auto type=frame.msg_type;
            const auto generation=from.generation;
            timer->async_wait([this,owner,timer,payload=std::move(payload),type,generation](auto ec) {
                --owner->delayed;
                if(!ec && !owner->closed && owner->generation==generation && !stopping) {
                    receive(*owner,{type,payload.size(),payload.data()},false);
                }
            });
            return;
        }
        if (frame.msg_type == msg::kTimeRequest) {
            const auto rx=static_cast<uint64_t>(core_os::nowUs());uint16_t id=0;
            if(!p::time_wire::decodeRequest(frame.payload,frame.len,id)) {
                core_os::countDiag(p::defs::diag::codes::core::kBadTimeRequest,1);return;
            }
            const auto n=p::time_wire::response(reply.data(),reply.size(),{id,rx,0});
            from.send(msg::kTimeResponse,reply.data(),n);
        }
        else if (frame.msg_type == msg::kDisarm)
        {
            if(!scenario_file.empty()) {++disarm_requests;}
            if(drop_disarm) {--drop_disarm;return;}
            const auto n = core::fastDisarm(*core, {frame.payload, frame.len}, reply);
            from.send(msg::kCoreResponse, reply.data(), n);
        }
        else if (frame.msg_type == msg::kCoreCommand)
        {
            if (from.reset_pending) {
                // Match the firmware's config fence while its after-reply
                // action drains/commits. Later commands cannot change Ready.
                p::core_wire::Header h;p::wire::Reader r(frame.payload,frame.len);
                (void)p::core_wire::decodeHeader(r,h);p::wire::Writer w(reply.data(),reply.size());
                p::core_wire::response(w,h,core::Result::Busy,core::Detail::StagingBusy);
                from.send(msg::kCoreResponse,reply.data(),w.size());return;
            }
            if(!scenario_file.empty() && frame.len>=4 && frame.payload[2]==static_cast<uint8_t>(p::defs::Family::System) &&
               frame.payload[3]==static_cast<uint8_t>(p::defs::SystemOp::Reboot)) {
                ++reboot_requests;
                // A lost REBOOT: neither applied nor answered.
                if(drop_reboot) {--drop_reboot;return;}
            }
            const auto n = commands->handle({frame.payload, frame.len}, reply);
            if(drop_arm_reply && frame.len>=4 && frame.payload[2]==static_cast<uint8_t>(p::defs::Family::System) &&
               frame.payload[3]==static_cast<uint8_t>(p::defs::SystemOp::Arm) && --drop_arm_reply==0) {
                (void)commands->takeAfterReply();return;
            }
            if(drop_config_reply && frame.len>=4 && frame.payload[2]==static_cast<uint8_t>(p::defs::Family::Group) &&
               frame.payload[3]==static_cast<uint8_t>(p::defs::GroupOp::CmdConfig) && --drop_config_reply==0) {
                (void)commands->takeAfterReply();return;
            }
            from.send(msg::kCoreResponse, reply.data(), n, commands->takeAfterReply());
        }
        else if (frame.msg_type >= p::defs::kCmdGroupChannelFirst &&
                 frame.msg_type < p::defs::kCmdGroupChannelFirst + p::defs::kCmdGroupChannelCount)
        {
            if(!scenario_file.empty()) {if(frame.len==4) {++prefix_commands;}else {++value_commands;}}
            core->groups().receiveCommand(frame.msg_type - p::defs::kCmdGroupChannelFirst,
                {frame.payload, frame.len}, core_os::nowUs());
        }
        else if (frame.msg_type == msg::kDiagQuery)
        {
            query(from, frame);
        }
    }
    void query(Peer &from, const p::Scanner::Frame &f)
    {
        dp::QueryHeader h;
        const uint8_t *body = nullptr;
        size_t length = 0, n = 0;
        std::array<uint8_t, 1024> bytes{};
        auto result = dp::ResultCode::InvalidArgument;
        if (dp::decodeQueryHeader(f.payload, f.len, h, body, length))
        {
            if (h.subcommand == 1 && dp::decodeGetInfo(body, length))
            {
                dp::InfoBody info;
                info.uptime_us = core_os::nowUs();
                info.reset_reason = core_os::resetWasCrash() ? 1 : 0;
                info.instances_ok = static_cast<uint8_t>(core->instanceCount());
                info.instances_failed = static_cast<uint8_t>(core->instancesFailed());
                info.safe_mode = core->safeMode();
                info.records_dropped = diag->dropped;
                info.events_tx_dropped = diag->tx_dropped;
                info.version = "0.1.0-vdev";
                info.version_len = 10;
                n = dp::encodeInfoBody(info, bytes.data(), bytes.size());
                result = dp::ResultCode::Ok;
            }
            else if (h.subcommand == 2)
            {
                uint8_t start = 0;
                if (dp::decodeGetEntries(body, length, start))
                {
                    std::array<diag::detail::SlotSnapshot, 254> snapshots{};
                    for (size_t i = 0; i < 254; ++i)
                    {
                        snapshots[i] = diag->slots.read(i);
                    }
                    std::array<dp::EntryState, 112> entries{};
                    uint8_t total = 0, next = 255;
                    const auto count = diag::detail::collectEntries(
                        diag::kCodeTable.data(), snapshots.data(), snapshots.size(), start, entries.data(),
                        entries.size(), total, next, &diag->slots);
                    n = dp::encodeEntriesBody(total, next, entries.data(), count, bytes.data(), bytes.size());
                    result = dp::ResultCode::Ok;
                }
            }
            else if (h.subcommand == 3)
            {
                uint8_t what = 0;
                if (dp::decodeClear(body, length, what))
                {
                    if (what & 1)
                    {
                        diag->slots.clearOccurred();
                    }
                    if (what & 2)
                    {
                        diag->slots.resetCounts();
                        diag->last.fill(0);
                    }
                    result = dp::ResultCode::Ok;
                }
            }
            else
            {
                result = dp::ResultCode::NotSupported;
            }
        }
        n = dp::encodeQueryResponse(h, result, bytes.data(), n, reply.data(), reply.size());
        from.send(msg::kDiagQueryResponse, reply.data(), n);
    }
    void scenario()
    {
        const auto now=core_os::nowUs();
        if(scenario_file.empty() || now<scenario_poll) {return;}
        scenario_poll=now+100000;
        std::ifstream input(scenario_file);std::array<char,129> line{};
        input.getline(line.data(),line.size());
        if(!input || input.gcount()>128) {return;}
        std::istringstream command(line.data());uint64_t sequence=0;std::string action;
        if(!(command>>sequence>>action) || sequence<=scenario_sequence) {return;}
        scenario_sequence=sequence;
        if(action=="fault") {core_os::raiseDiag(p::defs::diag::codes::core::kLoopStalled,0);}
        else if(action=="fault-clear") {core_os::clearDiag(p::defs::diag::codes::core::kLoopStalled,0);}
        else if(action=="stall") {link_stalled=true;}
        else if(action=="resume") {link_stalled=false;}
        else if(action=="slow") {unsigned ms=0;if(command>>ms && ms<=1000) {slow_ms=ms;}}
        else if(action=="drop-config-reply") {unsigned n=0;if(command>>n && n<=32) {drop_config_reply=n;}}
        else if(action=="drop-disarm") {unsigned n=0;if(command>>n && n<=32) {drop_disarm=n;}}
        else if(action=="drop-arm-reply") {unsigned n=0;if(command>>n && n<=32) {drop_arm_reply=n;}}
        else if(action=="drop-reboot") {unsigned n=0;if(command>>n && n<=32) {drop_reboot=n;}}
        else if(action=="stats") {
            std::ofstream output(scenario_file.string()+".stats",std::ios::trunc);
            output<<"prefix "<<prefix_commands<<"\nvalues "<<value_commands<<'\n';
            output<<"disarms "<<disarm_requests<<'\n';
            output<<"reboots "<<reboot_requests<<'\n';
            for(size_t i=0;i<core->groups().capacity(core::GroupKind::Command);++i) {
                const auto &g=*core->groups().info(core::GroupKind::Command,uint8_t(i));
                if(g.configured) {output<<"group "<<i<<' '<<unsigned(g.loop)<<' '<<unsigned(g.count)<<'\n';}
            }
        }
        else if(action=="reboot") {
            if(peer) {peer->recoverPty();}
            boot();scenario_poll=0;
        }
        std::cout<<"SCENARIO "<<sequence<<' '<<action<<std::endl;
    }
    void tick()
    {
        if (stopping)
        {
            return;
        }
        scenario();
        if (!core_os::native::bootFileHealthy())
        {
            throw std::runtime_error("crash state persistence failed");
        }
        if (peer && !peer->closed)
        {
            if (const auto n = core::captureDoneEvent(*core, reply); n) { peer->send(msg::kCoreEvent, reply.data(), n); }
            for (size_t i=0; i<core->loopCapacity(); ++i) {
                const auto n=core::testgenDoneEvent(*core, reply); if (!n) { break; }
                peer->send(msg::kCoreEvent, reply.data(), n);
            }
            for (size_t i=0;i<core->groups().capacity(core::GroupKind::Subscription);++i) {
                const auto n=core->groups().takeStream(static_cast<uint8_t>(i),reply);
                if (n) { peer->send(static_cast<uint8_t>(p::defs::kSubGroupChannelFirst+i),reply.data(),n,core::AfterReply::None,true); }
            }
            const auto now = core_os::nowUs();
            if (now >= next_status)
            {
                const core::Ref ref{0, 0, 3};
                uint32_t hz = 10;
                core->params().get({&ref, 1}, {&hz, 1});
                next_status = now + 1000000 / hz;
                p::wire::Writer w(reply.data(), reply.size());
                w.appendU8(p::core_wire::kVersion);
                w.appendU8(0);
                w.appendU16Le(counter++);
                w.appendU64Le(now);
                w.appendU8(static_cast<uint8_t>(core->armStatus()));
                w.appendU8((core->armStatus() == core::ArmStatus::Fault ? 1 : 0) | (core->blocksArm() ? 2 : 0) |
                           (core->pendingReboot() ? 4 : 0) | (core->nvsDirty() ? 8 : 0) |
                           (core->safeMode() ? 32 : 0) | core::captureStatusBits(*core) | core::testgenStatusBits(*core));
                w.appendU8(diag->activeErrors());
                w.appendU8(255);
                w.appendU8(255);
                w.appendU8(static_cast<uint8_t>(core->loopCount()));
                for (size_t i = 0; i < core->loopCapacity(); ++i) {
                    const auto *loop = core->loopAt(i); if (!loop) { continue; }
                    w.appendU8(loop->id()); w.appendU32Le(loop->stats().overruns);
                }
                w.appendU8(static_cast<uint8_t>(core->lastDisarmReason()));
                peer->send(msg::kStatusStream, reply.data(), w.size(), core::AfterReply::None, true);
            }
            const auto violations = core_os::realtimeViolations();
            if (violations != last_rt_violations) {
                std::cerr << "real-time guard violations: " << violations << '\n';
                last_rt_violations = violations;
            }
            // Records remain pending until queued. No unbounded recording vector.
            for (size_t slot = 0; slot < diag::detail::kRuntimeSlots; ++slot) {
                auto dirty = diag->slots.takeDirty(slot);
                if (!dirty) { continue; }
                const auto &code = diag::kCodeTable[diag->slots.codeSlot(slot)];
                const bool active = diag->slots.read(slot).state & dp::kStateActive;
                for (const auto bit : {active ? 2U : 1U, active ? 1U : 2U}) {
                    if (!(dirty & bit)) { continue; }
                    size_t n;
                    if (bit == 1) {
                        dp::RecordEvent ev{static_cast<uint64_t>(now), static_cast<uint8_t>(code.severity),
                            static_cast<uint8_t>(code.source), diag->slots.instance(slot), code.code, 0, 0, 0};
                        n = dp::encodeRecordEvent(diag->sequence, 0, ev, reply.data(), reply.size());
                    } else {
                        dp::ClearedEvent ev{static_cast<uint64_t>(now), static_cast<uint8_t>(code.source), diag->slots.instance(slot), code.code};
                        n = dp::encodeClearedEvent(diag->sequence, 0, ev, reply.data(), reply.size());
                    }
                    if (!peer->send(msg::kDiagEvent, reply.data(), n)) { diag->slots.markDirty(slot, dirty); break; }
                    ++diag->sequence; dirty &= ~bit;
                }
            }
            std::lock_guard lock(diag->mutex);
            if (diag->count)
            {
                const auto &r = diag->records[diag->head];
                const auto &v = r.report;
                dp::RecordEvent ev{r.timestamp,
                                   static_cast<uint8_t>(v.code.severity),
                                   static_cast<uint8_t>(v.code.source),
                                   v.instance,
                                   v.code.code,
                                   v.err,
                                   v.arg0,
                                   v.arg1};
                const auto n = dp::encodeRecordEvent(diag->sequence, 0, ev, reply.data(), reply.size());
                if (peer->send(msg::kDiagEvent, reply.data(), n))
                {
                    ++diag->sequence;
                    diag->head = (diag->head + 1) % 128;
                    --diag->count;
                }
            }
            std::array<uint32_t, 254> counts{};
            for (size_t i = 0; i < 254; ++i)
            {
                counts[i] = diag->slots.read(i).count;
            }
            std::array<dp::CounterEntry, 64> entries{};
            std::array<uint8_t, 64> slots{};
            size_t cursor = 0;
            const auto n = diag::detail::collectCounterDeltas(diag::kCodeTable.data(), counts.data(),
                                                              diag->last.data(), 254, cursor, entries.data(),
                                                              slots.data(), entries.size(), &diag->slots);
            if (n)
            {
                const auto len = dp::encodeCounterDeltaEvent(diag->sequence, 0, now, entries.data(), n,
                                                             reply.data(), reply.size());
                if (peer->send(msg::kDiagEvent, reply.data(), len))
                {
                    ++diag->sequence;
                    for (size_t i = 0; i < n; ++i)
                    {
                        diag->last[slots[i]] = counts[slots[i]];
                    }
                }
            }
        }
        timer.expires_after(std::chrono::milliseconds(5));
        timer.async_wait(
            [this](auto ec)
            {
                if (!ec)
                {
                    tick();
                }
            });
    }
    void accept()
    {
        auto next = std::make_shared<Peer>(*this, io);
        acceptor.async_accept(next->socket,
                              [this, next](auto ec)
                              {
                                  if (ec)
                                  {
                                      return;
                                  }
                                  if (!peer || peer->closed)
                                  {
                                      peer = next;
                                      peer->read();
                                  }
                                  else
                                  {
                                      next->close();
                                  }
                                  accept();
                              });
    }
    void stop()
    {
        stopping = true;
        if (peer)
        {
            peer->close();
        }
        asio::error_code ec;
        acceptor.close(ec);
        timer.cancel();
        signals.cancel();
        commands.reset();
        core.reset();
        core_os::native::setDiagSinks(nullptr, nullptr, nullptr);
        if (core_os::native::cleanBootFile() != 0)
        {
            throw std::runtime_error("failed to persist clean shutdown");
        }
    }
};
void Peer::close()
{
    if (closed)
    {
        if (draining)
        {
            finishDrain();
        }
        return;
    }
    closed = true;
    asio::error_code ec;
    socket.close(ec);
    stale.cancel();
    write_deadline.cancel();
#ifndef _WIN32
    if (pty)
    {
        pty->close(ec);
    }
#endif
}
// A device that reboots after its reply leaves that reply readable. Closing a
// TCP socket with unread input (a frame pipelined behind REBOOT) makes the OS
// send RST, and on Windows an RST discards the client's unread reply. So send
// FIN after the reply, discard input like a rebooting device, and close once
// the client closes or after a bound. The peer is closed for accept() at once.
void Peer::closeAfterReply()
{
    if (closed)
    {
        return;
    }
    closed = true;
    draining = true;
    stale.cancel();
    write_deadline.cancel();
    asio::error_code ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    if (ec)
    {
        finishDrain();
        return;
    }
    socket.cancel(ec); // the read loop's pending read completes as closed
    auto self = shared_from_this();
    drain.expires_after(std::chrono::seconds(1));
    drain.async_wait([self](auto) { self->finishDrain(); });
    drainRead();
}
void Peer::drainRead()
{
    auto self = shared_from_this();
    socket.async_read_some(asio::buffer(discard),
                           [self](auto ec, size_t)
                           {
                               if (!self->draining)
                               {
                                   return;
                               }
                               if (ec)
                               {
                                   self->finishDrain();
                                   return;
                               }
                               self->drainRead();
                           });
}
void Peer::finishDrain()
{
    if (!draining)
    {
        return;
    }
    draining = false;
    drain.cancel();
    asio::error_code ec;
    socket.close(ec);
}
bool Peer::send(uint8_t type, const uint8_t *data, size_t n, core::AfterReply after, bool stream)
{
    if (closed || !n || server.link_stalled)
    {
        return false;
    }
    if (stream) {
        // The front buffer may be in flight. Replace only a pending sample
        // of this channel; interleaved groups retain independent latest slots.
        for (size_t i = 1; i < output.size(); ++i) {
            if (output[i].stream && output[i].bytes->at(3) == type) {
                auto bytes = std::make_shared<Bytes>(p::frameSize(n));
                p::encodeFrame(bytes->data(), bytes->size(), type, data, n);
                output[i].bytes = std::move(bytes);
                return true;
            }
        }
    }
    if (output.size() >= 64)
    {
        if (!stream && type != msg::kDiagEvent)
        {
            recoverPty();
        }
        return false;
    }
    auto bytes = std::make_shared<Bytes>(p::frameSize(n));
    p::encodeFrame(bytes->data(), bytes->size(), type, data, n);
    output.push_back({bytes, after, stream, type == msg::kCoreResponse || type == msg::kDiagQueryResponse || type == msg::kTimeResponse,
                      std::chrono::steady_clock::now() + std::chrono::seconds(2)});
    if (after!=core::AfterReply::None) { reset_pending=true; }
    armDeadline();
    if (output.size() == 1)
    {
        write();
    }
    return true;
}
void Peer::recoverPty()
{
#ifndef _WIN32
    if (pty)
    {
        // Cancel pending operations but retain the master/slave path. Old
        // handlers own their buffers and cannot touch the new generation.
        ++generation;
        asio::error_code ec;
        pty->cancel(ec);
        stale.cancel();
        write_deadline.cancel();
        output.clear();
        reset_pending=false;
        scanner = p::Scanner(1024);
        read();
        return;
    }
#endif
    close();
}
void Peer::armDeadline()
{
    write_deadline.cancel();
    auto deadline = std::chrono::steady_clock::time_point::max();
    for (const auto &item : output)
    {
#ifndef _WIN32
        // A PTY can legitimately have no external reader between CLI calls.
        // Only command replies need a drain deadline; idle streams stay bounded
        // by the latest-wins queue and one in-flight buffer.
        if (pty && !item.critical)
        {
            continue;
        }
#endif
        deadline = std::min(deadline, item.expires);
    }
    if (deadline == std::chrono::steady_clock::time_point::max())
    {
        return;
    }
    write_deadline.expires_at(deadline);
    auto self = shared_from_this();
    const auto epoch = generation;
    write_deadline.async_wait(
        [self, epoch](auto ec)
        {
            if (!ec && self->generation == epoch)
            {
                self->recoverPty();
            }
        });
}
void Peer::write()
{
    auto self = shared_from_this();
    const auto item = output.front();
    if(item.bytes->at(3)==msg::kTimeResponse &&
       !p::time_wire::stampFrame(item.bytes->data(),item.bytes->size(),static_cast<uint64_t>(core_os::nowUs()))) {
        close();return;
    }
    const auto epoch = generation;
    auto handler = [self, item, epoch](auto ec, size_t)
    {
        if (self->closed || self->generation != epoch)
        {
            return;
        }
        if (ec)
        {
            self->close();
            return;
        }
        self->output.pop_front();
        if (item.after != core::AfterReply::None) { self->reset_pending=false; }
        if (item.after != core::AfterReply::None &&
            (item.after != core::AfterReply::CommitAndReboot || self->server.core->ota().commit().ok()))
        {
            if (item.after == core::AfterReply::Crash)
            {
                std::_Exit(86);
            }
#ifndef _WIN32
            if (self->pty)
            {
                self->output.clear();
                self->write_deadline.cancel();
                self->stale.cancel();
                self->scanner = p::Scanner(1024);
                self->server.boot();
                return;
            }
            else
#endif
            {
                self->closeAfterReply();
                self->server.boot();
                return;
            }
        }
        self->armDeadline();
        if (!self->output.empty())
        {
            self->write();
        }
    };
    // The completion condition runs before every low-level write in Asio's
    // composed operation. A canceled generation must not reissue a partial
    // write whose successful completion had already been queued at cancel().
    auto progress = [self, epoch](const asio::error_code &ec, size_t transferred) -> size_t
    { return !self->closed && self->generation == epoch ? asio::transfer_all()(ec, transferred) : 0; };
#ifndef _WIN32
    if (pty)
    {
        asio::async_write(*pty, asio::buffer(*item.bytes), progress, handler);
        return;
    }
#endif
    asio::async_write(socket, asio::buffer(*item.bytes), progress, handler);
}
void Peer::read()
{
    auto self = shared_from_this();
    const auto epoch = generation;
    auto handler = [self, epoch](auto ec, size_t n)
    {
        if (self->closed || self->generation != epoch)
        {
            return;
        }
        if (ec)
        {
            self->close();
            return;
        }
        size_t offset = 0;
        while (offset < n && !self->closed)
        {
            size_t space = 0;
            auto *dst = self->scanner.writable(space);
            const auto count = std::min(space, n - offset);
            if (!count)
            {
                self->close();
                return;
            }
            std::copy_n(self->input.data() + offset, count, dst);
            self->scanner.commit(count);
            offset += count;
            p::Scanner::Frame f;
            while (self->scanner.next(f) && !self->closed)
            {
                self->server.receive(*self, f);
                if (self->generation != epoch)
                {
                    return;
                }
            }
        }
        self->stale.expires_after(std::chrono::milliseconds(250));
        self->stale.async_wait(
            [self, epoch](auto error)
            {
                if (!error && !self->closed && self->generation == epoch)
                {
                    self->scanner.dropStale();
                    p::Scanner::Frame f;
                    while (self->scanner.next(f))
                    {
                        self->server.receive(*self, f);
                    }
                }
            });
        if (!self->closed)
        {
            self->read();
        }
    };
#ifndef _WIN32
    if (pty)
    {
        pty->async_read_some(asio::buffer(input), handler);
        return;
    }
#endif
    socket.async_read_some(asio::buffer(input), handler);
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        std::filesystem::path state = "vdev-state";
        unsigned port = 8765;
        p::DeviceId device_id{0x02, 0x44, 0x4C, 0, 0, 1};
        bool pty = false, bench = false;
        bool safe_mode=false;
        std::filesystem::path scenario_file;
        int64_t fail_after = -1;
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            auto value = [&]()
            {
                if (++i >= argc)
                {
                    throw std::runtime_error("missing option value");
                }
                return std::string(argv[i]);
            };
            if (arg == "--state")
            {
                state = value();
            }
            else if (arg == "--port")
            {
                port = drivelink::parseUnsigned(value(), 65535);
            }
            else if (arg == "--device-id")
            {
                const auto bytes = drivelink::parseHex(value());
                if (bytes.size() != device_id.size()) { throw std::runtime_error("device id needs 12 hex digits"); }
                std::copy(bytes.begin(), bytes.end(), device_id.begin());
            }
            else if (arg == "--pty")
            {
                pty = true;
            }
            else if (arg == "--allow-crash")
            {
                bench = true;
            }
            else if(arg=="--scenario-file") {scenario_file=value();}
            else if(arg=="--safe-mode") {safe_mode=true;}
            else if (arg == "--fail-writes-after")
            {
                fail_after = drivelink::parseUnsigned(value());
            }
            else if (arg == "--help")
            {
                std::cout << "drivelink-vdev [--state DIR] [--port PORT | --pty] [--allow-crash] "
                             "[--device-id HEX12] [--fail-writes-after N] [--scenario-file PATH] [--safe-mode]\n";
                return 0;
            }
            else
            {
                throw std::runtime_error("unknown option: " + arg);
            }
        }
        std::filesystem::create_directories(state);
        DirectoryLock lock(state / "process.lock");
        p::Sha256Digest baseline_elf{};
        const auto source_id=drivelink::parseHex(DRIVELINK_BUILD_ID);
        std::copy_n(source_id.begin(),std::min(source_id.size(),baseline_elf.size()),baseline_elf.begin());
        core_os::native::setOtaBaseline(baseline_elf,"0.1.0-vdev");
        core_os::native::setOtaDirectory(state);
#ifdef _WIN32
        TimerResolution timer_resolution;
#endif
        Server s(bench);
        s.scenario_file=scenario_file;
        s.device_id = device_id;
        if (core_os::native::openFileStore(s.store, state / "kv") != 0 ||
            core_os::native::openBootFile(state / "boot.state") != 0)
        {
            throw std::runtime_error("cannot load persistent state");
        }
        if (fail_after >= 0)
        {
            core_os::native::failWritesAfter(s.store, static_cast<uint32_t>(fail_after));
        }
        if(safe_mode) {core_os::storeCrashCount(3);core_os::native::setResetWasCrash(true);}
        s.boot(true);
        if (pty)
        {
#ifdef _WIN32
            throw std::runtime_error("--pty is Linux-only");
#else
            auto peer = std::make_shared<Peer>(s, s.io);
            const int master = posix_openpt(O_RDWR | O_NOCTTY);
            if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
            {
                throw std::runtime_error("cannot open pty");
            }
            const char *name = ptsname(master);
            if (!name)
            {
                throw std::runtime_error("cannot name pty");
            }
            peer->slave = ::open(name, O_RDWR | O_NOCTTY);
            termios mode{};
            if (peer->slave < 0 || tcgetattr(peer->slave, &mode) != 0)
            {
                throw std::runtime_error("cannot configure pty");
            }
            cfmakeraw(&mode);
            if (tcsetattr(peer->slave, TCSANOW, &mode) != 0)
            {
                throw std::runtime_error("cannot set raw pty");
            }
            peer->pty = std::make_unique<asio::posix::stream_descriptor>(s.io, master);
            s.peer = peer;
            peer->read();
            std::cout << "READY serial://" << name << std::endl;
#endif
        }
        else
        {
            s.acceptor.open(asio::ip::tcp::v4());
            s.acceptor.set_option(asio::socket_base::reuse_address(true));
            s.acceptor.bind({asio::ip::address_v4::loopback(), static_cast<uint16_t>(port)});
            s.acceptor.listen();
            s.accept();
            std::cout << "READY tcp://127.0.0.1:" << s.acceptor.local_endpoint().port() << std::endl;
        }
        s.signals.async_wait(
            [&](auto ec, int)
            {
                if (!ec)
                {
                    s.stop();
                }
            });
        s.tick();
        s.io.run();
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "vdev: " << e.what() << '\n';
        return 1;
    }
}
