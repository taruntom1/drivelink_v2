# H3b-2 — mDNS agent discovery (approved plan, 2026-11-20)

## Context

H3b-1 (PR #90, merged) gave the GUI remote mode through `drivelink-agent`, but an agent is only reachable by typing its address. The architecture ("Agent discovery", decided 2026-11-14) says that with LAN listening on, the agent advertises itself over mDNS/DNS-SD and the GUI's Agent tab lists what it finds. Library: mjansson/mdns (user decision 2026-11-19). Branch `claude/overhaul-H3b-2` from `origin/host_overhaul` (created), PR into `host_overhaul`. The status row for H3b-1 becomes `merged` in this PR.

User decisions (2026-11-20): service `_drivelink._tcp.local` with a minimal TXT; Settings gets a Connection section with a discovery toggle only; the hardware check runs with WSL switched to **mirrored networking**; work Sonnet 5.5 can handle is delegated to it.

## Scope

**In:** pinned mdns 1.4.3; the agent's responder (LAN binds only); the library's browse; CLI `agent-browse`; the Agent tab's "Found on the network" list; Settings > Connection "Find agents on the network"; notices; docs; tests; PC check.
**Not in:** IPv6 advertising/browsing (typed IPv6 addresses still work); RFC 6762 name-conflict probing; Settings' safety and sessions sections; anything on the agent protocol (stays v3).

## Discovery contract (new section "Discovery" in `docs/protocol/agent.md`, no version bump)

- Service type `_drivelink._tcp.local.`; instance = config `name` (default: the host name; 1..63 bytes UTF-8); target host `drivelink-<hostname>.local.` (own name, so it never collides with avahi's records for the host); port = the agent's port.
- TXT: `txtvers=1`, `proto=3` (agent protocol version), `version=<VERSION>`. No device data (the lobby still needs the token).
- Records per query: PTR (`_drivelink._tcp` and `_services._dns-sd._udp`) answered with PTR + SRV + TXT + A as additionals; SRV/TXT/A asked directly answered too. TTLs: 120 s host records (SRV, A), 4500 s PTR/TXT. QU questions and queries from a port other than 5353 get a unicast answer (legacy unicast echoes id and question). Two announcements 1 s apart at start, goodbye (TTL 0) at stop.
- Advertised only when the bind address is not loopback and `advertise` is on. A specific bind → A record for that address, only on its interface. `0.0.0.0` → every up, non-loopback IPv4 interface, each answering with its own address (`IP_MULTICAST_ALL=0`, per-interface membership); interfaces are re-scanned every 10 s (a Pi service may start before DHCP).
- A failure to open 5353 is a warning on stderr; the agent serves TCP regardless.

## Changes

**Dependency** — `host/cmake/Dependencies.cmake`: `drivelink_mdns` INTERFACE target (FetchContent, tag 1.4.3, SHA256 `be1fd8e3…0480483`, SYSTEM include so its warnings don't fail zero-warning builds). Notices: `host/gui/notices/notices.json` + licence text (public domain), regenerated `THIRD_PARTY_NOTICES.txt`; the .deb picks it up from the same file. Architecture "Dependencies" line names it.

**Library** (`host/libdrivelink`, Opus): new `include/drivelink/discovery.hpp`, `src/discovery.cpp` (packet parsing, merge), `src/discovery_net.cpp` (IPv4 interface listing: `getifaddrs` / `GetAdaptersAddresses` + `iphlpapi`).
- `struct FoundAgent {instance, host, port, addresses, from, proto (optional), version; std::string endpoint() const;}` — endpoint prefers the responder's source address when it is among the A records.
- `std::vector<FoundAgent> browseAgents(std::chrono::milliseconds window = 1000ms)`: one QU PTR query from an ephemeral port on every up IPv4 interface, answers collected until the window ends (blocking; callers use a worker thread). No follow-up queries (the responder always sends the additionals).
- Pure, tested: `parseResponse(bytes, from) -> partial records`, `mergeRecords`, `parseTxt`.

**Agent** (`host/agent`, Opus responder; Sonnet config): `src/mdns.cpp/.hpp` — `Advertiser` with its own thread (poll on its sockets, no shared state with the Asio thread), started from `Agent::start()` and stopped (goodbye) in shutdown. Pure, unit-tested in `agent_tests`: record-set building, which interfaces/addresses to advertise for a bind, the interface re-scan diff, instance/host name sanitizing. Config: `advertise` (bool, default true), `name`; flags `--name NAME`, `--no-advertise`; `config.cpp` + `test_config.cpp`; usage text. A stderr line names what is advertised.

**CLI** (Sonnet): `drivelink-cli agent-browse [--timeout-ms N]` (no endpoint): readable table (instance, address:port, protocol, version, host) and a JSON envelope `{agents: [...]}`; exit 0 with an empty list when none found. Existing commands unchanged.

**GUI backend** (Opus): `LocalConnection` gains `agentBrowse` property `{state: off|searching|idle, agents: [{instance, host, address, proto, version, compatible}]}` and `startAgentBrowse()`, `stopAgentBrowse()`, `browseAgentsNow()`; browse runs on its own worker thread (the `m_lister` pattern in `connection_remote.cpp`). Rounds at 0, 1, 3, 7, 15, 31 s, then every 60 s while the tab is shown (RFC 6762 back-off); "Search again" resets it. Pure `FoundAgents` in `remote_model.*`: an agent stays until missed in two rounds; `compatible` = `proto == 3`. `AppSettings::discoverAgents` (INI, default on); off = no browsing, no multicast. Scenario/test hook for a fixed found list (screenshots).

**GUI QML** (Sonnet, from my written contract): `AgentTab.qml` "Found on the network" section above the address (G6 layout recorded in `gui-design.md`): rows with instance, `address:port` in mono, version, an "Incompatible" badge when `proto` differs (row disabled with the reason), click → fills the address and the remembered token of a recent agent with that address, then lists devices if a token is present, else focuses the token field; "Searching…", the empty text ("Agents appear here when they listen on the LAN") and a Search-again icon button; a line when discovery is off in Settings. `SettingsPage.qml` Connection card with the toggle. QML tests and screenshot scenarios (`remote-agent-found`, `remote-agent-found-empty`, `settings` updated).

## Tests

- Library (host_tests, both OSes): golden response packets → `parseResponse`/`mergeRecords` (PTR+SRV+TXT+A, missing additionals, malformed/truncated packets, unknown TXT keys, endpoint choice); `parseTxt`.
- Agent (agent_tests, Linux): record sets per query type, legacy-unicast answer, bind → interface selection, re-scan diff, name sanitizing, config keys/flags; a loopback unicast round trip (responder answers to a 127.0.0.1 socket, the library parser reads it).
- `agent_integration.py` (Sonnet): agent bound to the host's non-loopback IPv4 → `agent-browse` finds it (address, port, proto 3, version); a loopback agent and `--no-advertise` are not found; skipped with a message when no non-loopback IPv4 exists.
- GUI: `FoundAgents` and back-off schedule (gui-backend, both OSes); Linux `gui-remote` case: real agent on the LAN address → `agentBrowse` lists it → click fills the address → device list; Settings toggle (shell tests); QML tests for the found list states; env-gated `benchDiscovery` in `gui_remote_tests` for the PC check.

## Hardware check (PC, after review; waits on you once)

You (one time): add to `%USERPROFILE%\.wslconfig`:
```
[wsl2]
networkingMode=mirrored
[experimental]
ignoredPorts=5353
hostAddressLoopback=true
```
then `wsl --shutdown` (`ignoredPorts` lets Linux bind 5353 although Windows' own mDNS holds it). If inbound multicast to WSL is then blocked, the Hyper-V firewall default needs allowing in an admin shell — I'll give the exact command only if needed.

Then the session: agent in WSL bound to the LAN address with vdev (no board needed; "Board state" unchanged) → `agent-browse` inside WSL finds it → `gui_remote_tests benchDiscovery` on Windows finds it and connects to its device → GUI screenshots of the found list. Also rerun `benchRemote`'s dry run to confirm mirrored mode keeps 127.0.0.1 forwarding. If same-host multicast between Windows and mirrored WSL still fails, I record it, keep the WSL-side result, and add a deferred row (GUI on Windows finds the agent on the Pi) for H3c/HW. Row in `plans/hardware-log.md`.

## Review

Self-review at high effort (the status row: H3b-2 "high, self"): the diff touches no lease, arming, command or access path; the agent gains a read-only UDP responder. Focus: untrusted packet parsing bounds, thread shutdown/goodbye, sockets on Windows/MinGW, no blocking on the GUI thread. I'll switch to an independent Opus review if the diff turns out to touch those safety paths.

## Deviations / decisions to log

User: TXT `txtvers/proto/version`, Settings discovery toggle, mirrored WSL for the check. Internal: IPv4 only; own target host name; no conflict probing; responder on its own thread; 10 s interface re-scan; browse back-off schedule and two-miss expiry; endpoint from the responder's source address; `advertise`/`name` config keys and flags; CLI `agent-browse`.

## Docs

`docs/protocol/agent.md` (Discovery), `host/agent/README.md`, `host/libdrivelink/README.md`, `host/README.md` (`agent-browse`), `host/gui/README.md` (found list, Settings Connection), `host/CLAUDE.md` (agent rule: responder never touches device state), `plans/gui-design.md` (found list layout, G1 frame 17 connection item), `docs/development.md` and `docs/bench.md` (mirrored WSL, discovery check), `plans/host-steps.md` ("Left to step plans" mDNS row settled), `plans/host-architecture.md` (Dependencies), status file (H3b-1 merged, H3b-2 row, handoff, Models exception, decisions), `plans/hardware-log.md`, this plan archived as `plans/archive/h3b-2-implementation.md`.

## Models

Opus 5.5 (high), this session: contracts (`discovery.hpp`, agent.md Discovery, the QML property contract), library browse and parsing, the agent responder, the GUI backend, integration, fixes, review, PC check. **Sonnet 5.5 (high) subagents**, each in its own build directory, from written contracts reviewed by me: (1) dependency pin + notices + agent config keys/flags with tests; (2) CLI `agent-browse` and the `agent_integration.py` discovery scenarios; (3) `AgentTab.qml` found list, Settings Connection card, QML tests and screenshot scenarios. Exception row in "Models and effort".

## Verification

`python tools/validate.py quick` / `--only` while iterating; one `full` (gen, host-win, host-linux with gui-remote, agent, deb, installer) before review, read from `build/validate/summary.json`; then the PC check above.
