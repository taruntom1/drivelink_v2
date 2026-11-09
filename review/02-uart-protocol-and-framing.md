# 02: UART Protocol and Framing (`uart_comm` and the protocols built on it)

Frame format: `AA AA AA | MSG_TYPE | LEN | PAYLOAD[LEN] | CRC16-LE`. The start of a frame is found by the UART peripheral's AT-command pattern detector. The parser then reads the header and body with `uart_read_bytes`.

---

## P1. High: `UART_DATA` handling flushes the whole RX ring buffer

[parser.cpp:53-56](../components/uart_comm/src/parser.cpp)

```cpp
case UART_DATA:
    // Data without pattern detection — discard stray bytes
    uart_flush_input(m_port);
```

`UART_DATA` events are not "stray bytes". The driver posts one for every RX-timeout or FIFO-full interrupt, including the bytes that belong to a valid frame. Events are processed in FIFO order, so a typical burst looks like this:

```
PATTERN_DET(F1)  DATA(F1)  PATTERN_DET(F2)  DATA(F2)
```

1. `PATTERN_DET(F1)`: the parser reads F1. Meanwhile F2 has already arrived in the ring buffer.
2. `DATA(F1)`: `uart_flush_input()` **deletes F2**.
3. `PATTERN_DET(F2)`: the stored position is now stale, so the parser returns -1 or reads garbage. F2 is lost.

Whenever the RX task falls behind by one event (a slow callback, a blocked mutex as in C4, or simply a 100 Hz setpoint stream interleaved with commands), whole frames disappear without any counter being incremented.

The test suite does not catch this because every test sends one frame and waits for the reply (see [09-testing.md](09-testing.md)).

**Fix**: on `UART_DATA`, do nothing (or just note `event.size`). Discard bytes only up to the next pattern position: `uart_pattern_get_pos()` tells you where it is. Junk before a pattern is already discarded by `handlePatternDetected`. If there is no pending pattern and the buffer grows past, say, `kMaxFrameSize`, discard just that many bytes.

The same "flush everything" reflex appears when the payload length is too large ([parser.cpp:97-101](../components/uart_comm/src/parser.cpp)). That also wipes valid frames that follow, and the event is counted as a `crc_error`, which is misleading. Add a `length_errors` counter.

**Status: fixed** (together with P2). The parser was rewritten as a software scanner and hardware pattern detection was removed. Driver events now only wake the RX task. `Parser::nextFrame()` pulls bytes from the ring buffer into a two-frame window and scans for `AA AA AA | MSG_TYPE | LEN | PAYLOAD | CRC`. Nothing is ever flushed: an oversized LEN or a CRC mismatch drops only the candidate's first byte and rescans. A partial frame that gets no new bytes for 100 ms is dropped (`timeouts`). `UART_BUFFER_FULL` no longer flushes either (the driver stashes the FIFO, so no bytes are lost), and `UART_FIFO_OVF` relies on the CRC to resync. New counters: `length_errors` and `discarded_bytes`. `CONFIG_UART_COMM_PATTERN_QUEUE_SIZE` became `CONFIG_UART_COMM_EVENT_QUEUE_SIZE`. Hardware tests TC-16 to TC-18 cover a back-to-back burst, an oversized LEN before a valid frame, and a truncated frame before a valid frame.

---

## P2. Medium: the start pattern is not unique, and is detected with zero idle gaps

[uart_comm.cpp:203-204](../components/uart_comm/src/uart_comm.cpp)

```cpp
uart_enable_pattern_det_baud_intr(port, 0xAA, 3, /*chr_tout*/9, /*post_idle*/0, /*pre_idle*/0);
```

No byte stuffing is applied, so any `AA AA AA` inside a payload or CRC fires the detector. Examples: the float 5.3e-13 is `0x2B AA AA AA`, and a CRC low byte `0xAA` right before the next preamble also matches. With `pre_idle = post_idle = 0`, the hardware has no gap to tell a preamble from data.

The effects are extra `PATTERN_DET` events and position-queue entries. Usually the driver's position update makes them stale and harmless. But when a false pattern sits inside a body the parser is still waiting for, it gets popped as the next frame start, and the following real frame is misaligned. Only CRC saves you, and P1 then flushes the buffer.

**Options (pick one)**
- Byte-stuff `0xAA` in the payload/CRC (COBS or SLIP-style), so three `0xAA` in a row can only be a preamble.
- Or require an idle gap before the preamble (`pre_idle` > 0) and have the host insert one. That costs some bandwidth.
- At minimum, after a CRC failure, resync by scanning for the next valid header instead of trusting the next pattern position.

