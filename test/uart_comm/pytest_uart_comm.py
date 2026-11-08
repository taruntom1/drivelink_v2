"""
pytest_uart_comm.py - Hardware-in-the-loop tests for the uart_comm component.

Prerequisites
-------------
1.  Flash ESP32-S3 with the firmware in test/uart_comm/main/test_uart_comm_main.cpp
    idf.py -C test/uart_comm build flash
2.  Wire a USB-UART adapter:
    ESP32-S3 GPIO43 (TX) -> USB-UART RXD
    ESP32-S3 GPIO44 (RX) -> USB-UART TXD
        GND ---------------- GND
3.  Install Python dependencies:
        pip install -r requirements.txt
4.  Run:
        pytest pytest_uart_comm.py --port COM5   (or /dev/ttyUSB0)

Test map
--------
TC-01  test_ping_echo_single_byte            Basic round-trip sanity check
TC-02  test_echo_multi_byte_payload          Echo with a known 8-byte pattern
TC-03  test_echo_max_payload                 Echo at maximum payload size (128 bytes)
TC-04  test_echo_empty_payload               Echo with zero-byte payload
TC-05  test_crc_error_increments_counter     One corrupt frame -> crc_errors += 1
TC-06  test_multiple_crc_errors_counted      Five corrupt frames -> crc_errors += 5
TC-07  test_counter_delta_isolation          Only the corrupt frames in this test
                                             are counted (not bleed from others)
TC-08  test_stream_received_by_callback      PC sends MSG_TYPE=0x80 -> ESP callback
                                             stores it -> CMD_READ_STREAM returns it
TC-09  test_stream_slot_cleared_after_read   Second CMD_READ_STREAM returns no-data
TC-10  test_sendcmd_with_stream_type         ESP's sendCommand(0x80, ...) returns
                                             ESP_ERR_INVALID_ARG
TC-11  test_esp_sends_stream_to_pc           CMD_TRIGGER_STREAM -> PC reads the
                                             stream frame (MSG_TYPE=0x80)
TC-12  test_stream_frame_crc_is_valid        CRC in the stream frame matches
TC-13  test_response_msg_type_in_rsp_range   All responses have 0x40 <= type <= 0x7F
TC-14  test_baud_ack_alive_ping             CMD_BAUD_ACK round-trip (link health)
TC-15  test_stray_bytes_ignored_before_frame Garbage bytes before a valid frame
                                             must not confuse the parser
TC-16  test_back_to_back_burst               8 frames in one write (payloads hold
                                             AA AA AA) -> 8 echoes, in order
TC-17  test_oversized_len_does_not_flush     False preamble with LEN > max before a
                                             valid frame -> frame still delivered,
                                             length_errors += 1
TC-18  test_truncated_frame_then_valid       Frame cut short, then a valid frame ->
                                             valid frame delivered, timeouts += 1
TC-19  test_invalid_send_args_rejected       Reserved MSG_TYPEs and null payloads
                                             are rejected by the TX API
"""

import struct
import time

import pytest

from protocol import UartCommProto, encode_frame, crc16_ccitt

# -- MSG_TYPE assignments (must match test_uart_comm_main.cpp) --
CMD_ECHO = 0x01
CMD_GET_COUNTERS = 0x02
CMD_READ_STREAM = 0x03
CMD_SENDCMD_STREAM = 0x04
CMD_TRIGGER_STREAM = 0x05
CMD_BAUD_ACK = 0x06
CMD_SEND_INVALID = 0x07

RSP_ECHO = 0x41
RSP_COUNTERS = 0x42
RSP_STREAM_DATA = 0x44
RSP_SENDCMD_RESULT = 0x45
RSP_TRIGGER_ACK = 0x46
RSP_BAUD_ACK = 0x47
RSP_SEND_INVALID = 0x48

STREAM_TEST = 0x80


# -- Helpers --

def get_counters(proto: UartCommProto) -> dict[str, int]:
    """Convenience wrapper around proto.get_counters()."""
    return proto.get_counters()


