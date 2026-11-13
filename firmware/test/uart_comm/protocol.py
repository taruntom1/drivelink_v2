"""
protocol.py - Python reference implementation of the DriveLink frame
(docs/protocol/framing.md), used by the HIL tests and the smoke script.

Frame format (matches protocol/src/framing.cpp exactly):
    [0xAA][0xAA][0xAA]  preamble (3 bytes)
    [MSG_TYPE]          1 byte
    [LEN_LO][LEN_HI]    u16 payload length, little-endian (0-1024)
    [HCRC]              CRC-8 (poly 0x07, init 0x00) over MSG_TYPE + LEN
    [PAYLOAD...]        LEN bytes
    [CRC_LO][CRC_HI]    CRC-16/CCITT over (MSG_TYPE + LEN + HCRC + PAYLOAD), little-endian

CRC-16/CCITT: polynomial 0x1021, initial value 0xFFFF (check value 0x29B1).
CRC-8: polynomial 0x07, initial value 0x00 (check value 0xF4).
"""

from __future__ import annotations

import struct
import time

CORE_PROTOCOL_VERSION = 3


def command_group_payload(epoch: int, counter: int, values: tuple[int, ...] = ()) -> bytes:
    """Core v3 command stream; values are descriptor-typed raw u32 slots."""
    if len(values) > 64:
        raise ValueError("at most 64 group entries")
    return struct.pack("<BBH", CORE_PROTOCOL_VERSION, epoch, counter) + struct.pack("<" + "I" * len(values), *values)

try:
    import serial  # used only in annotations; the self-test runs without pyserial
except ImportError:
    serial = None


# -- CRCs (same algorithms as protocol/src/crc.cpp) --

def _build_crc16_table() -> list[int]:
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        table.append(crc & 0xFFFF)
    return table


def _build_crc8_table() -> list[int]:
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) if (crc & 0x80) else (crc << 1)
        table.append(crc & 0xFF)
    return table


_CRC16_TABLE: list[int] = _build_crc16_table()
_CRC8_TABLE: list[int] = _build_crc8_table()


def crc16_ccitt(data: bytes) -> int:
    """Compute CRC-16/CCITT (poly 0x1021, init 0xFFFF) over *data*."""
    crc = 0xFFFF
    for byte in data:
        idx = ((crc >> 8) ^ byte) & 0xFF
        crc = (_CRC16_TABLE[idx] ^ (crc << 8)) & 0xFFFF
    return crc


def crc8(data: bytes) -> int:
    """Compute CRC-8 (poly 0x07, init 0x00) over *data*."""
    crc = 0
    for byte in data:
        crc = _CRC8_TABLE[crc ^ byte]
    return crc


assert crc16_ccitt(b"123456789") == 0x29B1
assert crc8(b"123456789") == 0xF4


# -- Frame encode / decode --

PREAMBLE = b"\xAA\xAA\xAA"
MAX_PAYLOAD = 1024         # protocol limit (drivelink::protocol::kMaxPayload)
HEADER_LEN = 4             # MSG_TYPE + LEN (2) + HCRC
OVERHEAD = 3 + HEADER_LEN + 2


def encode_header(msg_type: int, length: int, good_hcrc: bool = True) -> bytes:
    """Encode MSG_TYPE + LEN + HCRC (no preamble). LEN is not range-checked,
    so tests can build headers that announce too much."""
    head = bytes([msg_type]) + struct.pack("<H", length)
    hcrc = crc8(head)
    if not good_hcrc:
        hcrc ^= 0x5A
    return head + bytes([hcrc])


