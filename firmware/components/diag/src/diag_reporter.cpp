#include "diag_reporter.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "diag.hpp"
#include "diag_internal.hpp"
#include "diag_protocol.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "stoppable_task.hpp"
#include "tick_util.hpp"
#include "uart_comm.hpp"

static const char *const TAG = "diag";

namespace diag {

namespace {

constexpr size_t kMaxPayload = CONFIG_UART_COMM_MAX_PAYLOAD;
static_assert(kMaxPayload >= protocol::kMaxFixedEventSize,
              "CONFIG_UART_COMM_MAX_PAYLOAD is too small for a diag Record event");
static_assert(kMaxPayload >= protocol::kMaxInfoResponseSize,
              "CONFIG_UART_COMM_MAX_PAYLOAD is too small for the diag GetInfo response");
static_assert(detail::kRuntimeSlots < protocol::kNoNextPage, "slot indices must stay below the GetEntries end marker");

// Never more entries than slots: with 1024-byte frames a page could list 127
// entries, but only detail::kRuntimeSlots exist (and the arrays live on the task stack).
constexpr size_t kMaxCounterEntries = std::min(protocol::maxCounterEntries(kMaxPayload), detail::kRuntimeSlots);
constexpr size_t kMaxPageEntries = std::min(protocol::maxEntriesPerPage(kMaxPayload), detail::kRuntimeSlots);
constexpr size_t kMaxResponseBody = kMaxPayload - protocol::kQueryResponseHeaderSize;

/// After a failed event send (UART TX queue full), wait this long before
/// trying again, so a congested link isn't hammered every poll.
constexpr int64_t kTxBackoffUs = 100 * 1000;

bool inRange(uint8_t value, uint8_t lo, uint8_t hi)
{
    return value >= lo && value <= hi;
}

} // namespace

struct Reporter::Impl {
    Impl(uart_comm::UartComm &uart_in, const ReporterConfig &config_in) : uart(uart_in), config(config_in) {}

    uart_comm::UartComm &uart;
    const ReporterConfig config;
    QueueHandle_t query_queue = nullptr;
    uart_comm::CallbackRegistration query_registration;
    rtos_util::StoppableTask task;
    bool valid = false;

    // Reporter-task state only.
    uint16_t seq = 0;
    bool boot_pending = true;
    int64_t tx_backoff_until_us = 0;
    std::array<uint32_t, detail::kRuntimeSlots> last_sent{};
    std::array<detail::SlotSnapshot,detail::kRuntimeSlots> snaps{};
    std::array<uint32_t,detail::kRuntimeSlots> now{};
    uart_comm::ErrorCounters uart_last{};

    // Reporter-task buffers, each about MAX_PAYLOAD (1 KB). They live here,
    // allocated once with the Impl, not on the task's stack. tx_buf holds one
    // outgoing payload at a time (an event, or a query response).
    uart_comm::RxFrame query_frame{};
    std::array<uint8_t, kMaxResponseBody> resp_body{};
    std::array<uint8_t, kMaxPayload> tx_buf{};

    void onQueryFrame(const uart_comm::RxFrame &frame);
    void taskLoop();