def read_stream_slot(proto: UartCommProto) -> tuple[bool, bytes]:
    """
    Send CMD_READ_STREAM and decode the response.

    Returns (has_data, payload). has_data is True when the ESP callback
    had stored data since the last read.
    """
    proto.send_command(CMD_READ_STREAM)
    msg_type, payload = proto.read_frame()
    assert msg_type == RSP_STREAM_DATA, (
        f"Expected RSP_STREAM_DATA (0x44), got 0x{msg_type:02X}"
    )
    assert len(payload) >= 1, "RSP_STREAM_DATA must contain at least the has-data flag"
    has_data = payload[0] == 0x01
    data = bytes(payload[1:]) if has_data else b""
    return has_data, data


def test_ping_echo_single_byte(proto: UartCommProto) -> None:
    """A single-byte echo confirms bidirectional framing is working."""
    proto.send_command(CMD_ECHO, b"\xAB")
    msg_type, payload = proto.read_frame()

    assert msg_type == RSP_ECHO, f"Expected 0x{RSP_ECHO:02X}, got 0x{msg_type:02X}"
    assert payload == b"\xAB", f"Payload mismatch: {payload.hex()}"


def test_echo_multi_byte_payload(proto: UartCommProto) -> None:
    """Echo a predictable 8-byte payload; verify every byte is preserved."""
    original = bytes([0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF])
    proto.send_command(CMD_ECHO, original)
    msg_type, payload = proto.read_frame()

    assert msg_type == RSP_ECHO
    assert payload == original, f"Payload mismatch: expected {original.hex()}, got {payload.hex()}"


def test_echo_max_payload(proto: UartCommProto) -> None:
    """
    Echo exactly CONFIG_UART_COMM_MAX_PAYLOAD (128) bytes.
    Verifies that large payloads are framed and transmitted intact.
    """
    max_payload = 128
    original = bytes(range(max_payload))
    proto.send_command(CMD_ECHO, original)
    msg_type, payload = proto.read_frame(timeout=5.0)

    assert msg_type == RSP_ECHO
    assert len(payload) == max_payload, f"Length mismatch: {len(payload)} != {max_payload}"
    assert payload == original, "Max-payload echo data corrupted"


def test_echo_empty_payload(proto: UartCommProto) -> None:
    """Echo with len=0; the protocol must still frame and deliver it."""
    proto.send_command(CMD_ECHO, b"")
    msg_type, payload = proto.read_frame()

    assert msg_type == RSP_ECHO
    assert payload == b"", f"Expected empty payload, got {payload.hex()}"


def test_crc_error_increments_counter(proto: UartCommProto) -> None:
    """
    Send one frame with a deliberately corrupted CRC.
    Then query the error counters and assert crc_errors rose by exactly 1.
    """
    before = get_counters(proto)

    proto.send_corrupt_frame(CMD_ECHO, b"\xDE\xAD")
    time.sleep(0.05)

    after = get_counters(proto)
    delta = after["crc_errors"] - before["crc_errors"]
    assert delta == 1, f"Expected crc_errors +1, got delta={delta}"


def test_multiple_crc_errors_counted(proto: UartCommProto) -> None:
    """
    Send 5 corrupt frames. The crc_errors counter should increase by 5.
    A small inter-frame gap gives the ESP time to parse each one individually.
    """
    n_corrupt = 5
    before = get_counters(proto)

    for i in range(n_corrupt):
        proto.send_corrupt_frame(CMD_ECHO, bytes([i]))
        time.sleep(0.03)

    after = get_counters(proto)
    delta = after["crc_errors"] - before["crc_errors"]
    assert delta == n_corrupt, f"Expected crc_errors +{n_corrupt}, got delta={delta}"


