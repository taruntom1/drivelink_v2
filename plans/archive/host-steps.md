# Host steps

The archived implementation briefs for the [host architecture](../host-architecture.md),
planned with the user on 2026-11-14. The workflow below records how the
overhaul ran. Each implementation step used `/overhaul-step` and opened a
PR into `host_overhaul`. The results and PRs are in
[host status](../host-status.md) and
[the finished step archive](host-overhaul-done.md). Each step's own plan
settled implementation details and recorded its choices.

## Rules for every host step

- **Order.** Library, then the GUI in local mode, then the agent and remote mode, then ROS (user decision). A step starts when the steps it depends on are merged. "May run beside" marks a step that can run at the same time in its own session and worktree.
- **Language and toolchains.** The host is C++23 with GCC 13.1 as the floor; `protocol/` stays C++20. Qt 6.12.0 on Windows, Qt 6.10 as the API floor. No module-specific code anywhere in the host.
- **Validation.** `python tools/validate.py full` passes before the PR: `build/validate/summary.json` reports `full_gate_passed` for the revision pushed. Sessions read that report (`docs/development.md`, "JSON report") rather than console output or whole logs. A new test suite becomes a `Check`, and CI calls it by name; new checks (`agent`, `ros`, the version check) appear in the report automatically. CLI output and exit codes stay unchanged unless the step says otherwise; new JSON fields may be added.
- **Docs in the same PR.** `host/README.md`, the READMEs of the parts touched, `host/CLAUDE.md` when a rule changes, `docs/protocol/` for any wire change, `docs/development.md` for new checks or tools.
- **Hardware.** "Bare board" means the session runs it on the bench board after review (mock modules only). Motors, a real IMU, the Pi and the robot are deferred: the step writes and dry-runs the script and adds its row to "Deferred hardware checks" in [host-status.md](../host-status.md); results go to [hardware-log.md](../hardware-log.md). The bench is described in [docs/bench.md](../../docs/bench.md).
- **Review.** Planned and built on Opus 5.5 at high effort. An independent Opus review at high effort is **required** where the table says so (concurrency, wire or persistence, arm/disarm, command paths, lease and auth), and otherwise the session reviews its own diff unless the diff touches those paths.
- **Size.** If a step passes about 4–5k lines, its plan proposes a split into rows (for example H3a-1 and H3a-2); the briefs below name the natural cut where one is likely.

## Overview

| Step | Content | Depends on | Hardware check | Review |
|---|---|---|---|---|
| H3d | Signed apt repository on GitHub Pages, published by tag releases | H3c | container; deferred: Pi | self |
| H4a | ROS toolchain (Lyrical and Jazzy), colcon packages, `drivelink_hardware` state path: connect through the agent, mapping, lease, `read()`; `ros` and `ros-jazzy` checks and CI | H3d | vdev; deferred: robot (read only) | independent |
| H4b | Command path: command interfaces, mode switch, ARM/DISARM, `write()`, `ERROR` on a disarm | H4a | vdev; deferred: robot (wheels off the ground) | independent |
| H4c | `drivelink_bringup`: example diff-drive + IMU, diagnostics node with `ack_fault` and `recover`, full-loop launch test | H4b | vdev; deferred: robot driving | independent |
| H4d | ROS `.deb` packages (bloom) for Lyrical and Jazzy, amd64 and arm64, in the apt repository | H4c | container; deferred: Pi | self |
| H6 | Cleanup, docs, merge `host_overhaul` into `main` | all H steps, the user guide (UG3 in [guide-steps.md](guide-steps.md)) | reuse earlier bare-board and installed-GUI results | self |
| HW | Hardware campaign: every open deferred row, whenever hardware arrives | — | motors, IMU, Pi, robot | per fix |

Earlier briefs, overview rows and the decisions they settled are in
[host-overhaul-done.md](host-overhaul-done.md).

## Left to the step plans

Settled by the step's own plan (an internal choice, logged), or put to the user there if it touches the wire, a safety rule or a later step.

| Item | Step |
|---|---|
| When to promote `host_overhaul` and retire its temporary workflow | Settled in [H6](#h6-close-out) |

Not planned: a descriptor disk cache (until connecting gets slow), a USB transport, a higher baud rate, Python bindings, installer signing, a devcontainer.

## H3d Signed apt repository

**Depends on** H3c.

Scope:
- An apt repository on this repository's GitHub Pages (a `gh-pages` branch): one suite per Ubuntu release, named by its codename (`noble` for 24.04 and 26.04's codename), component `main`, amd64 and arm64. The package versions carry the release (`<VERSION>~ubuntu24.04`, `~ubuntu26.04`; H3c, user decision 2026-11-20), so one pool can hold both builds of a package. Only `vX.Y.Z` tags publish it, from the release's packages. Dev builds stay CI artifacts, and the `.deb` files stay release assets as well.
- Signing: a dedicated signing-only key that the user creates offline (the step gives the commands) and adds as Actions secrets. The session never handles the private key. The public key lives in the repository and is served with the apt repository; users install it under `/etc/apt/keyrings/` and name it with `signed-by`. Until the secrets exist, CI builds and verifies the repository with a throwaway key and pushes nothing.
- A container test: serve the built repository over HTTP, add it with the public key, `apt install drivelink-agent drivelink-cli`, and check that a repository signed with another key is refused.
- The Pi guide's install section uses the repository, with the release files as the fallback.

