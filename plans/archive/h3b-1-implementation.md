# H3b-1 — GUI remote mode (without mDNS)

## Context

H3a-1/H3a-2 (merged, PR #88/#89) built `drivelink-agent` and its client in libdrivelink: Hello with token and device, lobby `ListDevices`, the lease (`agentLeaseTake`/`agentLeaseRelease`, Lease events), `agentClients`, history and the composed clock. The GUI only speaks to a device directly (`LocalConnection`); its Connect page shows a disabled "Agent" tab. H3b gives the GUI the remote mode designed in G6. User decisions (2026-11-19): split H3b into **H3b-1** (this PR) and **H3b-2** (mDNS: mjansson/mdns, agent advertising, GUI browse); tokens kept in the GUI INI only when "Remember token" is checked (checked by default); ROS recognised by a **client kind in the Hello** (wire change → agent protocol v3).

Branch `claude/overhaul-H3b` (from `origin/host_overhaul`), PR into `host_overhaul`. Status table: H3b row becomes H3b-1 + H3b-2.

## Scope

**In H3b-1:** client kind on the wire (agent v3); the remote connection in the GUI (observer first, command groups only after a grant); Connect's Agent tab (typed address + token, recent agents, the agent's device list); lease chip, Clients and lease card, take/release/requesting/granted/revoked, takeover consent, confirm-live-edit (per-connection opt-out), ROS-driving banner, agent history in the faults drawer, unreachable / version-mismatch / no-device states, lease-gated exclusive operations (incl. calibration actions); `tools/wsl_serial.py`; PC hardware check.
**H3b-2:** mDNS library and notices, agent advertising (LAN only), library browse, the Agent tab's "Found on the network" list.

## Wire change (agent protocol v2 → v3; firmware/core untouched)

New enum `AgentClientKind` in `protocol/defs/registry.yaml`: Other 0, Gui 1, Cli 2, Ros 3 (generated code + docs via `tools/gen/gen.py`).
- Hello body: `… | client_name str8 | client_kind u8` (a kind > 3 → `InvalidArgument/None`).
- ClientList client: `client_id u16 | name str8 | kind u8 | holder u8`.
- Device record: `… | holder_id u16 | holder_name str8 | holder_kind u8`.
- Lease event (and Lease history entries): `holder_id u16 | holder_name str8 | holder_kind u8 | previous_id u16 | reason u8`.
- Version refusal body unchanged (readable by every version). Version byte 3.
- libdrivelink: `AgentHello::kind` (default Other), records/events gain the kind; CLI sends Cli, GUI Gui, H4a's `drivelink_hardware` Ros (H4a brief updated).
- Files: `registry.yaml`, `host/libdrivelink/include/drivelink/agent.hpp`, `src/agent_wire.cpp`, `src/session.cpp` (performHello), `host/agent/src/agent.cpp`/`lease.*`, CLI (`agent-devices`/`agent-clients` show the kind; JSON gains `kind`, readable output only adds a column — new fields allowed), `docs/protocol/agent.md` (+ Versions row), `host/test/test_agent_wire.cpp`, `agent_tests`, `agent_integration.py` (kind assertions).

## GUI backend (Opus)

Remote mode stays inside `LocalConnection` (renamed concept "device connection"; class name kept to limit churn) because Layout/Monitor/Tune/OTA are identical through the agent; a `Remote` struct holds the agent state. Files: `host/gui/backend/connection.hpp/.cpp`, new `connection_remote.cpp`, new pure `remote_model.hpp/.cpp` (tested), `app_settings`/`device_models` (recent agents).

