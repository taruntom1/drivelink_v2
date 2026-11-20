# Host steps

The steps that build the host in [host-architecture.md](host-architecture.md),
planned with the user on 2026-11-14. Each step runs in one session with
`/overhaul-step` and ends in one PR into `host_overhaul`. The state and PR of
every step are in [host-status.md](host-status.md); this file holds what
each step must do. A step session reads its brief, the architecture sections it
names and the status file, then writes its own plan for approval as the skill
says. A brief is a scope, not a design: the step plan settles the details and
logs its internal choices.

## Rules for every host step

- **Order.** Library, then the GUI in local mode, then the agent and remote mode, then ROS (user decision). A step starts when the steps it depends on are merged. "May run beside" marks a step that can run at the same time in its own session and worktree.
- **Language and toolchains.** The host is C++23 with GCC 13.1 as the floor; `protocol/` stays C++20. Qt 6.12.0 on Windows, Qt 6.10 as the API floor. No module-specific code anywhere in the host.
- **Validation.** `python tools/validate.py full` passes before the PR: `build/validate/summary.json` reports `full_gate_passed` for the revision pushed. Sessions read that report (`docs/development.md`, "JSON report") rather than console output or whole logs. A new test suite becomes a `Check`, and CI calls it by name; new checks (`agent`, `ros`, the version check) appear in the report automatically. CLI output and exit codes stay unchanged unless the step says otherwise; new JSON fields may be added.
- **Docs in the same PR.** `host/README.md`, the READMEs of the parts touched, `host/CLAUDE.md` when a rule changes, `docs/protocol/` for any wire change, `docs/development.md` for new checks or tools.
- **Hardware.** "Bare board" means the session runs it on the bench board after review (mock modules only). Motors, a real IMU, the Pi and the robot are deferred: the step writes and dry-runs the script and adds its row to "Deferred hardware checks" in [host-status.md](host-status.md); results go to [hardware-log.md](hardware-log.md). The bench is described in [docs/bench.md](../docs/bench.md).
- **Review.** Planned and built on Opus 5.5 at high effort. An independent Opus review at high effort is **required** where the table says so (concurrency, wire or persistence, arm/disarm, command paths, lease and auth), and otherwise the session reviews its own diff unless the diff touches those paths.
- **Size.** If a step passes about 4–5k lines, its plan proposes a split into rows (for example H3a-1 and H3a-2); the briefs below name the natural cut where one is likely.

## Overview

| Step | Content | Depends on | Hardware check | Review |
|---|---|---|---|---|
| H3c | Agent packages and Pi deployment: `drivelink-cli` split out, `drivelink-agent` (unit, udev rules, config, token), amd64 and arm64 in CI, Pi setup guide (replaces the old H3c script and H5) | H3b-2 | PC: packages in WSL with systemd; deferred: Pi | self |
| H3d | Signed apt repository on GitHub Pages, published by tag releases | H3c; may run beside H4a/H4b | container; deferred: Pi | self |
| H4a | `drivelink_hardware` (ros2_control), ROS in `setup_linux.sh`, ros check and CI | H3a | vdev; deferred: robot | independent |
| H4b | `drivelink_bringup`: example diff-drive + IMU, diagnostics node, launch test | H4a | vdev; deferred: robot | self |
| H6 | Cleanup, docs, merge `host_overhaul` into `main` | all H steps | bare board: full smoke and GUI | self |
| HW | Hardware campaign: every open deferred row, whenever hardware arrives | — | motors, IMU, Pi, robot | per fix |

T0 to H3b-2 are merged. Their briefs, overview rows and the items their plans settled are in [archive/host-overhaul-done.md](archive/host-overhaul-done.md).

## Left to the step plans

Settled by the step's own plan (an internal choice, logged), or put to the user there if it touches the wire, a safety rule or a later step.

| Item | Step |
|---|---|
| Pi UART wiring and the udev match for USB-attached ESPs (USB serial number; CH343 and native USB) | H3c |
| One packaging script or a sibling per package; Replaces/Breaks for the old `drivelink` package that held the CLI; the unit's hardening | H3c |
| reprepro or aptly; the key-generation commands given to the user; how the publish is dry-run before the secrets exist | H3d |
| How the ROS packages ship: colcon from source in the robot's workspace for now; whether to add bloom `.deb`s to the H3d repository is put to the user | H4a |
| Whether open HW rows must pass before `host_overhaul` merges into `main`; what of the temporary workflow is removed | H6 |

