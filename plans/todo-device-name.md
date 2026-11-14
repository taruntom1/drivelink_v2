# To-do (firmware): device name and id in `GET_INFO`

Status: **scheduled for H1c** (re-planned 2026-11-14; brief in [host-steps.md](host-steps.md)), as core v9 on `host_overhaul`. Added 2026-11-12 during the GUI design (G1). It changes the wire protocol, so it follows the "host and firmware change together" rule. The field layout must be rechecked against `GET_INFO` in `docs/protocol/core.md`, which has grown since this was written.

## Why

`GET_INFO` ([firmware-architecture.md](firmware-architecture.md), "Notes on the ops") returns the versions, build id, descriptor hash, capacities, loops and flags. Nothing in it identifies the board. So:
- the GUI can't tell two boards apart, and can only list recent devices by port and firmware version
- the agent, the ROS diagnostics node and logs can't say which board they are talking to
- a session file from Tune can't record which board it was made on

## Proposal

1. **Device id:** the factory MAC address (`esp_efuse_mac_get_default`), 6 bytes, read-only. Unique per chip and needs no setup. The host uses it as the key for recent devices and session files.
2. **Device name:** a short label set by the user, up to 16 bytes of UTF-8 like instance labels, stored in NVS. Empty by default, and the host then shows the id. Strings aren't param types, so it gets its own system op, `SET_NAME(name)`, like layout `SET_LABEL`.
3. **`GET_INFO`** gains `device_id[6]` and `name_len u8 | name`.

## What it touches (one change)

- `protocol/defs/`: the `GET_INFO` body and the new system op, then the generated codecs and their round-trip tests
- firmware core: the system family handlers and NVS storage for the name
- `docs/protocol/`: the system family spec
- `libdrivelink`: the device model, and a `name` command in `drivelink-cli`
- GUI (later, H2): the status bar, Connect's recent list keyed by device id, and Rename on the Device screen
- tests: native core tests, and the HIL test app if it checks `GET_INFO`

## Decided (2026-11-14, host planning)

- `SET_NAME` applies live and sets `nvs_dirty`; SAVE persists it and REVERT restores the saved name, like a `persist` param. This matches the GUI's Rename dialog.
- It is allowed while armed: it never touches the loops.
- Through the agent it is an exclusive operation (needs the lease, or a disarmed device with no holder).

## Left to the H1c plan

- Store the name under its own NVS key, or in the saved config so it gets the A/B generation? An internal choice, made against `persistence.cpp` and logged.