def test_counter_delta_isolation(proto: UartCommProto) -> None:
    """
    After sending 2 corrupt frames, only crc_errors should increase;
    timeouts, uart_errors, and overflows must remain unchanged.
    """
    before = get_counters(proto)

    for _ in range(2):
        proto.send_corrupt_frame(CMD_ECHO, b"\xFF")
        time.sleep(0.03)

    after = get_counters(proto)

    assert after["crc_errors"] - before["crc_errors"] == 2
    assert after["timeouts"] - before["timeouts"] == 0, "Unexpected timeout increment"
    assert after["uart_errors"] - before["uart_errors"] == 0, "Unexpected uart_error increment"
    assert after["overflows"] - before["overflows"] == 0, "Unexpected overflow increment"
    assert after["length_errors"] - before["length_errors"] == 0, "Unexpected length_error increment"


def test_stream_received_by_callback(proto: UartCommProto) -> None:
    """
    PC sends a stream frame (MSG_TYPE=0x80) directly.
    The ESP registered callback should capture the payload.
    CMD_READ_STREAM (0x03) then returns it.
    """
    stream_data = bytes([0xCA, 0xFE, 0xBA, 0xBE])

    read_stream_slot(proto)

    proto.ser.write(encode_frame(STREAM_TEST, stream_data))
    time.sleep(0.05)

    has_data, received = read_stream_slot(proto)

    assert has_data, "ESP did not report any stream data received"
    assert received == stream_data, (
        f"Stream payload mismatch: expected {stream_data.hex()}, got {received.hex()}"
    )


def test_stream_slot_cleared_after_read(proto: UartCommProto) -> None:
    """
    After CMD_READ_STREAM returns data, a second call must return has_data=False.
    This confirms the firmware clears g_stream_received on each read.
    """
    proto.ser.write(encode_frame(STREAM_TEST, b"\x11\x22\x33"))
    time.sleep(0.05)

    has_data_first, _ = read_stream_slot(proto)
    assert has_data_first, "First read should have data"

    has_data_second, _ = read_stream_slot(proto)
    assert not has_data_second, "Second read should return no data (slot must be cleared)"


def test_sendcmd_with_stream_type(proto: UartCommProto) -> None:
    """
    CMD_SENDCMD_STREAM (0x04) asks the ESP firmware to call
    sendCommand(0x80, ...), which must return ESP_ERR_INVALID_ARG.
    RSP_SENDCMD_RESULT (0x45) carries 0x01 when the expected error was seen.
    """
    proto.send_command(CMD_SENDCMD_STREAM, b"\xAA\xBB")
    msg_type, payload = proto.read_frame()

    assert msg_type == RSP_SENDCMD_RESULT, (
        f"Expected RSP_SENDCMD_RESULT (0x45), got 0x{msg_type:02X}"
    )
    assert len(payload) == 1, f"Expected 1-byte result, got {len(payload)}"
    assert payload[0] == 0x01, (
        "ESP did not get ESP_ERR_INVALID_ARG - sendCommand stream-type guard is broken"
    )


def test_esp_sends_stream_to_pc(proto: UartCommProto) -> None:
    """
    CMD_TRIGGER_STREAM (0x05) tells the ESP to call sendData(0x80, payload).
    PC should receive the stream frame (MSG_TYPE=0x80) with the matching payload.
    It also receives RSP_TRIGGER_ACK (0x46); order is not guaranteed.
    """
    trigger_payload = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x42])
    proto.send_command(CMD_TRIGGER_STREAM, trigger_payload)

    frames = {
        msg_type: payload
        for msg_type, payload in proto.read_frames(2, timeout_per_frame=3.0)
    }

    assert RSP_TRIGGER_ACK in frames, (
        f"RSP_TRIGGER_ACK (0x46) not received; got types: {[hex(t) for t in frames]}"
    )
    assert STREAM_TEST in frames, (
        f"Stream frame (0x80) not received; got types: {[hex(t) for t in frames]}"
    )
    assert frames[STREAM_TEST] == trigger_payload, (
        f"Stream payload mismatch: expected {trigger_payload.hex()}, "
        f"got {frames[STREAM_TEST].hex()}"
    )