    bool sendEvent(const uint8_t *payload, size_t len);
    void sendBoot();
    void drainEvents();
    void handleQueries();
    void handleQuery(const uart_comm::RxFrame &frame);
    size_t encodeInfo(uint8_t *out, size_t cap) const;
    size_t encodeEntriesPage(uint8_t start, uint8_t *out, size_t cap);
    void applyClear(uint8_t what);
    void foldUartCounters();
    void sendCounterDeltas();
};

// ── Construction / destruction ──────────────────────────────────────────

Reporter::Reporter(uart_comm::UartComm &uart, const ReporterConfig &config)
    : m_impl(std::make_unique<Impl>(uart, config))
{
    init();
    Impl &d = *m_impl;

    if (!uart.isValid()) {
        ESP_LOGE(TAG, "UartComm is invalid; diag reporter not started");
        return;
    }
    if (!inRange(config.event_msg_type, 0xC0, 0xFE) || !inRange(config.query_msg_type, 0x01, 0x3F) ||
        !inRange(config.query_response_msg_type, 0x40, 0x7F)) {
        ESP_LOGE(TAG, "Invalid MSG_TYPEs: event=0x%02X (0xC0-0xFE) query=0x%02X (0x01-0x3F) "
                      "response=0x%02X (0x40-0x7F)",
                 config.event_msg_type, config.query_msg_type, config.query_response_msg_type);
        return;
    }

    d.query_queue = xQueueCreate(CONFIG_DIAG_QUERY_QUEUE_DEPTH, sizeof(uart_comm::RxFrame));
    if (d.query_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create query queue (depth=%d)", CONFIG_DIAG_QUERY_QUEUE_DEPTH);
        return;
    }

    Impl *const impl = &d;
    esp_err_t ret = uart.registerCallback(
        config.query_msg_type, [impl](const uart_comm::RxFrame &frame) { impl->onQueryFrame(frame); },
        d.query_registration);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register query callback (msg=0x%02X): %s", config.query_msg_type,
                 esp_err_to_name(ret));
        return;
    }

    ret = d.task.start("diag", config.task_stack_size, config.task_priority, [impl]() { impl->taskLoop(); });
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start reporter task: %s", esp_err_to_name(ret));
        d.query_registration.reset();
        return;
    }

    d.valid = true;
}

Reporter::~Reporter()
{
    if (!m_impl) {
        return;
    }
    m_impl->query_registration.reset();
    m_impl->task.stop();
    if (m_impl->query_queue != nullptr) {
        vQueueDelete(m_impl->query_queue);
        m_impl->query_queue = nullptr;
    }
}

bool Reporter::isValid() const
{
    return m_impl && m_impl->valid;
}

// ── RX task ─────────────────────────────────────────────────────────────

void Reporter::Impl::onQueryFrame(const uart_comm::RxFrame &frame)
{
    // RX task: hand off without blocking.
    if (xQueueSend(query_queue, &frame, 0) != pdTRUE) {
        DIAG_COUNT(codes::diag::kQueryDropped);
        return;
    }
    task.notify();
}

// ── Reporter task ───────────────────────────────────────────────────────

void Reporter::Impl::taskLoop()
{
    const TickType_t poll_ticks = rtos_util::msToTicksCeil(CONFIG_DIAG_REPORTER_POLL_MS);
    const int64_t period_us = static_cast<int64_t>(CONFIG_DIAG_COUNTER_PERIOD_MS) * 1000;
    int64_t next_scan_us = esp_timer_get_time() + period_us;

    while (!task.stopRequested()) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= tx_backoff_until_us) {
            if (boot_pending) {
                sendBoot();
            }
            if (!boot_pending) {
                drainEvents();
            }
        }

        handleQueries();

        if (now_us >= next_scan_us) {
            foldUartCounters();
            if (now_us >= tx_backoff_until_us) {
                sendCounterDeltas();
            }
            next_scan_us += period_us;
            if (next_scan_us <= now_us) {
                next_scan_us = now_us + period_us;
            }
        }

        // Woken early by a query (notify) or by stop().
        (void)ulTaskNotifyTake(pdTRUE, poll_ticks);
    }
}

bool Reporter::Impl::sendEvent(const uint8_t *payload, size_t len)
{
    if (len == 0U || len > kMaxPayload) {
        return false;
    }
    if (uart.sendCommand(config.event_msg_type, payload, len) != ESP_OK) {
        tx_backoff_until_us = esp_timer_get_time() + kTxBackoffUs;
        return false;
    }
    ++seq; // only frames that left are numbered, so a gap means loss on the link
    return true;
}

void Reporter::Impl::sendBoot()
{
    protocol::BootEvent ev{};
    ev.timestamp_us = detail::wallClockUs();
    ev.reset_reason = static_cast<uint8_t>(esp_reset_reason());
    ev.instances_ok=config.instances_ok;ev.instances_failed=config.instances_failed;ev.safe_mode=config.safe_mode;

    const size_t len = protocol::encodeBootEvent(seq, detail::currentFlags(), ev, tx_buf.data(), tx_buf.size());
    if (sendEvent(tx_buf.data(), len)) {
        boot_pending = false;
    }
}

