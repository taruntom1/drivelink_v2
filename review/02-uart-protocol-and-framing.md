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

---

## P3. Medium: the setpoint stream does heavy work in RX context

See C4 in [01](01-concurrency-and-lifecycle.md). `handleSetpointStream` takes the manager mutex up to `count` times per frame, with a 50 ms timeout each. One frame with 20 entries can stall RX for up to a second.

---

## P4. Low: protocol edge cases

| Location | Issue |
|----------|-------|
| [uart_comm.cpp:395-416](../components/uart_comm/src/uart_comm.cpp) | `sendData(ch, nullptr, len>0)` calls `memcpy` from null. There is also no way to report failure: it returns `void` even if the component is invalid. |
| [uart_comm.cpp:366-391](../components/uart_comm/src/uart_comm.cpp) | `sendCommand` does not reject `msg_type == 0x00` or `0xFF`, which fall outside every documented range. The doc comment says these ranges are enforced. |
| [motor_manager_uart.cpp:79-83](../components/motor_manager/src/motor_manager_uart.cpp) | The setpoint stream accepts `frame.len > expected_len` (trailing junk is ignored silently). Use `!=` to catch host-side framing bugs. |
| [motor_manager_uart.cpp:121-129](../components/motor_manager/src/motor_manager_uart.cpp) | On a version mismatch, the response echoes `subcommand` and `motor_id` from a payload whose layout is unknown. Harmless, but the host may misinterpret it. |
| [motor_manager_uart.cpp:166-173](../components/motor_manager/src/motor_manager_uart.cpp) | Two "reserved extension slot" floats are required but ignored. The protocol size is fixed at 67 bytes, so a v1 host must send padding. Fine, but document it in the README packet table. |
| [imu_bmo_055.cpp:789-792](../components/imu_bmo_055/src/imu_bmo_055.cpp) | When `len < 2`, the response reports subcommand `SetStreamEnabled` (0) even though the host never sent it. |
| Response coupling | Motor responses carry `motor_id` but no request sequence number. With the config queue depth of 8 and several commands in flight, the host cannot match a response to the request when it sends two commands to the same motor. Add a sequence byte (it fits in the reserved `0x00` at payload[4]). |

---

## P5. Low: CRC implementation is correct but opaque

[frame.cpp:8-13](../components/uart_comm/src/frame.cpp) passes `~0xFFFF` (= 0) into `esp_rom_crc16_be` and inverts the result, relying on the ROM routine inverting input and output internally. The result matches CRC-16/CCITT-FALSE and the Python reference. Replace the NOSONAR comment with one line explaining *why* the double inversion is needed.
