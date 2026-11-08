/**
 * @file test_uart_comm_main.cpp
 * @brief ESP32-S3 test firmware for the uart_comm component.
 *
 * Acts as a command responder on UART0 (TX=GPIO43, RX=GPIO44) at 921600 baud.
 * All communication uses the uart_comm framing protocol.
 * IDF logging is routed to the built-in USB-Serial-JTAG CDC interface (GPIO19/20);
 * connect the ESP32-S3 "USB" port to your PC and open it with idf.py monitor.
 *
 * ─── Test Command Protocol ────────────────────────────────────────────────────
 *
 *  PC → ESP (commands, MSG_TYPE 0x01-0x07):
 *
 *  0x01  CMD_ECHO           Payload: N bytes  → ESP replies RSP_ECHO (0x41) with same payload
 *  0x02  CMD_GET_COUNTERS   Payload: empty    → RSP_COUNTERS (0x42), 24 bytes:
 *                               [crc_errors:u32le][timeouts:u32le]
 *                               [uart_errors:u32le][overflows:u32le]
 *                               [length_errors:u32le][discarded_bytes:u32le]
 *  0x03  CMD_READ_STREAM    Payload: empty    → RSP_STREAM_DATA (0x44)
 *                               If the stream-receive callback has fired since last read:
 *                                 [1:u8][stored_payload...]
 *                               If no data:
 *                                 [0:u8]
 *  0x04  CMD_SENDCMD_STREAM Payload: any      → ESP calls sendCommand(0x80, payload, len)
 *                               and reports:
 *                               RSP_SENDCMD_RESULT (0x45), 1 byte:
 *                                 0x01 = got ESP_ERR_INVALID_ARG (expected)
 *                                 0x00 = any other result (unexpected)
 *  0x05  CMD_TRIGGER_STREAM Payload: N bytes  → ESP calls sendData(0x80, payload, len)
 *                               RSP_TRIGGER_ACK (0x46), 1 byte: 0x01
 *                               Then the stream frame (MSG_TYPE=0x80) is also sent.
 *                               Order: ACK may arrive before stream due to TX priority.
 *  0x06  CMD_BAUD_ACK       Payload: empty    → RSP_BAUD_ACK (0x47), empty payload
 *                               Used to confirm the link is alive after baud-rate change.
 *  0x07  CMD_SEND_INVALID   Payload: empty    → RSP_SEND_INVALID (0x48), 1 byte bitmask.
 *                               The ESP makes four invalid TX calls; bit n is set
 *                               when call n returned ESP_ERR_INVALID_ARG:
 *                                 bit0 sendCommand(0x00, ...)    reserved MSG_TYPE
 *                                 bit1 sendCommand(0xFF, ...)    reserved MSG_TYPE
 *                                 bit2 sendCommand(0x41, nullptr, 3)
 *                                 bit3 sendData(0x80, nullptr, 3)
 *
 *  PC can also send a stream frame (MSG_TYPE=0x80) at any time; the ESP's
 *  callback will capture the payload for readback via CMD_READ_STREAM.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Hardware wiring (no external USB-UART adapter needed):
 *   The UART0 pins are accessible via the board's USB-UART bridge port
 *   (usually labelled "UART" or "COM" on the dev board):
 *     ESP32-S3  GPIO43 (U0TXD)  →  PC USB-UART bridge RXD
 *     ESP32-S3  GPIO44 (U0RXD)  ←  PC USB-UART bridge TXD
 *     GND       ────────────────── GND
 *   Use the SEPARATE "USB" port on the board (USB-Serial-JTAG, GPIO19/20)
 *   for idf.py monitor / logging.  Both ports can be plugged in simultaneously.
 *   If your board has only one USB port it is the USB-Serial-JTAG; in that
 *   case use GPIO43/44 breakout pins with any USB-UART dongle for test comms.
 */

#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "uart_comm.hpp"

static const char* TAG = "uc_test";

using namespace uart_comm;