- **Connect:** `connectAgent(address, token, device, remember)` → `SessionOptions::agent` (`client_name` = "drivelink-gui@<host>", kind Gui), `retire_quarantine` as the CLI uses. `listAgentDevices` on a worker for the device list (`agentDevices` property: alias, id, name, online, clients, holder name/kind), refreshed every 2 s while the list is shown. Refusals map to G6 states: unreachable (Connection error), version mismatch (AgentVersion body: agent and core versions), bad token, unknown device, device offline ("No device").
- **Lease state machine (pure, `remote_model`):** fed by Lease events (incl. history replay), own client id from `agentHello()`, Take/Release replies. States for the chip: Available, Mine, Ros, Other, Requesting, Revoked. A Take that timed out (`outcome_unknown`) stays Requesting until a Lease event decides it.
- **Observer rules:** remote `setupGroups()` (command groups) runs only while the lease is Mine; `armEnabled` false with "ARM needs the lease" otherwise. DISARM always enabled with a link. On revocation (event naming us as `previous_id`) or Release: drop command groups locally without CMD_CLEAR (the agent cleared them), end Tune run / identify / Auto re-run without device calls (the agent disarmed; the signal stops on DISARM — TuneRelease waits on STATUS as today), manual commands back to 0, confirmation cleared, toast "Lease taken by <name>". ARM answered `Busy/None` right after a grant → "Previous holder's command group is still on the device — try again in a moment" (no auto-retry).
- **Exclusive ops** (`actionBlocked`, `layoutBlocked`, `updateBlocked`, `tuneBlocked`, SAVE/REVERT/reboot/factory reset/safe-mode exit/rename/actions, Tune run, identification, OTA): allowed when Mine, or when nobody holds the lease and the device is disarmed; else blocked with "Needs the lease (held by <name>)". Reads, live/staged param SET, ACK, subscriptions stay open. Calibration actions follow the action rule.
- **Live edits while another client holds the lease:** a param write from Layout/Tune/the panel is held locally and `confirmLiveEdit` (property: item, text) asks; `applyConfirmedEdit(dontAskAgain)` / `cancelConfirmedEdit()`; opt-out lasts this connection only, reset on reconnect. Nothing is sent until Apply.
- **Clients:** `agentClients` on connect, on every Lease event and every 2 s while Device is shown → `clients` property (name, kind, holder, self).
- **History:** History events (captured in the early drain too, before the observer) decoded into `agentHistory` (time, kind, text from the device's diag names, arm state, lease change, online/offline), shown in the faults drawer under "Agent history" vs "This connection".
- **Device loss:** Closing(DeviceOffline) → link-lost overlay with "The agent lost the device"; Retry reconnects through the agent. A requested REBOOT of a serial device through the agent waits for Boot as today; a vdev reboot through the agent shows as device loss (known trap).
- **Recent agents:** `AppSettings`/INI: address, last device alias/id/name, token only when Remember is checked; forget removes the token.

## GUI QML

Opus: `ConnectPage.qml` Agent tab wiring, `Shell.qml` (lease chip, banner, dialogs routing, Esc/DISARM above scrims), `DevicePage.qml` card placement, `FaultsDrawer.qml` sections.
**Sonnet 5.5 (high), from written contracts reviewed by me, with Figma G6/G2 node ids:**
1. `style/DLLeaseChip.qml` (6 variants, `133:984`), `DLStatusBar` optional lease slot, `DLDialog` optional Confirmation checkbox; gallery entries; `tst_controls`/`tst_statusbar` cases.
2. `app/AgentDeviceList.qml` (device list from G2 components; recorded in `plans/gui-design.md`), `app/ClientsCard.qml`, `app/LeaseDialogs.qml` (take-from-ROS/other with unchecked safe-to-stop box, release confirmation, confirm live edit), the ROS banner component, the drawer's agent-history list — QML only, against a fixed property/signal contract.
3. `tools/wsl_serial.py attach|detach|status|run -- <cmd>` with `--dry-run`, `--busid`/`--vid-pid 1a86:55d3`: refuses unless bound/shared, `run` always detaches in `finally` (and on Ctrl+C), `status` reports owner; `tools/tests/test_wsl_serial.py` (mocked `usbipd`). `docs/development.md` row.
4. Remote screenshot scenarios in `host/gui/tests/scenarios.py` (Linux: agent + vdev) and the Linux remote backend test harness script, from my test list.

## Tests

- Wire: `test_agent_wire.cpp` (kind round-trips, kind > 3 refused, v2 refused with v3 body), `agent_tests` lease/history with kinds, `agent_integration.py` kind checks.
- `remote_model` pure tests (Windows + Linux, in `gui-backend`): lease states from event sequences incl. history replay, timed-out Take, revocation, ROS vs Other, exclusive-op gating table, live-edit confirmation opt-out reset.
- New `gui-remote` QtTest (Linux only, host-linux check; starts `drivelink-agent` + two vdevs): observer connect (no CMD_CONFIG, ARM disabled), Take → groups → ARM → manual command; second client (CLI `--lease`) takes → our groups dropped, disarmed, Revoked; DISARM from observer; exclusive op blocked while another holds / allowed when free and disarmed; live edit confirmation; history after reconnect; device offline → link lost; bad token / version / unknown device / offline states.
- `gui-quick`/`gui-gallery` for the new controls; screenshot scenarios for the 16 G6 states compared with Figma (Linux offscreen, 2560×1440 + 1920×1080, dark/light).
- `wsl_serial` unit tests in the `gen`/tools Python tests.

## Hardware check (PC, by the session after review)

User's one-time step: plug in the bench board; in an admin shell `usbipd bind --busid <CH343 busid>`.
Then the session: `wsl_serial.py run -- <agent with serial:///dev/ttyACM0>`; GUI (Windows) connects to the agent at 127.0.0.1:47620; observer; Take; ARM; CLI `--lease` second client takes → GUI shows Revoked and device disarmed; DISARM from the GUI as observer while the CLI holds and arms; REBOOT through the agent on serial → Boot path (map reset, ClockModel after Boot) — the H3a deferred check; port returned to Windows (`wsl_serial.py status`), board restored and checked on COM3 (refuses bench commands). Row in `plans/hardware-log.md`. No motors → no deferred row needed beyond noting that a remote motor check rides on the existing H2d-2 row (none added).

## Review

Independent review on Opus (high), required by the brief (lease and arming in the GUI, the remote command group) and the wire change. Focus: observer → owner transitions, revocation paths vs Tune/identify/manual commands, exclusive-op gating, generation guards, wire v3 consistency.

## Deviations / decisions to log

- User: split H3b-1/H3b-2; mjansson/mdns for H3b-2; INI token only with "Remember token" (checked by default); client kind in Hello (agent v3).
- Internal: remote mode inside `LocalConnection` rather than a second class; ClientList polled (no event); Busy/None ARM after handoff not retried automatically; client name `drivelink-gui@<hostname>`; kind > 3 refused.

## Docs

`docs/protocol/agent.md` (v3), generated registry docs, `host/gui/README.md` (Remote mode section), `host/agent/README.md`, `host/libdrivelink/README.md` (kind), `host/README.md` (CLI kind column), `host/CLAUDE.md` (GUI remote rule), `docs/development.md` (`wsl_serial.py`, `gui-remote`), `docs/bench.md` (WSL agent on the bench board), `plans/gui-design.md` (device list, H3b issues settled), `plans/host-steps.md` (H3b split, H4a kind Ros), status file, hardware log.

## Models

Opus 5.5 (high): plan, wire contract, backend, Shell/Connect/Device wiring, integration, fixes, review. Sonnet 5.5 (high) subagents in their own build directories: items 1–4 above from written contracts, reviewed by me. Exception row added to "Models and effort".

## Verification

`python tools/validate.py quick` while iterating; `full` (incl. `gen`, `host-win`, `host-linux` with `gui-remote`, `agent`, `deb`, `installer`) before review, read from `build/validate/summary.json`; then the PC hardware check above.
