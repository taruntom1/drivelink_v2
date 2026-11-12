# F3c implementation plan

Approved by the user on 2026-11-12. One PR into firmware_overhaul; multiple
commits allowed. This Codex session implements; an available independent Codex
reviewer replaces the Opus-specific workflow at high review effort. CLI output
is readable plus --json. Hardware validation includes the bare ESP32 over serial.

## Scope

- First libdrivelink slice: standalone Asio TCP/serial, session correlation,
  bounded queues, read-only retries, current core families, status and diag.
- drivelink-vdev runs real core/mock code with TCP and Linux PTY, durable KV,
  reboot and crash recovery. Default native unit-test storage stays in-memory.
- Generic descriptor-driven CLI, typed values/actions, JSON envelopes/NDJSON.
- Host superbuild, Qt-MinGW and Linux presets, package exports and host-linux CI.
- Groups/arming, time model, full cached device model, GUI/agent/ROS remain with
  their scheduled steps. No wire or safety rule changes.

## Changes and tests

host/{libdrivelink,vdev,cli,superbuild,test,cmake}; protocol CMake exports and
package.xml; native core_os optional file/boot/diagnostic adapters; native
build source list; CI; related rules, READMEs and protocol/status documentation.

Unit tests cover scalar/ref/blob parsing, durable reopen/failed writes,
sequence wrap, stale replies after timeout, no mutation retry, cancellation,
bounded events, malformed pages/streams and explicit version mismatch.
CLI integration uses separate vdev processes for save/reboot/reload, failed
SAVE, crash-loop safe mode and exit, directory locking, TCP and Linux PTY.
An external installed-package consumer verifies find_package exports.

Required regression checks: three zero-warning IDF builds (normal, nolog,
UART HIL app), native tests, generator check and unit tests, Python frame
reference. Host checks run on Qt MinGW and Ubuntu 26.04.

## Review and hardware

Self-review plus an independent high-effort Codex review is required for the
new host session concurrency and persistent simulator backend. Verify/fix each
finding, then rerun affected checks. After review, flash normal firmware and
run host/test/hardware_smoke.py on bare ESP32 UART0 COM3 with console COM4.
Record discovery, descriptors, layout/params, save/reboot/reload, mock actions,
status/diag and restored original config. No attached equipment is driven.

## Internal choices to log

- Public service results use nlohmann JSON; pinned Asio/JSON archives have
  SHA256 checks. The static host library keeps Asio private.
- Requests serialize; successful seqs wrap, timeout seqs retire until reconnect.
  Only idempotent reads retry. Event overflow drops oldest with a counter.
- Native files commit each KV blob separately, preserving the real core's A/B
  commit order; a separate running marker models crashes across processes.
- Vdev uses the existing real-time staging deadlines. Desktop integration tests
  may retry only definite Busy/Withdrawn rejections (no effect); they do not
  retry mutation timeouts. Firmware/core deadline rules are unchanged.

## Documentation and finish

Update root/firmware/component/host guidance, new READMEs, protocol package and
core consumer notes, architecture implementation status and overhaul status.
Record hardware outcome, review findings, next-step handoff and PR link. Push
commits and open exactly one PR targeting firmware_overhaul.