// ── Test command MSG_TYPE values ─────────────────────────────────────────────
static constexpr uint8_t CMD_ECHO           = 0x01;
static constexpr uint8_t CMD_GET_COUNTERS   = 0x02;
static constexpr uint8_t CMD_READ_STREAM    = 0x03;
static constexpr uint8_t CMD_SENDCMD_STREAM = 0x04;
static constexpr uint8_t CMD_TRIGGER_STREAM = 0x05;
static constexpr uint8_t CMD_BAUD_ACK       = 0x06;
static constexpr uint8_t CMD_SEND_INVALID   = 0x07;

// ── Response MSG_TYPE values ─────────────────────────────────────────────────
static constexpr uint8_t RSP_ECHO           = 0x41;
static constexpr uint8_t RSP_COUNTERS       = 0x42;
static constexpr uint8_t RSP_STREAM_DATA    = 0x44;
static constexpr uint8_t RSP_SENDCMD_RESULT = 0x45;
static constexpr uint8_t RSP_TRIGGER_ACK    = 0x46;
static constexpr uint8_t RSP_BAUD_ACK       = 0x47;
static constexpr uint8_t RSP_SEND_INVALID   = 0x48;

// ── Stream channel used throughout tests ────────────────────────────────────
static constexpr uint8_t STREAM_TEST = 0x80;

