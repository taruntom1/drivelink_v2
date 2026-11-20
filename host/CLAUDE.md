# Host rules

Read the root rules, [host architecture](../plans/host-architecture.md) for the
target, and [README.md](README.md) for builds/usage. Public service results,
descriptors, values and events are typed C++. JSON formatting/dependencies live
in CLI; preserve its readable, JSON/NDJSON and CSV output and exit codes.

- `libdrivelink` is C++23 without Qt/ROS/firmware dependencies. Public namespace
  is `drivelink`; protocol code is `drivelink::protocol`. Asio is private.
  Dependencies stay version/SHA256-pinned; never edit vendored source.
- Discover module types/values/actions from the device, never a compiled module
  table. Generated constants are for core/system IDs only. Share framing,
  Scanner and Reader/Writer; keep descriptor/scalar parsing pure/tested.
  Validate complete lengths/pages/counts and payload limits; mismatches are explicit.
- One Asio I/O thread owns sockets/serial/TX and periodic time. Sync callers
  serialize except emergency DISARM; never call sync on I/O. `Client` sync is
  single-caller; typed async callables own their inputs and run on the serial
  service executor with a temporary Client. Callbacks are dispatched FIFO,
  exactly once and in reply order, never inline/on I/O. Bound admission through
  callback consumption; never drop an accepted completion. DISARM reserves TX
  space as well as admission and bypasses ordinary service/TX queues.
- Transport failure completes pending work but retains the I/O work guard until
  explicit close. Serialize close with submission; a checked-but-not-yet-posted
  request must never land on a stopped context. Close must not depend on peer or
  application dispatcher progress. Bound event queues and expose drop counters.
  Release operation-owner locks before cancelling transport request handles:
  close joins I/O under its closing lock, and I/O completion needs the owner lock.
- DeviceModel retains Session, not a temporary service Client. Publish immutable
  shared snapshots, tag generations, invalidate readiness on Boot and reload
  complete metadata. Reload publication preserves current link/runtime/loss
  state; a delayed result cannot resurrect a disconnected model. Group data is
  pulled, not dispatched. Seqlock payload words must be atomic C++ objects.
- Managed commands use a bounded producer queue and prefix-only I/O keepalives.
  Consumers supply finite holds; no module-specific semantics. Flush both queued
  values and pending TX generations before mode changes. Boot, clear and close
  invalidate traffic; never replay values or arm automatically.
- Correlate type, sequence, family and op. Retry only known-idempotent reads;
  a mutation timeout has an uncertain outcome. Retire timed-out IDs until
  reconnect (or an opt-in `retire_quarantine`, for the agent's links that never
  reconnect); fail explicitly on exhaustion, except DISARM, which may reuse a
  retired ID. Preserve request family/op and
  outcome_unknown through service parsing and OTA cleanup; malformed mutation
  replies are uncertain, local preflight errors are not. Read classification
  and automatic retry eligibility are separate.
- Time ping IDs, boot/session clock generations and timeout retirement stay
  separate from core sequence IDs. Stamp on I/O; ClockEstimator stays
  single-caller, bounded and monotonic. See [time.md](../docs/protocol/time.md).
- OTA's WRITE retry exception permits only identical offset/data. Batch calls
  hold ownership, retire missing IDs and respect device window/chunk/payload
  limits (at most four outstanding). Reconcile uncertain BEGIN/FINISH/CONFIRM
  through STATUS; never automatically repeat REBOOT. A reboot reply is acceptance
  only: verify full running ELF identity, slot and core version before CONFIRM,
  then recheck identity. Cancellation never confirms. See
  [ota.md](../docs/protocol/ota.md).
- vdev uses actual core/mock modules, one config caller, fixed scratch/bounded
  diagnostics and firmware real-time rules. Native storage defaults to in-memory.
  Keep the `-vdev` version marker after simulated OTA so physical preflights refuse it.
- Agent ([agent/README.md](agent/README.md), contract
  [agent.md](../docs/protocol/agent.md)): nothing from a device reaches a
  connection before its Hello is accepted; refuse, then close. One agent
  thread owns sockets, routing and group maps; Session completions and frames
  come to it through the dispatcher, guarded by the device's link and map
  generations. Never close a connection inline while iterating clients. DISARM
  from any client goes out at once on the fast path (it waits, bounded, only
  when the Session cannot admit it). ARM, CMD_CONFIG/CMD_CLEAR and command
  frames reach a device only from the lease holder; exclusive operations
  only from the holder or while nobody holds it and the device is not armed.
  A lease end stops the old holder at once, DISARMs, clears its command
  groups (an ARM still in flight gets another DISARM first), then grants a
  waiting Take; ARM waits while an earlier holder's command slot remains. On
  every device open: GET_INFO, DISARM, clear all groups, then accept clients.
  Keep routing, the group map, LIST, the lease and history pure
  (`groups.cpp`, `lease.cpp`, `history.cpp`) with unit tests. Agent clients
  ping the agent, never the device, and compose its ClockModel. The mDNS
  responder (`mdns.cpp`, own thread) answers only from fixed settings and
  never touches a device, a client or the lease; it advertises only on LAN
  binds, and parsing a received packet never throws.
