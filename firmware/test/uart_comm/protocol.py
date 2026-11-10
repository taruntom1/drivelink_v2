"""
protocol.py - Python implementation of the uart_comm binary protocol.

Frame format (matches frame.hpp / frame.cpp exactly):
    [0xAA][0xAA][0xAA]  preamble (3 bytes)
    [MSG_TYPE]          1 byte
    [LEN]               1 byte  (payload length, 0-255)
    [PAYLOAD...]        LEN bytes
    [CRC_LO][CRC_HI]    CRC-16/CCITT over (MSG_TYPE + LEN + PAYLOAD), little-endian

CRC-16/CCITT: polynomial 0x1021, initial value 0xFFFF.
"""

from __future__ import annotations

import struct
import time
import serial


# -- CRC-16/CCITT (same algorithm as crc16Ccitt() in protocol/src/crc.cpp) --

def _build_crc_table() -> list[int]:
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        table.append(crc & 0xFFFF)
    return table


_CRC_TABLE: list[int] = _build_crc_table()


def crc16_ccitt(data: bytes) -> int:
    """Compute CRC-16/CCITT (poly 0x1021, init 0xFFFF) over *data*."""
    crc = 0xFFFF
    for byte in data:
        idx = ((crc >> 8) ^ byte) & 0xFF
        crc = (_CRC_TABLE[idx] ^ (crc << 8)) & 0xFFFF
    return crc


# -- Frame encode / decode --

PREAMBLE = b"\xAA\xAA\xAA"


def encode_frame(msg_type: int, payload: bytes = b"") -> bytes:
    """Encode a complete frame ready to be written to the serial port."""
    if len(payload) > 255:
        raise ValueError(f"Payload too long: {len(payload)} > 255")
    header = bytes([msg_type, len(payload)])
    crc_data = header + payload
    crc = crc16_ccitt(crc_data)
    return PREAMBLE + crc_data + struct.pack("<H", crc)


def encode_frame_bad_crc(msg_type: int, payload: bytes = b"") -> bytes:
    """Encode a frame with an intentionally corrupted CRC (last byte flipped)."""
    frame = bytearray(encode_frame(msg_type, payload))
    frame[-1] ^= 0xFF   # flip all bits in the high CRC byte
    return bytes(frame)


# -- UartCommProto: thin serial wrapper --

class UartCommProto:
    """
    Wraps a pyserial Serial instance with uart_comm frame send/receive helpers.

    All timeouts are in seconds (float).
    """

    def __init__(self, ser: serial.Serial) -> None:
        self.ser = ser

    # -- Transmit --

    def send_command(self, msg_type: int, payload: bytes = b"") -> None:
        """Build and transmit a properly encoded frame."""
        self.ser.write(encode_frame(msg_type, payload))

    def send_corrupt_frame(self, msg_type: int, payload: bytes = b"") -> None:
        """Transmit a frame whose CRC has been deliberately corrupted."""
        self.ser.write(encode_frame_bad_crc(msg_type, payload))

    # -- Receive --

    def read_frame(self, timeout: float = 3.0) -> tuple[int, bytes]:
        """
        Read and return the next valid frame from the serial stream.

        The method scans for the 0xAA 0xAA 0xAA preamble, reads the header
        and payload, verifies the CRC, and returns (msg_type, payload).

        Raises TimeoutError if a complete, CRC-valid frame is not received
        within *timeout* seconds.
        """
        deadline = time.monotonic() + timeout
        buf = bytearray()

        while time.monotonic() < deadline:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                buf.extend(chunk)

            # Search for preamble in the accumulated buffer
            i = 0
            while i <= len(buf) - 3:
                if buf[i] == 0xAA and buf[i + 1] == 0xAA and buf[i + 2] == 0xAA:
                    # Need at least 2 more bytes for the header
                    if len(buf) < i + 5:
                        break   # wait for more data

                    payload_len = buf[i + 4]
                    total_needed = i + 3 + 2 + payload_len + 2  # preamble+hdr+payload+crc

                    if len(buf) < total_needed:
                        break   # wait for more data

                    # Slice the candidate frame fields
                    msg_type = buf[i + 3]
                    payload = bytes(buf[i + 5 : i + 5 + payload_len])
                    crc_lo = buf[i + 5 + payload_len]
                    crc_hi = buf[i + 5 + payload_len + 1]
                    received_crc = crc_lo | (crc_hi << 8)

                    # CRC covers MSG_TYPE + LEN + PAYLOAD
                    crc_data = bytes(buf[i + 3 : i + 5 + payload_len])
                    computed_crc = crc16_ccitt(crc_data)

                    if computed_crc != received_crc:
                        # Not a valid frame at this position - advance by 1 and retry
                        i += 1
                        continue

                    # Valid frame found - consume it from the buffer
                    del buf[: i + total_needed - i]
                    return msg_type, payload

                i += 1
            else:
                # No preamble byte found at all; drop everything except the last 2
                # bytes (they might be the start of a future preamble).
                if len(buf) > 2:
                    buf = buf[-2:]

        raise TimeoutError(
            f"No valid frame received within {timeout:.1f}s "
            f"(buffered {len(buf)} bytes: {bytes(buf[:32]).hex()})"
        )

    def read_frames(
        self,
        count: int,
        timeout_per_frame: float = 3.0,
    ) -> list[tuple[int, bytes]]:
        """Read exactly *count* frames, each within *timeout_per_frame* seconds."""
        return [self.read_frame(timeout_per_frame) for _ in range(count)]

    # -- Higher-level helpers --

    def ping(self, timeout: float = 3.0) -> bool:
        """Send a one-byte echo and verify the response. Returns True on success."""
        self.send_command(0x01, b"\xAB")
        try:
            msg_type, payload = self.read_frame(timeout)
            return msg_type == 0x41 and payload == b"\xAB"
        except TimeoutError:
            return False

    def get_counters(self, timeout: float = 3.0) -> dict[str, int]:
        """Request error counters from the firmware and return them as a dict."""
        self.send_command(0x02)
        msg_type, payload = self.read_frame(timeout)
        assert msg_type == 0x42, f"Expected RSP_COUNTERS (0x42), got 0x{msg_type:02X}"
        assert len(payload) == 24, f"Expected 24-byte counter payload, got {len(payload)}"
        (crc_errors, timeouts, uart_errors, overflows,
         length_errors, discarded_bytes) = struct.unpack_from("<IIIIII", payload)
        return {
            "crc_errors": crc_errors,
            "timeouts": timeouts,
            "uart_errors": uart_errors,
            "overflows": overflows,
            "length_errors": length_errors,
            "discarded_bytes": discarded_bytes,
        }
