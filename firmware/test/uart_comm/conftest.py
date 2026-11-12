"""
conftest.py - pytest fixtures for uart_comm hardware tests.

Usage
-----
Run from the firmware/test/uart_comm/ directory:

    pytest pytest_uart_comm.py --port COM5

The --port option specifies the COM port connected to ESP32-S3 UART0
(TX=GPIO43, RX=GPIO44).
Use --baud to override the default 921600 baud rate (rarely needed).

The ESP32-S3 must already be flashed with the test firmware from
firmware/test/uart_comm/main/test_uart_comm_main.cpp before running the tests.
"""

import pytest
import serial
import time
from typing import Generator

from protocol import UartCommProto


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--port",
        required=True,
        help="Serial port for the USB-UART adapter (e.g. COM5 or /dev/ttyUSB0)",
    )
    parser.addoption(
        "--baud",
        default=921600,
        type=int,
        help="Baud rate (default 921600, the uart_comm Kconfig default)",
    )


@pytest.fixture(scope="session")
def proto(request: pytest.FixtureRequest) -> Generator[UartCommProto, None, None]:
    """Open the serial link once for the whole test session."""
    port = request.config.getoption("--port")
    baud = request.config.getoption("--baud")

    ser = serial.Serial(port, baud, timeout=3.0)
    time.sleep(0.1)          # allow USB-UART adapter to settle
    p = UartCommProto(ser)
    p.reset_input()

    # Verify the firmware is alive with a lightweight ping before any test runs.
    try:
        p.send_command(0x01, b"\x55")   # CMD_ECHO with one byte
        msg_type, payload = p.read_frame(timeout=5.0)
        assert msg_type == 0x41 and payload == b"\x55", (
            f"Pre-test ping failed: got msg_type=0x{msg_type:02X} payload={payload.hex()}"
        )
    except Exception as exc:
        ser.close()
        pytest.exit(f"Cannot communicate with test firmware: {exc}", returncode=1)

    yield p

    ser.close()


@pytest.fixture(autouse=True)
def flush_between_tests(proto: UartCommProto) -> Generator[None, None, None]:
    """Discard any stale bytes in the RX buffer before each test."""
    proto.reset_input()
    yield
    # Small drain after the test so residual bytes don't bleed into the next one.
    time.sleep(0.02)
    proto.reset_input()
