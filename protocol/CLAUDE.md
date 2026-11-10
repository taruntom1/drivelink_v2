# CLAUDE.md (protocol)

Rules for working in `protocol/`. The repo map and cross-cutting rules are in the root [CLAUDE.md](../CLAUDE.md). The firmware's conventions are in [firmware/CLAUDE.md](../firmware/CLAUDE.md).

## What this is

The wire library shared by the firmware and the host: framing, CRCs, the frame scanner and the field codec. See [README.md](README.md).

## Rules

- **Pure C++20.** No ESP-IDF, FreeRTOS, `esp_log`, `sdkconfig.h` or OS headers, and no `CONFIG_*` symbols. Anything that needs them stays in the firmware (`uart_comm` supplies the driver, the clock and the logging).
- **Namespace `drivelink::protocol`**, headers under `include/drivelink/protocol/`. Types and enumerators PascalCase, functions camelCase, constants `k`-prefixed `constexpr`, members `m_`.
- **No allocation after construction, no blocking, no exceptions or RTTI.** The firmware calls this code from real-time-adjacent tasks. A constructor may allocate once (the `Scanner` window).
- **No logging, no clock.** Return results and counters, and let the caller log.
- **Untrusted input.** Every function that reads bytes from the wire checks lengths first and rejects instead of reading past the end. Frame payloads are views into the scanner's window, valid until its next call.
- **Wire changes.** Any change to the frame layout or to a codec's encoding is a protocol change: update `docs/protocol/`, the firmware, the host code and the Python HIL reference (`firmware/test/uart_comm/protocol.py`) in the same PR, and bump the protocol versions.
- **Tests.** Every header has tests in `protocol/test/`: round trips, exact lengths, rejection of malformed input, and the resync cases for the scanner. A new `.cpp` joins `PROTOCOL_DIR` test sources in `firmware/test/host/CMakeLists.txt` until `protocol/` has its own test target.
- **Warnings are errors** in the host test build, and the IDF build must stay warning-free.