Hardware check: container. Deferred row: `apt install` and later `apt upgrade`
on the Pi from the published repository.

Review: self; the PR states the release job's permissions and where the secrets are used.

## Decisions for every H4 step

Re-planned with the user on 2026-11-21. The architecture section "`drivelink_hardware` (ROS 2)" holds the design; these apply to H4a to H4d:

- **Distributions:** ROS 2 Lyrical (Ubuntu 26.04, the Pi) and Jazzy (Ubuntu 24.04). The code uses only the ros2_control API both share. Locally the `ros` check runs Lyrical in WSL, and `ros-jazzy` runs the same build and tests in the `ros:jazzy` image through Docker Desktop (skipped, with the one-line fix, when Docker is stopped). CI runs both.
- **Connection:** through the agent only. The plugin never opens a serial port; on a robot the agent owns the ports. Parameters: `agent` (default `tcp://127.0.0.1:47620`), `token_file` (default `/etc/drivelink/token`; the ROS user joins the `drivelink` group) or an inline `token` for tests and vdev, and `device` (alias or device id). The Hello sends `client_kind` Ros.
- **Lease at activation:** if another client holds the device's lease, `on_activate` fails with an error naming the holder (from the device record). A `take_lease: preempt` parameter takes it anyway. No wire change: LeaseTake itself always preempts.
- **Mapping:** a joint names its controller instance by label. The standard interfaces map by convention (state `position` → `position`, `velocity` → `velocity`; command `position` → `position_sp`, `velocity` → `velocity_sp`), and a parameter per interface overrides the item name. Open-loop duty is a custom `duty` interface (state `duty`, command `duty_sp`); claiming it sets the mode to `open_loop`. The IMU follows the ROS convention by default: orientation ← `quaternion.*`, angular velocity ← `gyro.*`, linear acceleration ← `accel.*` (with gravity), overridable the same way.
- **Arming:** ARM when a controller first claims a command interface, after the mode write and a fresh hold frame; DISARM when the last one is released and in `on_deactivate`. State-only use (broadcasters) never arms.
- **Recovery:** never re-arms by itself. A disarm, a lost lease, the device going offline or a stale group (`stale_timeout_ms`, default ten group periods and at least 100 ms; user decision 2026-11-21, H4a) makes `read()` return `ERROR`. The diagnostics node (H4c) offers `ack_fault` and `recover`; `recover` acknowledges the fault and reactivates the component and its controllers, so an operator or supervisor calling it is the explicit act that re-arms.
- **Packaging:** colcon from source in the robot's workspace (H4a to H4c), then bloom `.deb` files in the H3d apt repository (H4d).
- **Order:** strict, each step after the previous one merges.

## H4a ROS toolchain and `drivelink_hardware` state path

**Depends on** H3d. **Architecture:** "`drivelink_hardware` (ROS 2)".

Scope:
- Toolchain: a `ros` profile in `setup_linux.sh` (ROS Lyrical on 26.04 or Jazzy on 24.04, ros2_control, ros2_controllers, colcon, rosdep; skipping what is installed). `protocol/` and `host/libdrivelink/` build as colcon packages on both distributions with rosdep-clean `package.xml` files. The validate checks `ros` (WSL, Lyrical) and `ros-jazzy` (Docker); the CI jobs in `ros:lyrical` and `ros:jazzy`.
- `host/ros/drivelink_hardware`: the `SystemInterface` with state interfaces only. Parameters and mapping as above, joints and sensors (the IMU). `on_configure` connects through the agent, loads the device model and resolves every label and item (safe mode, a pending reboot, or anything missing or wrongly typed is a clear error). `on_activate` checks the holder, takes the lease and sets up one subscription group per loop that holds a mapped signal. `noexcept` `read()` from the seqlock buffers; `ERROR` when the lease is lost or the device goes offline. `on_deactivate` releases the lease.
- Its README: parameters, mapping, the token, and how to build it with colcon.

Tests: plugin unit tests (parameters, mapping, refusals); integration against vdev behind the agent: configure, activate, state values that follow the mock plant, the refusal while another client holds the lease, `preempt`, `ERROR` on lease loss and on the agent stopping. Both distributions.

Hardware check: vdev. Deferred row: the Pi's agent with the real ESP; the robot on stands, wheels turned by hand and the IMU tilted: joint positions with the right sign and scale, IMU values plausible.