def test_stream_frame_crc_is_valid(proto: UartCommProto) -> None:
    """
    The read_frame() helper already validates CRC before returning.
    This test makes the validation explicit by re-encoding the received
    fields and cross-checking the CRC, proving the on-wire encoding matches.
    """
    trigger_payload = bytes([0x01, 0x02, 0x03, 0x04])
    proto.send_command(CMD_TRIGGER_STREAM, trigger_payload)

    received: dict[int, bytes] = {}

    for _ in range(2):
        msg_type, payload = proto.read_frame()
        received[msg_type] = payload

    assert STREAM_TEST in received, "Stream frame not received"
    sp = received[STREAM_TEST]

    crc_data = bytes([STREAM_TEST, len(sp)]) + sp
    expected_crc = crc16_ccitt(crc_data)

    encoded = encode_frame(STREAM_TEST, sp)
    on_wire_crc = struct.unpack_from("<H", encoded, len(encoded) - 2)[0]

    assert on_wire_crc == expected_crc, (
        f"CRC mismatch: on-wire 0x{on_wire_crc:04X}, expected 0x{expected_crc:04X}"
    )


def test_response_msg_type_in_rsp_range(proto: UartCommProto) -> None:
    """
    All response MSG_TYPEs sent by the firmware must fall in 0x40-0x7F
    (the response range defined by the protocol).
    Stream frames (0x80) are exempt and checked separately.
    """
    test_vectors = [
        (CMD_ECHO, b"\x01", RSP_ECHO),
        (CMD_GET_COUNTERS, b"", RSP_COUNTERS),
        (CMD_READ_STREAM, b"", RSP_STREAM_DATA),
        (CMD_SENDCMD_STREAM, b"\x00", RSP_SENDCMD_RESULT),
        (CMD_BAUD_ACK, b"", RSP_BAUD_ACK),
    ]

    for cmd, payload, expected_type in test_vectors:
        proto.send_command(cmd, payload)
        msg_type, _ = proto.read_frame()
        assert msg_type == expected_type, (
            f"CMD 0x{cmd:02X}: expected RSP 0x{expected_type:02X}, got 0x{msg_type:02X}"
        )
        assert 0x40 <= msg_type <= 0x7F, (
            f"Response 0x{msg_type:02X} is outside the response range 0x40-0x7F"
        )


def test_baud_ack_alive_ping(proto: UartCommProto) -> None:
    """RSP_BAUD_ACK (0x47) must be returned with an empty payload."""
    proto.send_command(CMD_BAUD_ACK, b"")
    msg_type, payload = proto.read_frame()

    assert msg_type == RSP_BAUD_ACK, (
        f"Expected RSP_BAUD_ACK (0x47), got 0x{msg_type:02X}"
    )
    assert payload == b"", f"Expected empty payload, got {payload.hex()}"


def test_stray_bytes_ignored_before_frame(proto: UartCommProto) -> None:
    """
    Inject random garbage bytes directly onto the wire before a valid frame.
    The ESP parser must discard the junk and still deliver the subsequent
    valid CMD_ECHO correctly (no CRC error increment, no timeout).
    """
    before = get_counters(proto)

    garbage = bytes([0x00, 0x11, 0xFF, 0x55, 0x33])
    valid_frame = encode_frame(CMD_ECHO, b"\xBE\xEF")
    proto.ser.write(garbage + valid_frame)

    msg_type, payload = proto.read_frame(timeout=3.0)

    after = get_counters(proto)

    assert msg_type == RSP_ECHO, f"Expected RSP_ECHO, got 0x{msg_type:02X}"
    assert payload == b"\xBE\xEF"

    # The stray bytes should not cause a CRC error (they do not start with 0xAAx3).
    assert after["crc_errors"] == before["crc_errors"], (
        "crc_errors incremented due to stray bytes - parser discarded them incorrectly"
    )