- GUI remote mode (H3b-1, [gui/README.md](gui/README.md) "Remote mode"): a
  `LocalConnection` through the agent with its Hello's `client_kind` Gui. It
  observes first and sets up command groups only while it holds the lease;
  the agent's Lease events are the truth. Losing the lease forgets the
  command groups without CMD_CLEAR (the agent cleared them) and cancels an
  ARM in flight; a grant never arms. Take from another holder needs the
  operator's explicit safe-to-stop confirmation. Exclusive operations follow
  the agent's rule (`exclusiveBlocked()`). A live param edit while another
  client holds the lease is held unsent until the operator applies it (the
  opt-out lasts one connection). A token is stored only when the user asks.
- ROS plugin ([ros/drivelink_hardware/README.md](ros/drivelink_hardware/README.md)):
  only through the agent (Hello `client_kind` Ros), never a serial port; only
  the ros2_control API Jazzy and Lyrical share. Lifecycle callbacks may block
  (bounded); `read()` is `noexcept` with no allocation, blocking lock or device call,
  and learns of agent events, link loss and a Boot only through atomics set
  by the session observer. A lost lease, Closing, link loss, a Boot, dropped
  observer events or a stale group (`stale_timeout_ms`) returns `ERROR`;
  `on_error` only disconnects (no device call in the controller_manager
  loop; the agent clears the groups and lease), and nothing re-arms or
  reconnects by itself. Activation refuses another holder unless
  `take_lease: preempt`; any failure after the Take, or an unconfirmed
  release, disconnects. Mapping stays pure (`mapping.cpp`, unit-tested).
- CLI `--json` stdout contains only envelopes/NDJSON; errors retain exit code,
  device result and detail.
- GUI ([gui/README.md](gui/README.md)): Qt 6.10 API floor. Figma is the source
  of the tokens; `tokens.json` and `Theme.qml` are generated by `gen_theme.py`,
  never hand-edited. Controls take every colour, size and font from `Theme`,
  only draw and emit requests, and contain nothing module-specific. Keep
  DISARM reachable: no Qt modal popups (they block the status bar). New icons
  come from the Figma G2 set, at the bundled Lucide version.
- GUI completions/model notifications run on the GUI thread through a FIFO
  session dispatcher. Keep its receiver alive through callback drain; stale
  connection/session generations cannot publish or arm. Bound initial event
  draining and preserve DISARM requested before link publication or behind an
  earlier failed DISARM. Observer close waits for an admitted safety request;
  an unconfirmed forced close must report an uncertain outcome.
- GUI command groups initially send prefixes only. Never infer a semantic hold
  from descriptor defaults. An already-armed direct connection observes until
  explicit DISARM acknowledgement, including an armed state seen during
  discovery followed by watchdog disarm. Never adopt other owners' groups;
  reconcile uncertain setup and clear only matching owned generation/epoch/refs.
- GUI OTA uses the library workflow (`drivelink/ota.hpp`) and the reboot
  expectation every reboot uses: REBOOT once, never repeated; CONFIRM only after
  `classifyOtaBoot` matches the candidate this GUI uploaded, at most once
  automatically, never after a roll back or cancel. Without a known candidate
  only the user's explicit Confirm sends CONFIRM. An active update blocks ARM
  and configuration actions; an unknown or Ready OTA state blocks reboots.
  The image picker is non-modal and offered only while disarmed; nothing arms
  while it is open. The only other native dialogs are Tune's session and CSV
  file dialogs (non-modal, allowed while armed; user decision 2026-11-18) and
  a custom panel's own file dialogs (the same rules): Esc goes to the dialog
  while it is open, DISARM stays clickable.
- Custom panels (H2f): module-specific GUI code exists only in
  `gui/modules/<type name>/`, loaded by the device's type name; its logic is
  typed C++ with QtTest tests, registered through `registerModulePanels()`. A panel uses
  the generic backend surface (live rows, actions by key sent once, action
  results, bounded text files) and adds no backend, wire or safety rule. The
  IMU calibration panel sends `calibration_apply` only after
  `calibration_write` replied ok, never re-sends, and records an operation
  interrupted by unloading as "may have run".
- GUI loop-statistics subscriptions, the Layout live group and Monitor's groups
  (one per plotted loop) use free slots only, after command setup (live and
  Monitor after loop statistics, one mutation at a time), record intent before
  SUB_CONFIG, and are reused or cleared only on an exact owned LIST match; a
  successful configure drops every other record for that slot; a SUB_CONFIG
  that lands on a newer boot releases its slot or records it under that boot.
  The Layout live group exists only while Layout is shown. Monitor refuses an
  add without a free slot or over 100 % of the link, and caps a group at
  1 kHz (the firmware drains each group once per 1 ms).
