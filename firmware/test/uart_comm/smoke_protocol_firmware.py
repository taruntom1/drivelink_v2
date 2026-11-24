"""
smoke_protocol_firmware.py - check that the main firmware's diag and time
protocols answer over the link (docs/protocol/diag.md, time.md).

This is not a pytest suite: it runs against the main firmware (firmware/), not
the uart_comm test app. Flash it, then run from firmware/test/uart_comm/:

    python smoke_protocol_firmware.py --port COM5            (921600 baud by default)

It checks, each with the protocol version the firmware must speak:
  - diag      GetInfo (0x23) -> response on 0x63, diag v3
  - diag      GetEntries(0) -> response on 0x63 (the diag task's largest stack use)
  - time ping host request 0x02 -> stamped response 0x42, independent v1

Exit code 0 when every check passes. Watch the USB-Serial-JTAG console
(idf.py -C firmware monitor) for the "Stack high-water" lines, logged every
diag.stack_log_s (30 s by default).
"""

from __future__ import annotations

import argparse
import sys
import struct
import time

import serial

from protocol import UartCommProto

DIAG_QUERY, DIAG_RESPONSE, DIAG_VERSION = 0x23, 0x63, 0x03
TIME_REQUEST, TIME_RESPONSE, TIME_VERSION = 0x02, 0x42, 1



def wait_for(proto: UartCommProto, msg_type: int, seq: int | None, timeout: float) -> bytes:
    """Read frames until one of `msg_type` (with `seq` at payload[1], if given) arrives.
    Other traffic (diag events, status and other streams) is skipped."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"no frame of type 0x{msg_type:02X} within {timeout:.1f}s")
        t, payload = proto.read_frame(timeout=remaining)
        if t == msg_type and (seq is None or (len(payload) > 1 and payload[1] == seq)):
            return payload


def check(name: str, ok: bool, detail: str) -> bool:
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="serial port on the ESP's UART0 (GPIO43/44)")
    parser.add_argument("--baud", type=int, default=921600, help="baud rate (default 921600)")
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.5)
    time.sleep(0.1)
    proto = UartCommProto(ser)
    proto.reset_input()
    results: list[bool] = []

    # diag GetInfo: version, seq, subcommand GetInfo.
    # Opening the port can reset the board (the first open after a flash does,
    # through the adapter's DTR/RTS lines), and a command sent while it boots is
    # lost. So retry this first query, every 0.7 s for up to 5 s.
    try:
        for attempt in range(7):
            proto.send_command(DIAG_QUERY, bytes([DIAG_VERSION, 0x31, 0x01]))
            try:
                p = wait_for(proto, DIAG_RESPONSE, 0x31, 0.7)
                break
            except TimeoutError:
                if attempt == 6:
                    raise
        else:  # pragma: no cover - the loop always breaks or raises
            raise TimeoutError("no diag response")
        ok = len(p) >= 4 + 11 and p[0] == DIAG_VERSION and p[2] == 0x01 and p[3] == 0x00
        results.append(check("diag GetInfo", ok,
                             f"version {p[0]}, result {p[3] if len(p) > 3 else '?'}, "
                             f"{len(p)} bytes"))
    except TimeoutError as exc:
        results.append(check("diag GetInfo", False, str(exc)))

    # diag GetEntries from slot 0: version, seq, subcommand GetEntries, start.
    proto.send_command(DIAG_QUERY, bytes([DIAG_VERSION, 0x34, 0x02, 0x00]))
    try:
        p = wait_for(proto, DIAG_RESPONSE, 0x34, 3.0)
        ok = len(p) >= 6 and p[0] == DIAG_VERSION and p[2] == 0x02 and p[3] == 0x00
        results.append(check("diag GetEntries", ok,
                             f"version {p[0]}, result {p[3] if len(p) > 3 else '?'}, "
                             f"total {p[4] if len(p) > 4 else '?'}, {len(p)} bytes"))
    except TimeoutError as exc:
        results.append(check("diag GetEntries", False, str(exc)))

    # Neither invalid request may generate a time reply. The next reply must
    # belong to the valid request; then exercise ordinary diag/ping pool reuse.
    proto.send_command(TIME_REQUEST, b'\x01\x00')
    proto.send_command(TIME_REQUEST, struct.pack('<BH', 2, 0x7131))
    proto.send_command(TIME_REQUEST, struct.pack('<BH', TIME_VERSION, 0x7132))
    try:
        p = wait_for(proto, TIME_RESPONSE, None, 3.0)
        ok = len(p) == 19
        if ok:
            version, ident, rx, tx = struct.unpack('<BHQQ', p)
            ok = version == TIME_VERSION and ident == 0x7132 and tx >= rx > 0
        results.append(check('time ping', ok, f'{len(p)} bytes, RX/TX order and CRC'))
    except TimeoutError as exc:
        results.append(check('time ping', False, str(exc)))

    ser.close()
    passed = all(results)
    print("smoke test:", "PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
