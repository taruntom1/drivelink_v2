# Host rules

Read the root rules, [host architecture](../plans/host-architecture.md) for design decisions, and [README.md](README.md) for builds and usage. The public `libdrivelink` API exposes typed C++ service results, descriptors, values and events. The CLI owns its readable, JSON/NDJSON and CSV output and exit codes.

## Subsystem rules

When editing shared tests in `host/test/` or changing another subsystem's integration with one of these components, read the rules for the behavior being exercised.

- [libdrivelink](libdrivelink/CLAUDE.md) covers transport, session lifetime, service execution, model publication and OTA.
- [GUI](gui/CLAUDE.md) covers operator controls, GUI dispatch, command ownership and device workflows.
- [Agent](agent/CLAUDE.md) covers routing, leases, connection teardown and discovery.
- [ROS hardware plugin](ros/drivelink_hardware/CLAUDE.md) covers lifecycle, watchdog ownership and real-time callbacks.

## Shared contracts

- Discover module types, values and actions from the device. Generated constants identify core and system IDs. Use the shared framing, Scanner and Reader/Writer.
- Serialize synchronous calls on a Client except emergency DISARM. Never make a synchronous call on the I/O thread. Agent clients ping the agent and compose its ClockModel.
- Treat timed-out mutations and malformed mutation replies as uncertain. Automatically retry only eligible idempotent reads, with the documented OTA WRITE exception for identical offset and data. Reconcile uncertain OTA transitions through STATUS.
- Command producers supply finite holds. Flush queued values and pending TX generations before changing modes. Boot, clear and close invalidate traffic. Never replay values or arm automatically.
- Keep dependencies version/SHA256-pinned and vendored source untouched.

## Tools and distribution

- vdev uses actual core/mock modules, one config caller, fixed scratch/bounded diagnostics and firmware real-time rules. Native storage defaults to in-memory. Keep the `-vdev` version marker after simulated OTA so physical preflights refuse it.
- CLI `--json` stdout contains only envelopes/NDJSON; errors retain exit code, device result and detail.
- Distribution (H2g): versions come from the root `VERSION` through `cmake/DriveLinkVersion.cmake`, never a literal. Anything the installer bundles (a dependency, Qt module or plugin) needs a `gui/notices/notices.json` entry and its licence text; regenerate with `gen_notices.py --write`. Staging refuses an unlisted DLL. The installer's AppId never changes. Release installers come from CI's open-source Qt. The Linux `.deb` packages (`drivelink-cli`, `drivelink-agent`, the GUI's `drivelink`; H3c) bundle no shared library (Qt comes from Ubuntu's packages); a new `import Qt…` in the GUI needs its `qml6-module-*` package in `tools/package_linux.py` Depends. The CLI and agent notices are `host/notices/`, kept equal to the GUI's entries. The agent package's maintainer scripts never stop or restart the service on upgrade (stopping DISARMs every device; user decision 2026-11-20): only remove stops it, only a first install or a reinstall after remove starts it.
- Windows host binaries use Qt's MinGW kit exclusively. Firmware native tests use MSYS2 UCRT64 GCC 16.2; keep build dirs and runtime DLLs separate. The root `full` gate includes Linux/MinGW unit/integration and installed-package smoke checks; host-linux CI must pass. For each hardware smoke check, follow its documented equipment and operator confirmation requirements. Record the check as pending in [host status](../plans/host-status.md) if either prerequisite is missing.
