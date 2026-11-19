# H2g plan: installer and local GUI close-out

Branch `claude/overhaul-H2g` from `origin/host_overhaul` (`1da9074`), PR into
`host_overhaul`. One PR, about 2.5–3.5k lines including licence texts (no
split). User answers before the plan (2026-11-19): Inno Setup installed here
with winget; a nav-rail **Settings** page with Appearance and About; `VERSION`
starts at **0.1.0**; Sonnet 5.5 builds the installer scripts, the Settings QML
and the notices from written contracts.

## Scope

In: the root `VERSION` and its consumers with a check; a Release staging
directory and an unsigned Inno Setup installer, built locally by a new
`installer` check and by host-windows CI as an artifact; third-party notices in
the installer, the install directory and the GUI; the Settings page (theme,
About); close-out of remaining responsive and theme defects found by a full
screenshot pass; `host/README.md` GUI usage; design records.

Out (later or not planned): signing (not planned), the `.deb` (H5), the
`vX.Y.Z` tag (H6), G1 Settings' safety, connection and sessions sections (not
built; nothing needs them yet), Linux packaging.

## Changes

### 1. Version (session)

- `VERSION` = `0.1.0`. `tools/version.py`: the base version; the full version
  is the base at a clean checkout of tag `v<base>`, else
  `<base>+g<sha7>[.dirty]` (fits the 31-character `esp_app_desc` field);
  without git, the base. `--check` compares every consumer below with the base.
- `cmake/DriveLinkVersion.cmake` (repo root, shared): reads `VERSION`
  (configure dependency) and git at configure time; sets
  `DRIVELINK_VERSION` (numeric, for `project(VERSION)`) and
  `DRIVELINK_VERSION_FULL`. Used by `firmware/CMakeLists.txt` and the HIL app
  (`PROJECT_VER` only when not given, so `bench_fw.py`'s `f9-candidate`
  override keeps working), `protocol/`, `libdrivelink/`, the superbuild.
- `package.xml` (protocol, libdrivelink) stay literal `0.1.0`; `--check`
  catches drift. vdev reports `<base>-vdev` (deterministic for the smokes);
  `ota_smoke.py` reads the base instead of the literal.
- A generated `drivelink_version.hpp` (configure time) gives the GUI and CLI
  the full version. CLI gains nothing visible (output unchanged).
- `gen` check gains the step "versions match" (`tools/version.py --check`);
  tests in `tools/tests/test_version.py` (suffix rules with a fake git,
  mismatch reports).

### 2. Release staging and installer (Sonnet, from my contract)

- Preset `windows-mingw-release` (Release, `BUILD_TESTING=OFF`, own binary dir).
- `tools/package_win.py [--skip-build] [--no-installer] [--out build/installer]`:
  configure/build the release preset; stage `drivelink-gui.exe`,
  `drivelink-cli.exe`, `drivelink-vdev.exe`; `windeployqt --release
  --qmldir host/gui --compiler-runtime --no-translations`; copy `LICENSE` and
  the notices; **refuse any staged DLL/plugin not mapped to a notices entry**
  (a pure function, unit-tested); a self-contained smoke with Qt and MinGW
  removed from PATH (`drivelink-cli --help`, `drivelink-gui --screen connect
  --out` offscreen, vdev starts and answers the CLI); then ISCC with
  `/DAppVersion=<full>` → `build/installer/DriveLink-<full>-setup.exe`.
- `host/gui/installer/drivelink.iss`: fixed AppId, x64, per-user by default
  with an all-users choice, Start-menu entry for the GUI (optional desktop
  icon), the proprietary `LICENSE` page, the notices as the info page and in
  `{app}`, uninstaller; no PATH change. No app icon file exists: Inno's
  default (logged).
- `validate.py`: new Windows-only `installer` check (Unavailable with the
  winget command when ISCC is missing). CI host-windows runs `--only host-win
  installer` (installs Inno Setup when the runner lacks it, pinned) and uploads
  the setup exe as an artifact.

### 3. Third-party notices (Sonnet assembles, I check)