Review: independent (the lease and the real-time read path).

## H4b Command path

**Depends on** H4a.

Scope:
- Command interfaces (`position`, `velocity`, `duty`) per joint, mapped as above; one command group per loop that holds a commanded input.
- `prepare_command_mode_switch` / `perform_command_mode_switch`: one command interface per joint at a time (joints may differ); write each controller's `mode`, set holds (current position, zero velocity, zero duty) and send a fresh frame after the write. ARM on the first claim, DISARM on the last release; a refused ARM fails the switch with the refusal detail in the log.
- `noexcept` `write()` that queues one frame and never blocks; NaN is sent as hold.
- `read()` returns `ERROR` on a disarm while armed (watchdog, fault, loop stall, the GUI taking the lease). Nothing re-arms by itself.

Tests: unit tests for the switch state machine and the hold mapping; integration against vdev behind the agent: claim and arm, commands move the mock plant, release and disarm, a mode change while armed, NaN as hold, a GUI-style Take from a second client, an injected disarming fault (`actuator.mock` `diagnostic`), a watchdog trip. Both distributions.

Hardware check: vdev. Deferred row: the robot, wheels off the ground, first-run procedure: each wheel through `forward_command_controller` in velocity, position and duty; polarity; release disarms; a Take from the GUI ends in `ERROR`.

Review: independent (arming, the command path, the lease).

## H4c `drivelink_bringup` and diagnostics

**Depends on** H4b.

Scope:
- `host/ros/drivelink_bringup`: a xacro macro for the `<ros2_control>` block, an example two-wheel diff-drive robot with an IMU (URDF), controllers YAML (`diff_drive_controller`, `joint_state_broadcaster`, `imu_sensor_broadcaster`), launch files with arguments for the agent, device and token, and a vdev layout for the example (two velocity controllers on mock actuators and encoders, an `imu.mock`). The real robot's config is added when it is on the bench.
- The diagnostics node uses its own agent observer connection (no lease, `client_kind` Ros) and publishes `/diagnostics` while the component is inactive or in `ERROR`. It reports the status stream, diag events, lease holder and online state. `ack_fault` only acknowledges the device fault. `recover` reactivates the launch-configured controller list (user decision 2026-11-21).
- The launch test, full loop, in both `ros` checks: `cmd_vel` drives the vdev plant, `/odom` and `/joint_states` change, `/imu` publishes; an injected disarming fault ends in `ERROR` and `recover` brings it back; a Take from a second client ends in `ERROR`.
- Docs: the bringup README, the recovery procedure, the token and group setup, and a ROS section in `docs/pi-setup.md`.

Hardware check: vdev. Deferred row: the robot driving (first-run procedure, then on the floor): `cmd_vel`, odometry against a measured distance, the IMU heading, a fault and `recover`, a GUI Take.

Review: independent (`recover` acknowledges faults and reactivates the hardware).

## H4d ROS `.deb` packages

**Depends on** H4c.

Scope:
- bloom-generated Debian packages of `drivelink_protocol`, `drivelink` (libdrivelink), `drivelink_hardware` and `drivelink_bringup` for Lyrical (resolute) and Jazzy (noble), amd64 and arm64, versioned from the root `VERSION` like the other packages (one version for the repo; `tools/version.py --check`).
- CI builds them, installs them in clean ROS containers, and runs the launch test from the installed packages against the installed agent. Tags publish them to the H3d apt repository next to the agent packages (and as release assets).
- Bloom uses `ros-<distro>-drivelink{,-protocol,-hardware,-bringup}` and the existing `<full-version>~ubuntu<release>` scheme. Dependencies between DriveLink ROS packages pin the exact Debian version. See the [apt package layout](../../packaging/apt/README.md#ros-2-packages).
- Docs: installing with apt (the ROS repository plus ours), beside the source build.

Hardware check: container. Deferred row: on the Pi after the next tag, `apt install` of the Lyrical arm64 packages and the H4c launch file against the real device.

Review: self.

## H6 Close-out

**Depends on** every H step and the last [user guide step](guide-steps.md).

- Archive the completed step briefs, update the repository documentation,
  and retire the temporary step skills, rules and `host_overhaul` push trigger.
- Run the full validation gate and self-review in the same Codex session,
  as approved by the user. Reuse the
  [earlier board and installed GUI results](../hardware-log.md) for this
  documentation and workflow change.
- Merge the cleanup into `host_overhaul`, then promote it to `main`.
  Keep v0.1.1 as the current release. The open hardware checks move forward
  to HW and fixes target `main` after promotion.

## HW Hardware campaign

Run the [open hardware checks](../host-status.md#deferred-hardware-checks)
as hardware arrives. Correct any guide wiring, polarity, steps or values
that differ from the real runs in the same PR as the fix.