Not planned: a descriptor disk cache (until connecting gets slow), a USB transport, a higher baud rate, Python bindings, installer signing, a devcontainer.

## H3c Agent packages and Pi deployment

**Depends on** H3b-2. Runs next, before H4a. **Architecture:** "Agent deployment", "`drivelink-agent`", "CI".

**Re-planned (user decisions 2026-11-20):** the old H3c (an install script that built from source) and H5 (the agent `.deb`) merge into this step. The `.deb` is the only install path: a source build runs the packaging script on the Pi and installs what it makes with `apt install ./…deb`. The signed apt repository is H3d.

Scope:
- **Three packages**, versioned from `VERSION`, each built for amd64 and arm64:
  - `drivelink-cli`: the CLI and vdev, no Qt. Ubuntu 26.04 and 24.04.
  - `drivelink-agent`: the agent, its systemd unit, udev rules, config and token. It Recommends `drivelink-cli`. Ubuntu 26.04 and 24.04.
  - `drivelink` (H2g's package): the GUI only, and Depends `drivelink-cli` at the same version. Ubuntu 26.04 only, because 24.04's Qt 6.4 is below the 6.10 floor and nothing is bundled. Built for arm64 too, so a Pi desktop can run the GUI. Upgrading from H2g's package, which held the CLI, must work.
  - The 24.04 build also enforces the GCC 13 floor on Linux (24.04 ships GCC 13.2 and CMake 3.28). The ROS side stays 26.04 (Lyrical); 24.04 would be Jazzy.
- **First run** (`postinst`): a `drivelink` system user in `dialout`; a random token in `/etc/drivelink/token` (mode 0640, `root:drivelink`), made only when absent; `/etc/drivelink/agent.json` as a conffile with `bind` `127.0.0.1`, `token_file` and no devices; the service enabled and started, idling until devices are listed. LAN access means editing `bind`, which also turns on mDNS advertising. Remove keeps the config and token, and purge deletes them and the user.
- **Upgrade never restarts the agent** (safety, user decision 2026-11-20): stopping it DISARMs every device, so an `apt upgrade` must not stop a moving robot. The old process keeps serving, and `postinst` prints that `systemctl restart drivelink-agent` loads the new version. Only a fresh install starts the service; remove and purge stop it.
- **Devices are added by editing `agent.json`**: the guide shows how, and the helper below prints the device line to paste in.
- **systemd unit:** runs as `drivelink` with `--config /etc/drivelink/agent.json`, restarts on failure, stops with SIGTERM (the agent DISARMs every device), and carries hardening the plan picks.
- **ESP links:** the Pi's GPIO UART is the main link (`/dev/serial0` at 921600). A udev rule gives USB-attached ESPs (CH343 or the ESP's native USB) stable names, `/dev/drivelink/<serial>`, from the USB serial number. A helper prints the device line for a plugged-in board.
- **Pi setup guide:** Ubuntu 26.04 on the Pi 5, the GPIO UART with the serial console off, wiring (from the F8 row), installing the packages from the release, adding devices, the LAN bind, and checks with `drivelink-cli agent-devices` and `agent-browse`.
- **Tooling and CI:** the packaging script grows from `tools/package_linux.py` and `package_common.py` (stage under `/usr`, `dpkg-shlibdeps` Depends, no bundled library, staged smoke). The `deb` check covers every package. CI builds amd64 on `ubuntu-latest` and arm64 on `ubuntu-24.04-arm`, each in `ubuntu:26.04` and `ubuntu:24.04` containers (24.04 without the GUI). For every release and architecture, a clean container installs, upgrades (from a build with a lower version, checking the running agent is not restarted), removes and purges. The installed-GUI smoke (`installed_gui_smoke.py --deb`) runs on 26.04 for both architectures. Containers have no systemd as PID 1, so the unit is checked with `systemd-analyze verify` and the agent is started by hand with the installed config. The tag's `release` job attaches every package (release and architecture in the file name) and adds them to `SHA256SUMS`.
- **Notices** per package: the agent links libdrivelink, Asio, nlohmann JSON and mjansson/mdns, and the CLI links libdrivelink, Asio and nlohmann JSON.
- **Docs:** `host/agent/README.md` "Install", `host/README.md`, `docs/development.md`, `docs/bench.md` where WSL uses the packages.

