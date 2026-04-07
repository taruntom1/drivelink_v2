# uart_comm

## Overview
`uart_comm` is a framed UART transport for ESP-IDF with CRC16 integrity, callback-based dispatch, queue-based receive, and dual TX paths (FIFO command queue + latest-wins stream channels).

## Features
- Frame format with start marker, header, payload, CRC
- Hardware-assisted frame detection via UART pattern interrupt
- Per-message-type callbacks
- Pull-based RX queue for non-callback frames
- Prioritized command TX and low-latency stream TX
- Runtime error counters

## Dependencies
Required:
- `driver`
- `esp_driver_uart`

Optional:
- None

## Configuration
| Symbol | Type | Default | Description |
|--------|------|---------|-------------|
| `CONFIG_UART_COMM_PORT_NUM` | int | 1 | UART peripheral number. |
| `CONFIG_UART_COMM_TX_PIN` | int | 17 | TX GPIO. |
| `CONFIG_UART_COMM_RX_PIN` | int | 18 | RX GPIO. |
| `CONFIG_UART_COMM_BAUD_RATE` | int | 115200 | UART baud rate. |
| `CONFIG_UART_COMM_MAX_PAYLOAD` | int | 128 | Maximum protocol payload bytes. |
| `CONFIG_UART_COMM_RX_BUF_SIZE` | int | 1024 | UART RX ring buffer size. |
| `CONFIG_UART_COMM_TX_BUF_SIZE` | int | 1024 | UART TX ring buffer size. |
| `CONFIG_UART_COMM_CMD_QUEUE_DEPTH` | int | 8 | TX command queue depth. |
| `CONFIG_UART_COMM_RX_QUEUE_DEPTH` | int | 8 | RX delivery queue depth. |
| `CONFIG_UART_COMM_RX_TASK_STACK` | int | 4096 | RX task stack size. |
| `CONFIG_UART_COMM_RX_TASK_PRIO` | int | 5 | RX task priority. |
| `CONFIG_UART_COMM_TX_TASK_STACK` | int | 4096 | TX task stack size. |
| `CONFIG_UART_COMM_TX_TASK_PRIO` | int | 5 | TX task priority. |
| `CONFIG_UART_COMM_PATTERN_QUEUE_SIZE` | int | 20 | UART pattern queue depth. |
| `CONFIG_UART_COMM_RX_TIMEOUT_SYMBOLS` | int | 10 | UART RX timeout threshold. |

## Public API
Public header: `include/uart_comm.hpp`

- `UartComm(const UartCommConfig&)`, destructor, move operations
- `bool isValid() const`
- `esp_err_t sendCommand(uint8_t msg_type, const uint8_t* payload, uint8_t len)`
- `void sendData(uint8_t channel, const uint8_t* payload, uint8_t len)`
- `esp_err_t registerCallback(uint8_t msg_type, RxCallback callback)`
- `esp_err_t receive(RxFrame& frame, TickType_t timeout)`
- `ErrorCounters getErrorCounters() const`

Wire ranges:
- Commands `0x01..0x3F`
- Responses `0x40..0x7F`
- Streams `0x80..0xBF`
- Events `0xC0..0xFE`

## Usage Example
```cpp
#include "uart_comm.hpp"

uart_comm::UartComm comm({});
if (!comm.isValid()) {
    return;
}

uint8_t payload[] = {1, 2, 3};
comm.sendCommand(0x01, payload, sizeof(payload));
comm.sendData(0x80, payload, sizeof(payload));
```

## Error Handling
- `sendCommand` can return `ESP_ERR_INVALID_STATE`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_NO_MEM`
- `receive` returns `ESP_ERR_TIMEOUT` when queue is empty at timeout
- RX parser increments CRC/timeout/UART/overflow counters

## Thread Safety
- Stream slots and callback map are mutex-protected.
- Tasks communicate via FreeRTOS queues/notifications.
- Callback execution happens in RX task context.

## Limitations & Known Issues
- Start marker is fixed (`0xAA 0xAA 0xAA`).
- Frame payload max is compile-time bounded by Kconfig.
