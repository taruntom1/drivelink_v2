#include "core/core_link.hpp"
#include "core/command_service.hpp"
#include "core/cpu_load.hpp"
#include "freertos/idf_additions.h"
#include "core_os/boot_info.hpp"
#include "core_os/clock.hpp"
#include "core_os/periodic_task.hpp"
#include "uart_comm.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "diag.hpp"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *const TAG="core_link";
namespace core {
namespace msg=drivelink::protocol::defs::msg;
namespace {
const char *buildId() {
    static const auto id=[] {
        std::array<char,17> text{};
        const auto *sha=esp_app_get_description()->app_elf_sha256;
        constexpr char hex[]="0123456789abcdef";
        for(size_t i=0;i<8;++i) { text[2*i]=hex[sha[i]>>4];text[2*i+1]=hex[sha[i]&15]; }
        return text;
    }();
    return id.data();
}
}
static_assert(CONFIG_UART_COMM_MAX_PAYLOAD>=defs::kDescriptorMinPayload,"receive limit cannot hold a complete descriptor");
struct CoreLink::Impl {
    Core &core; uart_comm::UartComm &uart; CommandService service;
    StaticQueue_t queue_storage{};
    std::array<uint8_t,CONFIG_CORE_COMMAND_QUEUE_DEPTH*sizeof(uart_comm::RxFrame)> queue_bytes{};
    QueueHandle_t queue=nullptr;
    uart_comm::CallbackRegistration registration;
    uart_comm::CallbackRegistration disarm_registration;
    std::array<uart_comm::CallbackRegistration, defs::kCmdGroupChannelCount> command_registrations;
    std::array<uint8_t, kGroupStreamBytes> group_stream{};
    std::array<uint8_t, kCaptureDoneEventBytes> event{};
    core_os::PeriodicTask task;
    uart_comm::RxFrame frame{};
    std::array<uint8_t,CONFIG_UART_COMM_MAX_PAYLOAD> reply{};
    std::array<uint8_t,19 + 5*kMaxLoops> status{};
    CpuLoadSampler cpu;
    uint16_t counter=0;
    int64_t next_status=0;
    bool valid=false;
    Impl(Core &c,uart_comm::UartComm &u):core(c),uart(u),
        service(c,{esp_app_get_description()->version,buildId(),CONFIG_UART_COMM_MAX_PAYLOAD,
#ifdef CONFIG_CORE_BENCH_CRASH
        true
#else
        false
#endif
        }) {}
    void run() {
        if(xQueueReceive(queue,&frame,0)==pdTRUE) {
            const auto n=service.handle({frame.payload,frame.len},reply);
            esp_err_t err=uart.sendCommand(msg::kCoreResponse,reply.data(),n);
            for(unsigned i=0;err==ESP_ERR_NO_MEM && i<100;++i) {
                vTaskDelay(1);err=uart.sendCommand(msg::kCoreResponse,reply.data(),n);
            }
            if(err!=ESP_OK) { DIAG_COUNT(diag::codes::core::kResponseDropped); }
            const auto after=service.takeAfterReply();
            if(after!=AfterReply::None && err==ESP_OK) {
                if(uart.waitTxIdle(1000)!=ESP_OK) { ESP_LOGE(TAG,"TX drain timed out; reboot cancelled");return; }
                if(after==AfterReply::Crash) { esp_system_abort("bench crash command"); }
                core_os::reboot();
            }
        }
        if (const auto n=captureDoneEvent(core,event); n) {
            // Events are reliable frames: the command pool, retried briefly.
            esp_err_t err=uart.sendCommand(msg::kCoreEvent,event.data(),n);
            for(unsigned i=0;err==ESP_ERR_NO_MEM && i<100;++i) { vTaskDelay(1);err=uart.sendCommand(msg::kCoreEvent,event.data(),n); }
            if(err!=ESP_OK) { DIAG_COUNT(diag::codes::core::kResponseDropped); }
        }
        for (size_t i=0; i<core.loopCapacity(); ++i) {
            const auto n=testgenDoneEvent(core,event); if (!n) { break; }
            esp_err_t err=uart.sendCommand(msg::kCoreEvent,event.data(),n);
            for(unsigned retry=0;err==ESP_ERR_NO_MEM && retry<100;++retry) { vTaskDelay(1);err=uart.sendCommand(msg::kCoreEvent,event.data(),n); }
            if(err!=ESP_OK) { DIAG_COUNT(diag::codes::core::kResponseDropped); }
        }
        for (size_t i=0;i<core.groups().capacity(GroupKind::Subscription);++i) {
            const auto n=core.groups().takeStream(static_cast<uint8_t>(i),group_stream);
            if (n) {
                const auto err=uart.sendData(static_cast<uint8_t>(defs::kSubGroupChannelFirst+i),group_stream.data(),n);
                if(err!=ESP_OK && err!=ESP_ERR_TIMEOUT) { DIAG_COUNT(diag::codes::core::kResponseDropped); }
            }
        }
        const auto now=core_os::nowUs();
        if(now<next_status) { return; }
        const Ref rate{0,0,3};uint32_t hz=10;
        core.params().get({&rate,1},{&hz,1});
        next_status=now+1000000/hz;
        drivelink::protocol::wire::Writer w(status.data(),status.size());
        w.appendU8(drivelink::protocol::core_wire::kVersion);w.appendU8(0);w.appendU16Le(counter++);w.appendU64Le(static_cast<uint64_t>(now));
        w.appendU8(static_cast<uint8_t>(core.armStatus()));
        w.appendU8((core.armStatus()==ArmStatus::Fault?1:0)|(core.blocksArm()?2:0)|
            (core.pendingReboot()?4:0)|(core.nvsDirty()?8:0)|(core.safeMode()?32:0)|captureStatusBits(core)|testgenStatusBits(core));
        w.appendU8(diag::activeErrorCount());
        std::array<uint8_t,2> load{255,255};
#if CONFIG_CORE_STATUS_CPU_LOAD && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && !CONFIG_FREERTOS_SMP && !CONFIG_FREERTOS_UNICORE
        load=cpu.sample(static_cast<uint32_t>(now),{
            static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(0)),
            static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(1))});
#endif
        w.appendU8(load[0]);w.appendU8(load[1]);
        w.appendU8(static_cast<uint8_t>(core.loopCount()));
        for(size_t i=0;i<core.loopCapacity();++i) {
            const auto *loop=core.loopAt(i);if (!loop) { continue; }
            w.appendU8(loop->id());w.appendU32Le(loop->stats().overruns);
        }
        w.appendU8(static_cast<uint8_t>(core.lastDisarmReason()));
        const auto err=uart.sendData(msg::kStatusStream,status.data(),w.size());
        if(err!=ESP_OK && err!=ESP_ERR_TIMEOUT) { ESP_LOGW(TAG,"status send failed: %s",esp_err_to_name(err)); }
    }
};
CoreLink::CoreLink(Core &core,uart_comm::UartComm &uart):m_impl(std::make_unique<Impl>(core,uart)) {
    auto &d=*m_impl;
    d.queue=xQueueCreateStatic(CONFIG_CORE_COMMAND_QUEUE_DEPTH,sizeof(uart_comm::RxFrame),d.queue_bytes.data(),&d.queue_storage);
    if(!d.queue) { return; }
    auto err=uart.openStream(msg::kStatusStream,d.status.size());
    if(err!=ESP_OK) { ESP_LOGE(TAG,"status setup: %s",esp_err_to_name(err));return; }
    auto *impl=&d;
    err=uart.registerCallback(msg::kDisarm, [impl](const uart_comm::RxFrame &f) {
        std::array<uint8_t, 6> reply{};
        const auto n = fastDisarm(impl->core, {f.payload, f.len}, reply);
        if (impl->uart.sendCommand(msg::kCoreResponse, reply.data(), n) != ESP_OK) { DIAG_COUNT(diag::codes::core::kResponseDropped); }
    }, d.disarm_registration);
    if(err!=ESP_OK) { ESP_LOGE(TAG,"disarm callback: %s",esp_err_to_name(err));return; }
    for (size_t i=0;i<core.groups().capacity(GroupKind::Subscription);++i) {
        err=uart.openStream(static_cast<uint8_t>(defs::kSubGroupChannelFirst+i),12+4*core.groups().entryCapacity());
        if(err!=ESP_OK) { ESP_LOGE(TAG,"group stream setup: %s",esp_err_to_name(err));return; }
    }
    for (size_t i=0;i<core.groups().capacity(GroupKind::Command);++i) {
        err=uart.registerCallback(static_cast<uint8_t>(defs::kCmdGroupChannelFirst+i),
            [impl,i](const uart_comm::RxFrame &f) {
                impl->core.groups().receiveCommand(static_cast<uint8_t>(i),{f.payload,f.len},core_os::nowUs());
            },d.command_registrations[i]);
        if(err!=ESP_OK) { ESP_LOGE(TAG,"group callback setup: %s",esp_err_to_name(err));return; }
    }
    err=uart.registerCallback(msg::kCoreCommand,[impl](const uart_comm::RxFrame &f) {
        if (f.len >= 4 && f.payload[2] == static_cast<uint8_t>(defs::Family::System) &&
            f.payload[3] == static_cast<uint8_t>(defs::SystemOp::Disarm)) {
            std::array<uint8_t, 6> reply{};
            const auto n=fastDisarm(impl->core, {f.payload, f.len}, reply);
            if(impl->uart.sendCommand(msg::kCoreResponse,reply.data(),n)!=ESP_OK) { DIAG_COUNT(diag::codes::core::kResponseDropped); }
            return;
        }
        if(xQueueSend(impl->queue,&f,0)==pdTRUE) { return; }
        DIAG_COUNT(diag::codes::core::kCommandDropped);
        drivelink::protocol::core_wire::Header h;
        drivelink::protocol::wire::Reader r(f.payload,f.len);
        drivelink::protocol::core_wire::decodeHeader(r,h);
        uint8_t out[6]{};drivelink::protocol::wire::Writer w(out,sizeof(out));
        drivelink::protocol::core_wire::response(w,h,Result::Busy,Detail::StagingBusy);
        if(impl->uart.sendCommand(msg::kCoreResponse,out,w.size())!=ESP_OK) { DIAG_COUNT(diag::codes::core::kResponseDropped); }
    },d.registration);
    if(err!=ESP_OK) { ESP_LOGE(TAG,"core callback: %s",esp_err_to_name(err));return; }
    err=d.task.start("core_cfg",CONFIG_CORE_COMMAND_TASK_STACK,CONFIG_CORE_COMMAND_TASK_PRIO,0,1000,
        [](void *ctx,uint32_t) { static_cast<Impl *>(ctx)->run(); },impl);
    if(err!=core_os::kOk) { ESP_LOGE(TAG,"core task: %s",esp_err_to_name(err));return; }
    d.valid=true;
}
CoreLink::~CoreLink() { m_impl->disarm_registration.reset();for(auto &r:m_impl->command_registrations) { r.reset(); } m_impl->registration.reset();m_impl->task.stop();if(m_impl->queue) { vQueueDelete(m_impl->queue); } }
bool CoreLink::isValid() const { return m_impl->valid; }
uint32_t CoreLink::stackHighWater() const { return m_impl->task.stackHighWaterMark(); }
}