**Status: fixed** with the third option, done fully in software (see P1). The wire format is unchanged and there is no byte stuffing, so false `AA AA AA` sequences still occur inside payloads, but they can no longer cost a real frame: a rejected candidate drops one byte and the scan resumes inside it. A candidate whose MSG_TYPE byte is `0xAA` (a run of four or more start bytes) is not counted as an error when rejected, because it is usually a stray start byte just before a real preamble. The residual risk is a false candidate that passes the 16-bit CRC by chance (about 1 in 65536 per false candidate), the same as any CRC-framed protocol without stuffing.

---

## P3. Medium: the setpoint stream does heavy work in RX context

See C4 in [01](01-concurrency-and-lifecycle.md). `handleSetpointStream` takes the manager mutex up to `count` times per frame, with a 50 ms timeout each. One frame with 20 entries can stall RX for up to a second.

**Status: fixed** by C4 (setpoints go into a mailbox that the update task applies).

---

## P4. Low: protocol edge cases

| Location | Issue | Status |
|----------|-------|--------|
| [uart_comm.cpp:395-416](../components/uart_comm/src/uart_comm.cpp) | `sendData(ch, nullptr, len>0)` calls `memcpy` from null. There is also no way to report failure: it returns `void` even if the component is invalid. | **Fixed.** `sendData` returns `esp_err_t` and rejects a null payload with `len > 0`, a `len` above the max (it used to be clamped silently) and a non-stream channel. |
| [uart_comm.cpp:366-391](../components/uart_comm/src/uart_comm.cpp) | `sendCommand` does not reject `msg_type == 0x00` or `0xFF`, which fall outside every documented range. The doc comment says these ranges are enforced. | **Fixed.** Reserved types and a null payload with `len > 0` return `ESP_ERR_INVALID_ARG`. Covered by TC-19. |
| [motor_manager_uart.cpp:79-83](../components/motor_manager/src/motor_manager_uart.cpp) | The setpoint stream accepts `frame.len > expected_len` (trailing junk is ignored silently). Use `!=` to catch host-side framing bugs. | **Fixed.** |
| [motor_manager_uart.cpp:121-129](../components/motor_manager/src/motor_manager_uart.cpp) | On a version mismatch, the response echoes `subcommand` and `motor_id` from a payload whose layout is unknown. Harmless, but the host may misinterpret it. | **Fixed.** When the header is too short or has the wrong version, the response carries `subcommand = motor_id = 0xFF` (`kUnparsedHeaderByte`). Short payloads now get this response too, instead of being dropped silently. |
| [motor_manager_uart.cpp:166-173](../components/motor_manager/src/motor_manager_uart.cpp) | Two "reserved extension slot" floats are required but ignored. The protocol size is fixed at 67 bytes, so a v1 host must send padding. Fine, but document it in the README packet table. | **Fixed** (documented in the motor_manager README). **No longer applicable:** protocol v3 removed both reserved floats and `sample_time_ms`. The CREATE/CONFIGURE body is now 64 bytes and must be exactly that size. |
| [imu_bmo_055.cpp:789-792](../components/imu_bmo_055/src/imu_bmo_055.cpp) | When `len < 2`, the response reports subcommand `SetStreamEnabled` (0) even though the host never sent it. | **Fixed.** The IMU uses the same `0xFF` sentinel for an unparsed header. |
| Response coupling | Motor responses carry `motor_id` but no request sequence number. With the config queue depth of 8 and several commands in flight, the host cannot match a response to the request when it sends two commands to the same motor. Add a sequence byte (it fits in the reserved `0x00` at payload[4]). | **Fixed** as protocol v2 for motor_manager and IMU. Requests are `[ver][seq][sub][motor_id][body]` (IMU: `[ver][seq][sub][body]`). Responses are `[ver][seq][sub][motor_id][result]` (IMU: `[ver][seq][sub][result][extra]`), and the motor response's reserved byte is gone. `kProtocolVersion` is `0x02` for each component as a whole, streams included. time_sync is unchanged: its ESP-initiated request already carries a `u16 request_id` that is echoed and checked. **Breaking:** hosts must move to v2. |

---

## P5. Low: CRC implementation is correct but opaque

[frame.cpp:8-13](../components/uart_comm/src/frame.cpp) passes `~0xFFFF` (= 0) into `esp_rom_crc16_be` and inverts the result, relying on the ROM routine inverting input and output internally. The result matches CRC-16/CCITT-FALSE and the Python reference. Replace the NOSONAR comment with one line explaining *why* the double inversion is needed.

**Status: fixed.**