void Reporter::Impl::drainEvents()
{
    // Bounded: at most one queue's worth per pass.
    for (int i = 0; i < CONFIG_DIAG_RECORD_QUEUE_DEPTH; ++i) {
        detail::QueuedEvent ev{};
        if (!detail::peekEvent(ev)) {
            return;
        }
        if (ev.slot >= detail::kRuntimeSlots) {
            detail::popEvent();
            continue;
        }
        const Code &code = kCodeTable[detail::table().codeSlot(ev.slot)];

        uint8_t *buf = tx_buf.data();
        const size_t cap = tx_buf.size();
        size_t len = 0;
        if (ev.kind == detail::QueuedKind::Record) {
            protocol::RecordEvent rec{};
            rec.timestamp_us = ev.timestamp_us;
            rec.severity = static_cast<uint8_t>(code.severity);
            rec.source = static_cast<uint8_t>(code.source);
            rec.instance = detail::table().instance(ev.slot);
            rec.code = code.code;
            rec.esp_err = ev.esp_err;
            rec.arg0 = ev.arg0;
            rec.arg1 = ev.arg1;
            len = protocol::encodeRecordEvent(seq, ev.flags, rec, buf, cap);
        } else {
            protocol::ClearedEvent clr{};
            clr.timestamp_us = ev.timestamp_us;
            clr.source = static_cast<uint8_t>(code.source);
            clr.instance = detail::table().instance(ev.slot);
            clr.code = code.code;
            len = protocol::encodeClearedEvent(seq, ev.flags, clr, buf, cap);
        }

        if (!sendEvent(buf, len)) {
            return; // TX queue full: keep the event queued and retry later
        }
        detail::popEvent();
    }
}

void Reporter::Impl::handleQueries()
{
    for (int i = 0; i < CONFIG_DIAG_QUERY_QUEUE_DEPTH; ++i) {
        if (xQueueReceive(query_queue, &query_frame, 0) != pdTRUE) {
            return;
        }
        handleQuery(query_frame);
    }
}

void Reporter::Impl::handleQuery(const uart_comm::RxFrame &frame)
{
    protocol::QueryHeader header{};
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    protocol::ResultCode result = protocol::ResultCode::InvalidArgument;
    size_t resp_body_len = 0;

    if (protocol::decodeQueryHeader(frame.payload, frame.len, header, body, body_len)) {
        switch (static_cast<protocol::QuerySubcommand>(header.subcommand)) {
            case protocol::QuerySubcommand::GetInfo:
                if (protocol::decodeGetInfo(body, body_len)) {
                    resp_body_len = encodeInfo(resp_body.data(), resp_body.size());
                    result = protocol::ResultCode::Ok;
                }
                break;
            case protocol::QuerySubcommand::GetEntries: {
                uint8_t start = 0;
                if (protocol::decodeGetEntries(body, body_len, start)) {
                    resp_body_len = encodeEntriesPage(start, resp_body.data(), resp_body.size());
                    result = protocol::ResultCode::Ok;
                }
                break;
            }
            case protocol::QuerySubcommand::Clear: {
                uint8_t what = 0;
                if (protocol::decodeClear(body, body_len, what)) {
                    applyClear(what);
                    result = protocol::ResultCode::Ok;
                }
                break;
            }
            default:
                result = protocol::ResultCode::NotSupported;
                break;
        }
    }

    const size_t len = protocol::encodeQueryResponse(header, result, resp_body.data(), resp_body_len, tx_buf.data(),
                                                     tx_buf.size());
    if (len == 0U || uart.sendCommand(config.query_response_msg_type, tx_buf.data(), len) != ESP_OK) {
        DIAG_COUNT(codes::diag::kEventTxDropped);
    }
}