Hardware check (PC, run by the session): in WSL with systemd, install the amd64 packages; the service starts on the loopback config; the bench board is lent through `wsl_serial.py` and listed in the config; `drivelink-cli agent-devices` answers; restart, remove and purge. Deferred row (Pi): install the arm64 packages, the service survives a reboot, the GPIO UART device comes online, and a LAN bind is found by the GUI on the PC (H3b-2's row runs here). The F8 Pi time row runs through the installed CLI.

Review: self (packaging; no wire, lease or arming change).

Natural cut if large: the CLI split, arm64 CI and release job first; then the agent package, deploy files and guide.

## H3d Signed apt repository

**Depends on** H3c; may run beside H4a or H4b.

Scope:
- An apt repository on this repository's GitHub Pages (a `gh-pages` branch): one suite per Ubuntu release, named by its codename (`noble` for 24.04 and 26.04's codename), component `main`, amd64 and arm64. The package versions carry the release (`<VERSION>~ubuntu24.04`, `~ubuntu26.04`; H3c, user decision 2026-11-20), so one pool can hold both builds of a package. Only `vX.Y.Z` tags publish it, from the release's packages. Dev builds stay CI artifacts, and the `.deb` files stay release assets as well.
- Signing: a dedicated signing-only key that the user creates offline (the step gives the commands) and adds as Actions secrets. The session never handles the private key. The public key lives in the repository and is served with the apt repository; users install it under `/etc/apt/keyrings/` and name it with `signed-by`. Until the secrets exist, CI builds and verifies the repository with a throwaway key and pushes nothing.
- A container test: serve the built repository over HTTP, add it with the public key, `apt install drivelink-agent drivelink-cli`, and check that a repository signed with another key is refused.
- The Pi guide's install section uses the repository, with the release files as the fallback.

Hardware check: container. Deferred row: `apt install` and later `apt upgrade` on the Pi from the published repository (after H6's first tag).

Review: self; the PR states the release job's permissions and where the secrets are used.

## H4a `drivelink_hardware`

**Depends on** H3a. **Architecture:** "`drivelink_hardware` (ROS 2)".

Scope:
- The ros2_control `SystemInterface` as the architecture describes: parameters, configure and activate, the lease, groups, the command mode switch with fresh hold values, ARM on the first claimed interface and DISARM on the last, `noexcept` `read()` / `write()` from the seqlock buffers and the command queue, NaN to hold, timestamps mapped to ROS time, `ERROR` on a disarm.
- Both connection modes: the agent (address, token and device alias or id), or a serial port directly. One hardware component per device. Through the agent its Hello sends `client_kind` Ros (agent protocol v3, H3b-1), so the GUI shows "ROS is driving" while it holds the lease.
- `setup_linux.sh` adds ROS Lyrical, ros2_control and rosdep (skipping what is installed); the `ros` validate check in WSL; the ros CI job in `ros:lyrical`.

Tests: plugin unit tests; integration against vdev through the agent and directly over a PTY.

Hardware check: vdev. Deferred row: the robot (first-run procedure).

Review: independent (real-time paths, arming, the lease).

## H4b `drivelink_bringup`

**Depends on** H4a.

Scope:
- An example two-wheel diff-drive robot with an IMU: URDF/xacro snippet, controllers YAML (`diff_drive_controller`, `joint_state_broadcaster`, `imu_sensor_broadcaster`), launch files.
- The diagnostics node: `/diagnostics` from the status stream and diag events, and a fault-acknowledge service.
- A launch test against vdev and the agent in the `ros` check; the documented recovery procedure.

Hardware check: vdev. Deferred row: the robot.

Review: self.

## H6 Close-out

**Depends on** every H step. Like F10:

- Remove stale text, archive finished step plans, bring `host/CLAUDE.md`, the READMEs and `docs/` in line with the code.
- Remove the temporary "Host overhaul in progress" section of the root `CLAUDE.md` and the overhaul workflow that only served it, as agreed with the user then.
- Bare board: the full smoke set and the installed GUI.
- After merge: merge `host_overhaul` into `main` and tag `vX.Y.Z` from `VERSION`. Whether open HW rows must pass first is decided with the user at that point.

Review: self.

## HW Hardware campaign

Unchanged: it runs every open row of "Deferred hardware checks" in [host-status.md](host-status.md) whenever hardware arrives, pausing the step in progress at a clean point. Host steps add rows for the GUI with a real motor (H2e) and a real IMU (H2f), the Pi (H3c, H3d) and the robot (H4a, H4b).