def test_back_to_back_burst(proto: UartCommProto) -> None:
    """
    Send 8 echo frames in a single write, with no gaps between them.
    Some payloads contain AA AA AA (a false preamble) and AA bytes next to
    the frame boundary. Every frame must be echoed, in order, with no errors.

    8 frames fit in the firmware's RX delivery and TX command queues
    (CONFIG_UART_COMM_RX_QUEUE_DEPTH / CMD_QUEUE_DEPTH default to 8).
    """
    payloads = [
        b"\x01",
        b"\xAA\xAA\xAA",
        b"\x2B\xAA\xAA\xAA",                  # float 5.3e-13 as big-endian bytes
        b"\xAA\xAA\xAA\x01\x05\x00\x00",      # looks like a nested frame header
        b"",
        bytes(range(32)),
        b"\x00\xAA",
        b"\xAA" * 16,
    ]
    before = get_counters(proto)

    proto.ser.write(b"".join(encode_frame(CMD_ECHO, p) for p in payloads))

    for i, expected in enumerate(payloads):
        msg_type, payload = proto.read_frame(timeout=3.0)
        assert msg_type == RSP_ECHO, f"Frame {i}: expected RSP_ECHO, got 0x{msg_type:02X}"
        assert payload == expected, (
            f"Frame {i}: payload mismatch: expected {expected.hex()}, got {payload.hex()}"
        )

    after = get_counters(proto)
    for key in ("crc_errors", "length_errors", "timeouts", "discarded_bytes"):
        assert after[key] == before[key], f"{key} changed during a clean burst"


def test_oversized_len_does_not_flush(proto: UartCommProto) -> None:
    """
    A false preamble whose LEN exceeds the max payload, followed straight
    away by a valid frame. The parser must skip the bad header and still
    deliver the valid frame (the old parser flushed the whole RX buffer here).
    """
    before = get_counters(proto)

    bad_header = b"\xAA\xAA\xAA" + bytes([CMD_ECHO, 0xFF])
    proto.ser.write(bad_header + encode_frame(CMD_ECHO, b"\x5A\x5B"))

    msg_type, payload = proto.read_frame(timeout=3.0)
    after = get_counters(proto)

    assert msg_type == RSP_ECHO, f"Expected RSP_ECHO, got 0x{msg_type:02X}"
    assert payload == b"\x5A\x5B"
    assert after["length_errors"] - before["length_errors"] == 1
    assert after["crc_errors"] == before["crc_errors"], "Length error was counted as a CRC error"


def test_truncated_frame_then_valid(proto: UartCommProto) -> None:
    """
    A frame that announces 64 payload bytes but stops after 10, then a valid
    frame. Once the partial frame goes stale (about 100 ms) the parser must
    drop it and deliver the valid frame that was hiding inside its length.
    """
    before = get_counters(proto)

    truncated = b"\xAA\xAA\xAA" + bytes([CMD_ECHO, 64]) + bytes(range(1, 11))
    proto.ser.write(truncated + encode_frame(CMD_ECHO, b"\x77"))

    msg_type, payload = proto.read_frame(timeout=3.0)
    after = get_counters(proto)

    assert msg_type == RSP_ECHO, f"Expected RSP_ECHO, got 0x{msg_type:02X}"
    assert payload == b"\x77"
    assert after["timeouts"] - before["timeouts"] == 1


def test_invalid_send_args_rejected(proto: UartCommProto) -> None:
    """
    CMD_SEND_INVALID makes the firmware try sendCommand(0x00), sendCommand(0xFF),
    sendCommand(rsp, nullptr, 3) and sendData(0x80, nullptr, 3).
    All four must return ESP_ERR_INVALID_ARG (bitmask 0x0F).
    """
    proto.send_command(CMD_SEND_INVALID)
    msg_type, payload = proto.read_frame()

    assert msg_type == RSP_SEND_INVALID, f"Expected 0x{RSP_SEND_INVALID:02X}, got 0x{msg_type:02X}"
    assert len(payload) == 1
    assert payload[0] == 0x0F, f"Rejected-call bitmask 0x{payload[0]:02X}, expected 0x0F"