- Hold rule (user decision 2026-11-17, built in H2d-2): only numeric inputs
  whose descriptor range is finite and includes 0 are commanded; each loop's
  inputs split at connect into value groups (hold 0) and keepalive-only groups
  that never carry values; every value sent is a shown slider value or 0.
  Manual commands need this window's accepted ARM; DISARM, faults, Boot and
  close return values to 0 locally. A held slider that loses its press ramps
  to 0; Latch survives page changes. A live param write to a commanded
  instance discards its queued values and holds it at 0 until the write ends,
  then sends a fresh frame (user decisions 2026-11-18).
- Tune (H2e-1): gains are the live params a descriptor flags `tune`, never
  names. One run at a time, only with this window's accepted ARM; CONFIG, ARM
  and START are sent once, never retried. The run suspends the controller's
  manual commands at 0 like a live write and ends that hold only after the
  signal has ended on the device (TestgenDone or STATUS, never before STOP has
  landed), so a commandable target returns to 0. Boot, link loss and close end
  the run without device calls. The mode, Revert and Restore are locked during
  a run; the gains too while Auto re-run is off.
- Tune Auto re-run (H2e-2, user decisions 2026-11-18): a run starts by itself
  only after the user's Run under this window's current ARM (any DISARM,
  fault, Boot, link loss or controller change needs a new Run), only while
  Tune is shown, through the same start path as Run, 0.5 s after the gains
  are still and only when they differ from the last run's. With it on, a gain
  change during a run cancels it: STOP and ABORT once, the run is not kept,
  and the controller stays at 0 until STATUS shows the signal stopped. Opening
  a session never writes to the device; Restore matches a recorded set by
  controller type, label and gain names, never by item ids alone.
- Tune plant identification (H2e-3, user decisions 2026-11-18): one
  open-loop step through the Tune run path (CONFIG, ARM, START once), only
  with this window's ARM and the operator's "can move freely" confirmation
  (cleared by DISARM). The step input must be commandable (the hold rule), so
  it returns to 0. The controller is held at 0 from the start. A
  tune-flagged mode value (`open_loop` by default) is written first. The
  value to restore is read from the device, not the snapshot; if the write is
  refused, nothing runs. Whatever ends the step, once the signal has stopped
  a fresh frame returns the input to 0. The previous value is then written
  back with the controller still held; a local block delays that write but
  never cancels it. The hold ends only after the write-back. Boot and close
  write nothing back (close says so). A step never starts under another ARM
  than the one it was confirmed under. Until the write-back ends:
  - the gains, mode, Restore, Revert, Run and Auto re-run wait, as do SAVE,
    REVERT, reboots, rename and OTA;
  - it never grants Auto re-run's permission.

  Suggested gains need an open-loop step, and only the operator's Apply
  writes them (one live batch, a history entry). The fit runs on a worker
  thread. Mapping is by the naming conventions in
  `docs/adding-a-module.md`, never module names.
- A requested reboot is never
  re-sent: the GUI waits for Boot or reopens the link within a bounded window;
  an unrequested loss waits for the user's Retry. Listing serial ports never
  opens one (DTR/RTS can reset an ESP).
- GUI Layout sends each SET and layout op once, then `refreshConfigAsync`; an
  unanswered one is uncertain and read back, never retried. A param field sends
  on Enter or focus loss only when its text differs from the device value and
  the text last sent. Esc and DISARM (which takes no focus) cancel an open edit
  unsent. Layout ops and reboot params are locked while armed; ARM is disabled
  while a reboot is pending. Rules come from descriptors (loop masks, flags,
  `ref:` categories, generated source signals), never from type names.
- GUI actions (RUN) are sent once, one at a time, after the input is checked
  against the descriptors; no reply is "may have run", never retried, then
  `refreshConfigAsync`. Paused actions on a real-time loop are blocked while
  armed or arming; loop actions stay enabled. Forms are inline, never modal.
- Distribution (H2g): versions come from the root `VERSION` through
  `cmake/DriveLinkVersion.cmake`, never a literal. Anything the installer
  bundles (a dependency, Qt module or plugin) needs a `gui/notices/notices.json`
  entry and its licence text; regenerate with `gen_notices.py --write`.
  Staging refuses an unlisted DLL. The installer's AppId never changes. Release
  installers come from CI's open-source Qt. The Linux `.deb` packages
  (`drivelink-cli`, `drivelink-agent`, the GUI's `drivelink`; H3c) bundle no
  shared library (Qt comes from Ubuntu's packages); a new `import Qt…` in the
  GUI needs its `qml6-module-*` package in `tools/package_linux.py` Depends.
  The CLI and agent notices are `host/notices/`, kept equal to the GUI's
  entries. The agent package's maintainer scripts never stop or restart the
  service on upgrade (stopping DISARMs every device; user decision
  2026-11-20): only remove stops it, only a first install or a reinstall
  after remove starts it.
- Windows host binaries use Qt's MinGW kit exclusively. Firmware native tests
  use MSYS2 UCRT64 GCC 16.2; keep build dirs and runtime DLLs separate. The root
  `full` gate includes Linux/MinGW unit/integration and installed-package smoke checks; host-linux CI
  must pass. Hardware smoke runs only on the bare bench board.
