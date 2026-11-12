"""
smoke_main_firmware.py - check that the main firmware's managers answer over
the link (docs/protocol/framing.md).

This is not a pytest suite: it runs against the main firmware (firmware/), not
the uart_comm test app. Flash it, then run from firmware/test/uart_comm/:

    python smoke_main_firmware.py --port COM5            (921600 baud by default)

It checks, each with the protocol version the firmware must speak:
  - diag      GetInfo (0x23) -> response on 0x63, diag v2
  - diag      GetEntries(0) -> response on 0x63 (the diag task's largest stack use)
  - motor     SET_TIMING (0x21) -> response on 0x61, motor_manager v6, result Ok
              (it creates no motor, so nothing can move)
  - IMU       SetStreamEnabled(1) (0x22) -> response on 0x42, IMU v6, any result
              then a stream frame on 0xBF whose first byte is the IMU version (v6),
              then SetStreamEnabled(0) -> response on 0x42
              (skipped if diag reports the IMU manager did not start, e.g. no IMU fitted)
  - time_sync the ESP's periodic sync request on 0x20, time_sync v2 (every 5 s)

Exit code 0 when every check passes. Watch the USB-Serial-JTAG console
(idf.py -C firmware monitor) for the "Stack high-water" lines, logged every
CONFIG_DRIVELINK_STACK_LOG_PERIOD_S (30 s).
"""

from __future__ import annotations

import argparse
import struct
import sys
import time

import serial

from protocol import UartCommProto

DIAG_QUERY, DIAG_RESPONSE, DIAG_VERSION = 0x23, 0x63, 0x02
MOTOR_CONFIGURE, MOTOR_RESPONSE, MOTOR_VERSION = 0x21, 0x61, 0x06
IMU_CONFIGURE, IMU_RESPONSE, IMU_VERSION = 0x22, 0x42, 0x06
IMU_STREAM = 0xBF  # moved here from 0x82 in F2; the first payload byte is the IMU version
TIME_SYNC_REQUEST, TIME_SYNC_VERSION = 0x20, 0x02

STARTED_MOTOR, STARTED_TIME_SYNC, STARTED_IMU = 0x01, 0x02, 0x04


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
    started_mask = 0xFF
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
        if len(p) >= 15:
            started_mask = p[14]  # header(4) + flags(1) + uptime(8) + reset_reason(1)
        results.append(check("diag GetInfo", ok,
                             f"version {p[0]}, result {p[3] if len(p) > 3 else '?'}, "
                             f"started_mask 0x{started_mask:02X}, {len(p)} bytes"))
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

    # motor SET_TIMING: version, seq, subcommand 0x0B, motor_id, control_us, telemetry_us.
    if started_mask & STARTED_MOTOR:
        body = bytes([MOTOR_VERSION, 0x32, 0x0B, 0x00]) + struct.pack("<II", 1000, 10000)
        proto.send_command(MOTOR_CONFIGURE, body)
        try:
            p = wait_for(proto, MOTOR_RESPONSE, 0x32, 3.0)
            ok = len(p) == 6 + 8 and p[0] == MOTOR_VERSION and p[2] == 0x0B and p[4] == 0x00
            applied = struct.unpack_from("<II", p, 6) if len(p) >= 14 else ("?", "?")
            results.append(check("motor SET_TIMING", ok,
                                 f"version {p[0]}, result {p[4] if len(p) > 4 else '?'}, "
                                 f"detail {p[5] if len(p) > 5 else '?'}, applied {applied}"))
        except TimeoutError as exc:
            results.append(check("motor SET_TIMING", False, str(exc)))
    else:
        results.append(check("motor SET_TIMING", False, "diag reports the motor manager did not start"))

    # IMU: enable the stream, wait for a frame on the stream channel, disable it again.
    # SetStreamEnabled body: version, seq, subcommand 0x01, enabled.
    if started_mask & STARTED_IMU:
        proto.send_command(IMU_CONFIGURE, bytes([IMU_VERSION, 0x33, 0x01, 0x01]))
        try:
            p = wait_for(proto, IMU_RESPONSE, 0x33, 3.0)
            ok = len(p) >= 5 and p[0] == IMU_VERSION and p[2] == 0x01
            results.append(check("IMU SetStreamEnabled(1)", ok,
                                 f"version {p[0]}, result {p[3] if len(p) > 3 else '?'}"))
        except TimeoutError as exc:
            results.append(check("IMU SetStreamEnabled(1)", False, str(exc)))
        try:
            p = wait_for(proto, IMU_STREAM, None, 5.0)
            ok = len(p) >= 1 and p[0] == IMU_VERSION
            results.append(check("IMU stream on 0xBF", ok, f"version {p[0] if p else '?'}, {len(p)} bytes"))
        except TimeoutError as exc:
            results.append(check("IMU stream on 0xBF", False, str(exc)))
        proto.send_command(IMU_CONFIGURE, bytes([IMU_VERSION, 0x34, 0x01, 0x00]))
        try:
            p = wait_for(proto, IMU_RESPONSE, 0x34, 3.0)
            ok = len(p) >= 5 and p[0] == IMU_VERSION and p[2] == 0x01
            results.append(check("IMU SetStreamEnabled(0)", ok,
                                 f"version {p[0]}, result {p[3] if len(p) > 3 else '?'}"))
        except TimeoutError as exc:
            results.append(check("IMU SetStreamEnabled(0)", False, str(exc)))
    else:
        print("[SKIP] IMU stream check: diag reports the IMU manager did not start (no IMU fitted?)")

    # time_sync: the ESP sends a request every 5 s. version, reserved, request_id u16, client time i64.
    if started_mask & STARTED_TIME_SYNC:
        try:
            p = wait_for(proto, TIME_SYNC_REQUEST, None, 7.0)
            ok = len(p) == 12 and p[0] == TIME_SYNC_VERSION
            results.append(check("time_sync request", ok, f"version {p[0]}, {len(p)} bytes"))
        except TimeoutError as exc:
            results.append(check("time_sync request", False, str(exc)))
    else:
        results.append(check("time_sync request", False, "diag reports time_sync did not start"))

    ser.close()
    passed = all(results)
    print("smoke test:", "PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
