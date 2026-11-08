# 09: Testing

## What exists

`test/uart_comm/` is a standalone ESP-IDF app plus a pytest suite (15 tests) that drives a real board over serial:
- echo of 0, 1, N and max payload
- CRC error counting and counter isolation
- stream RX through a callback, stream TX from the device, CRC of device-sent frames
- `sendCommand` rejecting stream types
- stray bytes before a frame

The protocol is re-implemented in Python ([protocol.py](../test/uart_comm/protocol.py)) with a table-driven CRC that matches the firmware. That is a good, independent oracle.

## Gaps (ranked by the risk they leave uncovered)

| # | Gap | Why it matters |
|---|-----|----------------|
| G1 | **No back-to-back or burst traffic tests.** Every test is request → wait for response. | This is exactly the condition that triggers P1 (the `UART_DATA` flush drops frames). Add: send 50 echo frames without waiting, then expect 50 replies in order. Also: interleave a 100 Hz stream with commands. **Partly done:** TC-16 sends 8 echo frames in one write (8 is the default RX delivery and TX command queue depth; more would overflow the test firmware's queues, not the parser), with false preambles in the payloads. TC-17/TC-18 cover resync after an oversized LEN and a truncated frame. The interleaved stream test is still open. |
| G2 | **No payloads containing `AA AA AA`.** | This is the condition that triggers P2. Add echo tests with payloads `AA AA AA`, `xx AA AA`, plus a frame whose CRC low byte is `0xAA` (search for such a payload in Python). |
| G3 | No tests for motor_manager, time_sync or the IMU protocol parsers. | `parseAndDispatchConfigure`, `handleSetpointStream`, `decodeCalibrationBlob`, `encodeStreamSample`, `time_sync::protocol::*` and `encodeTelemetryLocked` are pure byte-in/byte-out functions. They can be unit-tested on the host (IDF `linux` target, or plain GoogleTest against the headers) with no hardware. Length-boundary cases (67-byte CreateMotor, short frames, `count` overflow) are the obvious first cases. |
| G4 | No PID unit tests. | The PID is pure math. Step-response, anti-windup, derivative-kick and `dt`-scaling tests would have caught M2. |
| G5 | No teardown tests. | Constructing and destroying each manager in a loop on target would immediately hit C1 / C2 (double `vTaskDelete`). |
| G6 | Length-limit test does not cover `LEN > CONFIG_UART_COMM_MAX_PAYLOAD`. | The parser's oversize path flushes the buffer. Assert that a valid frame sent right after an oversized one is still received (it currently isn't, because of the flush). |
| G7 | No CI. | Nothing builds the firmware or the test app automatically. A GitHub Actions job with `espressif/idf:v5.5.2` running `idf.py build` for `/` and `test/uart_comm` would at least catch build breaks such as I3 (`bno055_support.c`). |

## Harness inconsistencies

- [test_uart_comm_main.cpp:5](../test/uart_comm/main/test_uart_comm_main.cpp) and [conftest.py:12](../test/uart_comm/conftest.py) say **921600 baud**. But `conftest.py` defaults `--baud` to **115200**, the Kconfig default is 115200, and neither `sdkconfig.defaults` sets `CONFIG_UART_COMM_BAUD_RATE`. Pick one value and set it in `test/uart_comm/sdkconfig.defaults`.
- The test app depends on the whole `components/` dir via `EXTRA_COMPONENT_DIRS`, so it builds every component (motor, IMU, …). Limit it with `set(COMPONENTS main uart_comm)` to speed up builds and avoid unrelated breakage.