size_t Reporter::Impl::encodeInfo(uint8_t *out, size_t cap) const
{
    const esp_app_desc_t *app = esp_app_get_description();
    detail::SlotTable &table = detail::table();

    protocol::InfoBody info{};
    info.flags = detail::currentFlags();
    info.uptime_us = static_cast<uint64_t>(esp_timer_get_time());
    info.reset_reason = static_cast<uint8_t>(esp_reset_reason());
    info.instances_ok=config.instances_ok;info.instances_failed=config.instances_failed;info.safe_mode=config.safe_mode;
    info.records_dropped = table.read(codes::diag::kRecordDropped.slot).count;
    info.events_tx_dropped = table.read(codes::diag::kEventTxDropped.slot).count;
    info.version = app->version;
    info.version_len = strnlen(app->version, sizeof(app->version));
    return protocol::encodeInfoBody(info, out, cap);
}

size_t Reporter::Impl::encodeEntriesPage(uint8_t start, uint8_t *out, size_t cap)
{

    const detail::SlotTable &table = detail::table();
    for (size_t slot = 0; slot < detail::kRuntimeSlots; ++slot) {
        snaps[slot] = table.read(slot);
    }

    std::array<protocol::EntryState, kMaxPageEntries> entries{};
    uint8_t total = 0;
    uint8_t next = protocol::kNoNextPage;
    const size_t n = detail::collectEntries(kCodeTable.data(), snaps.data(), detail::kRuntimeSlots, start, entries.data(),
                                            entries.size(), total, next, &table);
    return protocol::encodeEntriesBody(total, next, entries.data(), n, out, cap);
}

void Reporter::Impl::applyClear(uint8_t what)
{
    detail::SlotTable &table = detail::table();
    if ((what & protocol::kClearOccurred) != 0U) {
        table.clearOccurred();
    }
    if ((what & protocol::kClearCounts) != 0U) {
        table.resetCounts();
        last_sent.fill(0U);
    }
}

void Reporter::Impl::foldUartCounters()
{
    // getErrorCounters() is cumulative since UartComm started; fold in the
    // change since the last read. uart_last starts at zero, so link errors
    // from before the reporter started are counted too.
    const uart_comm::ErrorCounters now = uart.getErrorCounters();
    detail::SlotTable &table = detail::table();
    auto fold = [&table](const Code &code, uint32_t value, uint32_t &last) {
        table.add(code.slot, value - last);
        last = value;
    };
    fold(codes::uart_comm::kStreamSkipped, now.stream_skipped, uart_last.stream_skipped);
    fold(codes::uart_comm::kCrcError, now.crc_errors, uart_last.crc_errors);
    fold(codes::uart_comm::kPartialFrame, now.timeouts, uart_last.timeouts);
    fold(codes::uart_comm::kFrameOrParityError, now.uart_errors, uart_last.uart_errors);
    fold(codes::uart_comm::kRxOverflow, now.overflows, uart_last.overflows);
    fold(codes::uart_comm::kLengthError, now.length_errors, uart_last.length_errors);
    fold(codes::uart_comm::kDiscardedBytes, now.discarded_bytes, uart_last.discarded_bytes);
    fold(codes::uart_comm::kHeaderCrcError, now.header_crc_errors, uart_last.header_crc_errors);
}

void Reporter::Impl::sendCounterDeltas()
{

    const detail::SlotTable &table = detail::table();
    for (size_t slot = 0; slot < detail::kRuntimeSlots; ++slot) {
        now[slot] = table.read(slot).count;
    }

    size_t cursor = 0;
    for (;;) {
        std::array<protocol::CounterEntry, kMaxCounterEntries> entries{};
        std::array<uint8_t, kMaxCounterEntries> slots{};
        const size_t n = detail::collectCounterDeltas(kCodeTable.data(), now.data(), last_sent.data(), detail::kRuntimeSlots,
                                                      cursor, entries.data(), slots.data(), entries.size(), &table);
        if (n == 0U) {
            return;
        }

        const size_t len = protocol::encodeCounterDeltaEvent(seq, detail::currentFlags(), detail::wallClockUs(),
                                                             entries.data(), n, tx_buf.data(), tx_buf.size());
        if (!sendEvent(tx_buf.data(), len)) {
            return; // retried at the next period, with the deltas accumulated
        }
        for (size_t i = 0; i < n; ++i) {
            last_sent[slots[i]] = now[slots[i]];
        }
    }
}

} // namespace diag
