"""
smoke_main_firmware.py - check that the main firmware's managers answer over
the link (docs/protocol/framing.md).

This is not a pytest suite: it runs against the main firmware (firmware/), not
the uart_comm test app. Flash it, then run from firmware/test/uart_comm/:

    python smoke_main_firmware.py --port COM5            (921600 baud by default)

It checks, each with the protocol version the firmware must speak:
  - diag      GetInfo (0x23) -> response on 0x63, diag v3
  - diag      GetEntries(0) -> response on 0x63 (the diag task's largest stack use)
  - time_sync the ESP's periodic sync request on 0x20, time_sync v2 (every 5 s)

Exit code 0 when every check passes. Watch the USB-Serial-JTAG console
(idf.py -C firmware monitor) for the "Stack high-water" lines, logged every
CONFIG_DRIVELINK_STACK_LOG_PERIOD_S (30 s).
"""

from __future__ import annotations

import argparse
import sys
import time

import serial

from protocol import UartCommProto

DIAG_QUERY, DIAG_RESPONSE, DIAG_VERSION = 0x23, 0x63, 0x03
TIME_SYNC_REQUEST, TIME_SYNC_VERSION = 0x20, 0x02
STARTED_TIME_SYNC = 0x02



def wait_for(proto: UartCommProto, msg_type: int, seq: int | None, timeout: float) -> bytes:
    """Read frames until one of `msg_type` (with `seq` at payload[1], if given) arrives.
    Other traffic (diag events, time sync requests, streams) is skipped."""
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
    expected_managers = 0xFF
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
        # Diag v3 reports module counts; legacy managers are still expected.
        expected_managers = STARTED_TIME_SYNC
        results.append(check("diag GetInfo", ok,
                             f"version {p[0]}, result {p[3] if len(p) > 3 else '?'}, "
                             f"expected_managers 0x{expected_managers:02X}, {len(p)} bytes"))
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

    # time_sync: the ESP sends a request every 5 s. version, reserved, request_id u16, client time i64.
    if expected_managers & STARTED_TIME_SYNC:
        try:
            p = wait_for(proto, TIME_SYNC_REQUEST, None, 7.0)
            ok = len(p) == 12 and p[0] == TIME_SYNC_VERSION
            results.append(check("time_sync request", ok, f"version {p[0]}, {len(p)} bytes"))
        except TimeoutError as exc:
            results.append(check("time_sync request", False, str(exc)))
    else:
        results.append(check("time_sync request", False, "time_sync is not expected"))

    ser.close()
    passed = all(results)
    print("smoke test:", "PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
