# To-do (firmware): device name and id in `GET_INFO`

Status: **implemented in H1c** (2026-11-15), core v9 on `host_overhaul`. See [approved plan and results](h1c-implementation.md). Added 2026-11-12 during the GUI design (G1). It changes the wire protocol, so it follows the "host and firmware change together" rule. The field layout must be rechecked against `GET_INFO` in `docs/protocol/core.md`, which has grown since this was written.

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

## H1c persistence decision

The name is part of the CRC-protected A/B layout commit record (format 3),
so it commits with all params and follows the same complete-generation
recovery. LP format 2 loads intact with an empty name. New SAVEs use format 3.