`host/gui/notices/THIRD_PARTY_NOTICES.txt` plus a small manifest
(`notices.json`: component, version, licence, source URL, DLL patterns).
Covers Qt 6.12.0 (the deployed modules and their bundled third-party parts,
from Qt's SBOM files) under LGPLv3 with the GPLv3 text it references and the
relinking terms (dynamically linked, replaceable DLLs, source location), the
MinGW runtime (GCC Runtime Library Exception, winpthreads), Inter and JetBrains
Mono (OFL 1.1), Lucide (ISC), Asio (BSL-1.0), nlohmann JSON 3.11.3 (MIT).
Compiled into the shell's resources for About; the same file in the installer.

Note: the local Qt install carries an educational licence file. Release
artifacts come from CI's aqtinstall (open-source Qt), as the notices state;
the local installer is for the bench check only.

### 4. Settings page (backend: session; QML: Sonnet)

- Backend `AppSettings` (C++, `drivelink_gui_backend`): `theme`
  (`dark|light|system`, default `dark`) in the existing GUI INI
  (`appearance/theme`, honours `--settings`), `dark` resolved from
  `QStyleHints::colorScheme` for `system`; `version`, `qtVersion`, `compiler`,
  `licence` and `notices` (from resources). The shell binds `Theme.dark`.
  Screenshot mode still renders both themes and never writes the setting.
- QML: a nav-rail footer item **Settings** (`settings` icon), enabled whenever
  no modal is open, also before connecting; a Settings page with Appearance
  (Dark / Light / Follow system) and About (name, full version, Qt and compiler,
  "Proprietary, all rights reserved", a scrollable notices view with Copy).
  Inline, no Qt modal popups, DISARM unaffected; Esc behaves as elsewhere.
  Built from G2 controls only; no new control or icon.
- Tests: QtTest for `AppSettings` (persistence, `system`, bad values) and a
  Quick Test for the page; screenshot scenarios `settings` and
  `settings-light` (connected and not), both sizes.

### 5. Close-out (session)

Run every existing screenshot scenario at both sizes in both themes; fix
small responsive or theme defects in this PR, list anything larger in the
handoff. Record what was built in `gui-design.md` (Settings from G1 frame 17,
partial) and `gui-design-status.md`.

## Tests

`tools/tests/test_version.py`, the package script's DLL-mapping tests (in
`tools/tests`), `AppSettings` QtTest, `tst_settings.qml`, two scenarios, the
staged self-contained smoke inside the `installer` check. Full gate:
`python tools/validate.py full` with the new `installer` check.

## Hardware check (bare board, after review)

`host/test/installed_gui_smoke.py --installer <setup.exe> --endpoint
serial://COM3 --cli <cli> --out <dir>` (written by me, dry-run on vdev first):
silent per-user install into a temp directory; records the device's layout and
settings with the CLI; stages and saves a mock layout (actuator/feedback mocks
under the controller, `imu.mock` in the sensor loop), reboots; renders with the
**installed** GUI, Qt off PATH: connect, device, drawer, layout, the
Calibration tab on `imu.mock`, tune, monitor, settings, both sizes and themes;
restores the recorded layout/settings, saves, reboots, verifies the board
matches "Board state" (disarmed, no pending/dirty, bench commands refused);
uninstalls silently. No firmware flash: the only firmware change is the
version string (checked from the built ELF's app description). Logged in
`hardware-log.md`. Nothing deferred, nothing for you to do.

## Review

Self-review (brief: self; no safety, wire, persistence-on-device or command
path is touched). I review every Sonnet deliverable against its contract before
it enters the gate.

## Delegation (exception row in the status file)

Opus 5.5 (high, this session): plan, version plumbing, CMake, firmware,
`AppSettings`, `validate.py`/CI, the hardware script, close-out fixes, review,
docs. Sonnet 5.5 (high) subagents, each from a written contract: (a) the
`.iss` and `package_win.py` with its tests; (b) the notices file and manifest;
(c) the Settings QML, its Quick Test and scenarios (after the backend lands).
Builds never share a build directory concurrently (H2f trap).

## Deviations (logged, no wire/safety/later-step change)

Version format and configure-time stamping; vdev keeps the base version;
Settings sections limited to Appearance and About; default Inno icon; no PATH
change; installer check runs in `full` on Windows only.

## Docs

`host/README.md` (install and GUI usage), `host/gui/README.md` (Settings,
notices, packaging), `docs/development.md` (`version.py`, `installer` check,
Inno Setup), `host/CLAUDE.md` (a bundled dependency needs a notices entry;
version comes from `VERSION`), `firmware/README.md` (app version), root
`CLAUDE.md` cross-area line on `VERSION`, `gui-design.md`,
`gui-design-status.md`, status file.