// ── Global state shared between callback and main loop ──────────────────────
static volatile bool    g_stream_received = false;
static uint8_t          g_stream_payload[CONFIG_UART_COMM_MAX_PAYLOAD];
static volatile uint8_t g_stream_len      = 0;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "uart_comm test firmware starting");
    ESP_LOGI(TAG, "UART0 TX=GPIO43 RX=GPIO44 @ 115200 baud");
    ESP_LOGI(TAG, "Logging via USB-Serial-JTAG CDC (this port).");
    ESP_LOGI(TAG, "Connect the board UART0 port to PC and run pytest_uart_comm.py");

    // ── Initialise uart_comm with Kconfig defaults ───────────────────────────
    UartCommConfig cfg{};   // all fields come from Kconfig / sdkconfig.defaults
    UartComm comm(cfg);

    if (!comm.isValid()) {
        ESP_LOGE(TAG, "UartComm initialisation failed – halt");
        return;
    }
    ESP_LOGI(TAG, "UartComm ready");

    // ── Register callback for the stream test channel (0x80) ─────────────────
    // Frames arriving on 0x80 are NOT pushed to the RX queue; they go here.
    // stream_cb must be destroyed before comm; declaring it after comm ensures that.
    uart_comm::CallbackRegistration stream_cb;
    comm.registerCallback(STREAM_TEST, [](const RxFrame& f) {
        g_stream_len = f.len;
        std::memcpy(g_stream_payload, f.payload, f.len);
        g_stream_received = true;
    }, stream_cb);

    // ── Main dispatch loop ────────────────────────────────────────────────────
    RxFrame frame{};
    while (true) {
        esp_err_t ret = comm.receive(frame, pdMS_TO_TICKS(15000));
        if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGI(TAG, "Idle – waiting for test commands on UART0...");
            continue;
        }

        switch (frame.msg_type) {

        // ── 0x01  CMD_ECHO ────────────────────────────────────────────────────
        case CMD_ECHO: {
                ESP_LOGI(TAG, "Test: CMD_ECHO started");
            ESP_LOGD(TAG, "CMD_ECHO len=%u", frame.len);
            comm.sendCommand(RSP_ECHO, frame.payload, frame.len);
            break;
        }

        // ── 0x02  CMD_GET_COUNTERS ────────────────────────────────────────────
        case CMD_GET_COUNTERS: {
                ESP_LOGI(TAG, "Test: CMD_GET_COUNTERS started");
            ErrorCounters ec = comm.getErrorCounters();
            uint8_t buf[24];
            std::memcpy(buf + 0,  &ec.crc_errors,      4);
            std::memcpy(buf + 4,  &ec.timeouts,        4);
            std::memcpy(buf + 8,  &ec.uart_errors,     4);
            std::memcpy(buf + 12, &ec.overflows,       4);
            std::memcpy(buf + 16, &ec.length_errors,   4);
            std::memcpy(buf + 20, &ec.discarded_bytes, 4);
            comm.sendCommand(RSP_COUNTERS, buf, sizeof(buf));
            break;
        }

        // ── 0x03  CMD_READ_STREAM ─────────────────────────────────────────────
        // Reports the last payload captured by the 0x80 callback.
        // First byte is a has-data flag; remaining bytes are the payload.
        case CMD_READ_STREAM: {
                ESP_LOGI(TAG, "Test: CMD_READ_STREAM started");
            uint8_t buf[1 + CONFIG_UART_COMM_MAX_PAYLOAD];
            uint8_t reply_len = 0;
            if (g_stream_received) {
                buf[0] = 0x01;
                uint8_t plen = g_stream_len;
                std::memcpy(&buf[1], g_stream_payload, plen);
                reply_len = static_cast<uint8_t>(1u + plen);
                // Clear for the next test
                g_stream_received = false;
                g_stream_len      = 0;
            } else {
                buf[0]   = 0x00;
                reply_len = 1;
            }
            comm.sendCommand(RSP_STREAM_DATA, buf, reply_len);
            break;
        }

        // ── 0x04  CMD_SENDCMD_STREAM ──────────────────────────────────────────
        // Verifies that sendCommand() with a stream MSG_TYPE is rejected.
        case CMD_SENDCMD_STREAM: {
                ESP_LOGI(TAG, "Test: CMD_SENDCMD_STREAM started");
            esp_err_t result = comm.sendCommand(STREAM_TEST, frame.payload, frame.len);
            uint8_t ok = (result == ESP_ERR_INVALID_ARG) ? 0x01 : 0x00;
            comm.sendCommand(RSP_SENDCMD_RESULT, &ok, 1);
            break;
        }

        // ── 0x05  CMD_TRIGGER_STREAM ──────────────────────────────────────────
        // Asks the ESP to transmit a stream frame (sendData) back to PC.
        // PC should read two frames: RSP_TRIGGER_ACK and the stream frame (0x80).
        case CMD_TRIGGER_STREAM: {
                ESP_LOGI(TAG, "Test: CMD_TRIGGER_STREAM started");
            comm.sendData(STREAM_TEST, frame.payload, frame.len);
            uint8_t ok = 0x01;
            comm.sendCommand(RSP_TRIGGER_ACK, &ok, 1);
            break;
        }

        // ── 0x06  CMD_BAUD_ACK ────────────────────────────────────────────────
        // Simple alive-ping used after baud-rate renegotiation.
        case CMD_BAUD_ACK: {
                ESP_LOGI(TAG, "Test: CMD_BAUD_ACK started");
            comm.sendCommand(RSP_BAUD_ACK, nullptr, 0);
            break;
        }

        // ── 0x07  CMD_SEND_INVALID ────────────────────────────────────────────
        // Verifies that the TX API rejects reserved MSG_TYPEs and null payloads.
        case CMD_SEND_INVALID: {
            ESP_LOGI(TAG, "Test: CMD_SEND_INVALID started");
            const uint8_t dummy[3] = {0x01, 0x02, 0x03};
            uint8_t mask = 0;
            if (comm.sendCommand(0x00, dummy, sizeof(dummy)) == ESP_ERR_INVALID_ARG) {
                mask |= 0x01;
            }
            if (comm.sendCommand(0xFF, dummy, sizeof(dummy)) == ESP_ERR_INVALID_ARG) {
                mask |= 0x02;
            }
            if (comm.sendCommand(RSP_ECHO, nullptr, 3) == ESP_ERR_INVALID_ARG) {
                mask |= 0x04;
            }
            if (comm.sendData(STREAM_TEST, nullptr, 3) == ESP_ERR_INVALID_ARG) {
                mask |= 0x08;
            }
            comm.sendCommand(RSP_SEND_INVALID, &mask, 1);
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown test command 0x%02X – ignoring", frame.msg_type);
            break;
        }
    }
}
