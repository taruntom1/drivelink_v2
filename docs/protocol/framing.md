# Framing (link layer)

How every message between the host and the ESP32-S3 is put on the wire. The payloads inside the frames are defined by each protocol (the legacy managers' code and host tests until they are ported, then the core's families). The MSG_TYPE and stream channel assignments are in [registry.md](registry.md), generated from the definitions described in [definitions.md](definitions.md). The diag codes are in [diag-codes.md](diag-codes.md).

The C++ implementation is `protocol/` (`framing.hpp`, `crc.hpp`, `scanner.hpp`), shared by the firmware and the host. The Python reference used by the hardware tests is `firmware/test/uart_comm/protocol.py`. Both are tested against the golden frames below.

## Physical link

- UART, 8N1, no flow control, **921600 baud** by default (`CONFIG_UART_COMM_BAUD_RATE`). The rate is fixed at compile time. Both ends must use the same value.
- On the board: UART0, TX GPIO43, RX GPIO44. The console runs on USB-Serial-JTAG, so logs never mix with frames.

## Frame

```
AA AA AA | MSG_TYPE u8 | LEN u16 | HCRC u8 | payload (LEN bytes) | CRC16 u16
```

| Field | Size | Content |
|---|---|---|
| Preamble | 3 | `AA AA AA`. Not escaped: the same bytes may appear inside a payload. |
| `MSG_TYPE` | 1 | Message type, see the ranges below |
| `LEN` | 2 | Payload length, little-endian, 0 to `MAX_PAYLOAD` |
| `HCRC` | 1 | CRC-8 over `MSG_TYPE` and `LEN` (3 bytes) |
| payload | `LEN` | Protocol data. Every protocol's payload starts with its version byte. |
| `CRC16` | 2 | CRC-16 over `MSG_TYPE`, `LEN`, `HCRC` and the payload, little-endian |

The overhead is 9 bytes per frame. All multi-byte fields, in the frame and in every payload, are little-endian.

### CRCs

| CRC | Polynomial | Init | Reflected | Final XOR | Check value (`"123456789"`) |
|---|---|---|---|---|---|
| `HCRC`: CRC-8 (CRC-8/SMBUS) | `0x07` | `0x00` | no | none | `0xF4` |
| `CRC16`: CRC-16/CCITT-FALSE | `0x1021` | `0xFFFF` | no | none | `0x29B1` |

### MAX_PAYLOAD

- `MAX_PAYLOAD` is the protocol constant **1024** (`drivelink::protocol::kMaxPayload`). It is also the hard upper bound for any receiver.
- A firmware may be built with a smaller limit (`CONFIG_UART_COMM_MAX_PAYLOAD`, 128 to 1024), never a larger one. From F3b on, `GET_INFO` reports it.
- Bulk transfers (descriptors, capture, OTA) use chunks of 512 bytes or less, so they fit with their headers.

## MSG_TYPE ranges

| Range | Class | Direction |
|---|---|---|
| `0x00` | reserved | never sent |
| `0x01–0x3F` | commands | either side may send one; the other answers |
| `0x40–0x7F` | responses | answer to a command |
| `0x80–0xBF` | streams | periodic data, latest value wins; nothing is answered |
| `0xC0–0xFE` | events | unsolicited notices |
| `0xFF` | reserved | never sent |

The current assignments are in [registry.md](registry.md), generated from `protocol/defs/registry.yaml`.

## Receiving (scanner rules)

A receiver keeps a window of received bytes and scans it for frames. At each preamble it runs these checks in order. A candidate that fails a check is noise: **only its first byte is dropped**, and the scan resumes from the next byte. A real frame that starts inside a rejected candidate is still found, and valid frames already received are never flushed.

1. Wait for the 4 header bytes, then check `HCRC`. A mismatch is a header CRC error.
2. Check `LEN` against the receiver's limit. Above it is a length error.
3. Wait for the whole frame, then check `CRC16`. A mismatch is a CRC error.

Checks 1 and 2 need no payload bytes. A false preamble (from noise, or from inside a payload while resyncing) is therefore rejected after 7 bytes, and almost never makes the receiver wait for a payload that will not come.

Other rules:
- A candidate whose `MSG_TYPE` is `0xAA` follows a run of four or more start bytes. Rejecting it is part of normal resync and is not counted as an error. A frame with `MSG_TYPE` `0xAA` is still delivered when it is valid.
- A partial frame that gets no new bytes for 100 ms is stale: the receiver drops its first byte and rescans (a timeout).
- The window holds two maximum-size frames, so a full window always holds one whole candidate and the scan always makes progress.

The firmware counts each kind of reject (`uart_comm::ErrorCounters`) and reports the counts as diag counters: `uart_comm.HeaderCrcError`, `LengthError`, `CrcError`, `PartialFrame` and `DiscardedBytes`.

## Golden frames

These exact bytes are checked by the C++ tests (`protocol/test/test_framing.cpp`) and by `protocol.py` (on import, and in its self-test, `python firmware/test/uart_comm/protocol.py`, which CI runs).

MSG_TYPE `0x41`, payload `11 22 33`:

```
AA AA AA  41  03 00  D2  11 22 33  2F 0D
preamble  type LEN  HCRC payload   CRC16
```

- `HCRC` = CRC-8 of `41 03 00` = `0xD2`
- `CRC16` = CRC-16 of `41 03 00 D2 11 22 33` = `0x0D2F`, sent as `2F 0D`

MSG_TYPE `0x80`, empty payload: `AA AA AA 80 00 00 0B 93 E8`.

MSG_TYPE `0x63`, a 300-byte payload whose byte `i` is `i & 0xFF`: the header is `63 2C 01 2D` (`LEN` = `0x012C`) and the CRC16 is sent as `22 71`.

## Versions

The frame has no version field. Any change to it changes every protocol, and every protocol's version byte is bumped with it. There is no backward compatibility: the firmware and the host are updated together.

| Date | Change | Protocol versions after it |
|---|---|---|
| 2026-11-12 (F2) | The legacy streams moved to `0xBD` (motor setpoints), `0xBE` (motor telemetry) and `0xBF` (IMU), out of the way of the group channels. No payload changed. | motor_manager v6, IMU v6, time_sync v2, diag v2 |
| 2026-11-10 (F1b) | `LEN` widened to u16, `HCRC` added, `MAX_PAYLOAD` 255 → 1024, default baud 921600 | motor_manager v5, IMU v5, time_sync v2, diag v2 |
| before | u8 `LEN`, no `HCRC`, the CRC16 over `MSG_TYPE`, `LEN` and the payload, `MAX_PAYLOAD` 255 | motor_manager v4, IMU v4, time_sync v1, diag v1 |