def encode_frame(msg_type: int, payload: bytes = b"") -> bytes:
    """Encode a complete frame ready to be written to the serial port."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"Payload too long: {len(payload)} > {MAX_PAYLOAD}")
    body = encode_header(msg_type, len(payload)) + payload
    return PREAMBLE + body + struct.pack("<H", crc16_ccitt(body))


def encode_frame_bad_crc(msg_type: int, payload: bytes = b"") -> bytes:
    """Encode a frame with a valid header but a corrupted CRC16 (last byte flipped)."""
    frame = bytearray(encode_frame(msg_type, payload))
    frame[-1] ^= 0xFF   # flip all bits in the high CRC byte
    return bytes(frame)


def encode_false_header(msg_type: int, length: int, good_hcrc: bool) -> bytes:
    """A preamble and header with no payload behind it: a false frame start."""
    return PREAMBLE + encode_header(msg_type, length, good_hcrc)


# The golden frames of protocol/test/test_framing.cpp (and docs/protocol/framing.md).
assert encode_frame(0x41, b"\x11\x22\x33") == bytes.fromhex("aaaaaa410300d21122332f0d")
assert encode_frame(0x80) == bytes.fromhex("aaaaaa8000000b93e8")
_GOLDEN_300 = encode_frame(0x63, bytes(i & 0xFF for i in range(300)))
assert _GOLDEN_300[:7] == bytes.fromhex("aaaaaa632c012d") and _GOLDEN_300[-2:] == bytes.fromhex("2271")


# -- UartCommProto: thin serial wrapper --

class UartCommProto:
    """
    Wraps a pyserial Serial instance with uart_comm frame send/receive helpers.

    All timeouts are in seconds (float).
    """

    def __init__(self, ser: serial.Serial) -> None:
        self.ser = ser
        # Bytes read but not yet consumed. Kept across read_frame() calls, so
        # a second frame that arrived in the same read is not lost.
        self._rx = bytearray()

    def reset_input(self) -> None:
        """Drop everything buffered, on the host and in the OS driver."""
        self._rx.clear()
        self.ser.reset_input_buffer()

    @staticmethod
    def _scan(buf: bytearray) -> tuple[int, bytes] | None:
        """
        Find and consume the first valid frame in *buf*, with the firmware
        scanner's rules: at each preamble check HCRC, then LEN, then the
        CRC16; a candidate that fails drops one byte. Returns None when more
        bytes are needed (bytes before a possible frame start are dropped).
        """
        i = 0
        while True:
            i = buf.find(PREAMBLE, i)
            if i < 0:
                # Keep a trailing partial preamble.
                keep = 0
                while keep < 2 and keep < len(buf) and buf[len(buf) - 1 - keep] == 0xAA:
                    keep += 1
                del buf[: len(buf) - keep]
                return None
            del buf[:i]
            i = 0
            if len(buf) < 3 + HEADER_LEN:
                return None
            header = bytes(buf[3:3 + HEADER_LEN])
            if crc8(header[:3]) != header[3]:
                i = 1
                continue
            payload_len = header[1] | (header[2] << 8)
            if payload_len > MAX_PAYLOAD:
                i = 1
                continue
            total = OVERHEAD + payload_len
            if len(buf) < total:
                return None
            body = bytes(buf[3:3 + HEADER_LEN + payload_len])
            (received_crc,) = struct.unpack_from("<H", buf, 3 + HEADER_LEN + payload_len)
            if crc16_ccitt(body) != received_crc:
                i = 1
                continue
            msg_type = header[0]
            payload = bytes(body[HEADER_LEN:])
            del buf[:total]
            return msg_type, payload

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

        The method scans for the 0xAA 0xAA 0xAA preamble, checks the header
        CRC and LEN, then the CRC16, and returns (msg_type, payload).

        Raises TimeoutError if a complete, CRC-valid frame is not received
        within *timeout* seconds.
        """
        deadline = time.monotonic() + timeout
        buf = self._rx

        while True:
            frame = self._scan(buf)
            if frame is not None:
                return frame
            if time.monotonic() >= deadline:
                break
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                buf.extend(chunk)

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
        assert len(payload) == 28, f"Expected 28-byte counter payload, got {len(payload)}"
        (crc_errors, timeouts, uart_errors, overflows,
         length_errors, discarded_bytes, header_crc_errors) = struct.unpack_from("<IIIIIII", payload)
        return {
            "header_crc_errors": header_crc_errors,
            "crc_errors": crc_errors,
            "timeouts": timeouts,
            "uart_errors": uart_errors,
            "overflows": overflows,
            "length_errors": length_errors,
            "discarded_bytes": discarded_bytes,
        }


# -- Self-test (no hardware): python protocol.py. CI runs it. --

class _FakeSerial:
    """Hands out the given bytes in small reads, to exercise the buffering."""

    def __init__(self, data: bytes) -> None:
        self.data = bytearray(data)

    @property
    def in_waiting(self) -> int:
        return len(self.data)

    def read(self, n: int) -> bytes:
        n = min(n, 5)
        out = bytes(self.data[:n])
        del self.data[:n]
        return out

    def reset_input_buffer(self) -> None:
        self.data.clear()


def _frames(data: bytes, count: int) -> list[tuple[int, bytes]]:
    proto = UartCommProto(_FakeSerial(data))  # type: ignore[arg-type]
    return [proto.read_frame(timeout=0.2) for _ in range(count)]


def _self_test() -> None:
    assert command_group_payload(255, 65535, (0x3fc00000, 42)) == bytes.fromhex("03ffffff0000c03f2a000000")
    assert len(command_group_payload(0, 0)) == 4
    assert len(command_group_payload(0, 0, (0,) * 64)) == 260
    try:
        command_group_payload(0, 0, (0,) * 65)
        raise AssertionError("expected a group length rejection")
    except ValueError:
        pass
    # Back-to-back frames, some holding a false preamble, one of maximum size.
    payloads = [b"\x01", b"\xAA\xAA\xAA", b"", bytes(range(32)), b"\xAA" * 16, bytes(MAX_PAYLOAD)]
    got = _frames(b"".join(encode_frame(0x41, x) for x in payloads), len(payloads))
    assert [x[1] for x in got] == payloads
    # A bad-HCRC false header announcing 1000 bytes does not hold up the frame behind it.
    assert _frames(encode_false_header(0x41, 1000, False) + encode_frame(0x42, b"\x09"), 1) == [(0x42, b"\x09")]
    # A valid header with LEN above the limit is skipped.
    assert _frames(encode_false_header(0x41, MAX_PAYLOAD + 1, True) + encode_frame(0x43, b"\x07"), 1) == [
        (0x43, b"\x07")
    ]
    # A bad CRC16 and stray start bytes are skipped.
    assert _frames(encode_frame_bad_crc(0x41, b"\x01\x02") + b"\xAA\xAA" + encode_frame(0x44), 1) == [(0x44, b"")]
    # A run of start bytes, then a frame whose MSG_TYPE is 0xAA.
    assert _frames(b"\xAA" * 50 + encode_frame(0xAA, b"\x05"), 1) == [(0xAA, b"\x05")]
    # Nothing valid: read_frame times out.
    try:
        _frames(encode_false_header(0x41, 10, True) + bytes(5), 1)
        raise AssertionError("expected a timeout")
    except TimeoutError:
        pass
    print("protocol.py self-test: ok")


if __name__ == "__main__":
    _self_test()
