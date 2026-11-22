# User guide steps

The steps that build DriveLink's user guide, a website on this repository's
GitHub Pages for people who use only the released artifacts. Planned with the
user on 2026-11-22. Each step runs in one session with the `guide-step` skill
and ends in one PR. The state and PR of every step are in
[guide-status.md](guide-status.md); this file holds what each step must do. A
brief is a scope, not a design: the step's plan settles the details and logs
its internal choices.

## Rules for every guide step

- **Order.** UG1, UG2, UG3, each after the previous one merges. All three come before the host overhaul's close-out (H6 in [host-steps.md](host-steps.md)).
- **Branches.** Branch from and target the integration branch named in [guide-status.md](guide-status.md) ("Branches"): `host_overhaul` until H6 merges it into `main`.
- **The reader.** Someone with only the released artifacts (the firmware zip, the Windows installer, the `.deb` files and the apt repository) and no ROS or embedded experience. The guide never asks them to clone, build or read the source, and it never names a repository path as something they need.
- **Product, not guide.** A guide step changes the site, its build and publishing, the GUI screenshot scenarios and the docs. It does not change the firmware, the wire, the host programs or a safety rule. A product bug or gap found while writing goes into "Product issues found" in the status file and to the user. It is fixed by a host or HW PR. The guide describes the product as it is.
- **Validation.** `python tools/validate.py full` passes before the PR, including the `guide` check from UG1 on. Read the result from `build/validate/summary.json` (`docs/development.md`, "JSON report").
- **Commands are tried.** Every command the guide gives runs once from the released or CI-built packages before the PR: against vdev (behind the agent where the page uses one), in the ROS containers (`ros-deb-install`), or in WSL. Commands that need motors, a real IMU, the Pi or the robot are dry-run against mocks. Their real runs are already rows in "Deferred hardware checks" in [host-status.md](host-status.md), and HW corrects the guide if those runs differ.
- **Browser checks.** Every step checks the built site at desktop and phone width, in light and dark, by keyboard and in print, and searches for the new pages.
- **Docs in the same PR.** `guide/CLAUDE.md` when a writing rule changes, `docs/development.md` for the build, check and commands, `packaging/apt/README.md` for any publishing change, and the repository docs whose user text moved into the guide.
- **Review.** As the table says. An independent reviewer is a separate session or agent that did not write the diff, at the effort in the status file's "Models and effort".
- **Size.** If a step passes about 4–5k lines (vendored files and generated output excluded), its plan proposes a split into rows such as UG2-1 and UG2-2.

## Overview

| Step | Content | Depends on | Checks | Review |
|---|---|---|---|---|
| UG1 | The site: guide sources, the build and `guide` check, the theme from the GUI tokens, Pagefind, the web flasher, generated screenshots, publishing with the apt repository on tags; the home page, the downloads page and `guide/CLAUDE.md` | H4d | browser; bare board: a first flash from the built flasher (ask first) | independent for the workflow and publish changes, self for the rest |
| UG2 | Start here, the concept primers and glossary, the PC bench journey with the reference build and clickable wiring diagrams | UG1 | browser; GUI steps against vdev | self |
| UG3 | The Pi robot and ROS 2 journeys, the reference (partly generated), troubleshooting and FAQ, About; user text moves out of the repository docs | UG2 | browser; commands in the ROS containers and WSL | self |

## Decisions

Settled with the user on 2026-11-22. A step needs the user's agreement to change any of them.

- **Journeys.** Three end-to-end journeys: the PC bench with the GUI (Windows or Ubuntu, the ESP on USB), the Pi robot with the remote GUI, and the ROS 2 robot. The CLI gets no page and stays documented in the repository.
- **Beginners.** Short concept primers written around DriveLink, one screen each, that link to the official ROS, Espressif and Ubuntu docs for depth. Every term is explained or linked the first time it appears, and a glossary holds them all.
- **Unverified hardware.** The motor, IMU, Pi and robot pages are written as finished, with no "not yet verified" markers. HW corrects the guide in the same PR as any fix when a deferred hardware row finds a difference.
- **Hardware.** One reference build with parts and wiring, plus a page on what else works and the limits (PWM+DIR drivers, quadrature encoders, 3.3 V logic, pins). Suggested parts, confirmed with the user in UG2's plan: ESP32-S3-DevKitC-1, Cytron MDD10A, 12 V encoder gear motors (JGB37-520 class), Adafruit BNO055, Raspberry Pi 5, a fused 12 V supply and a buck converter.
- **Structure.** Home → Start here → Concepts → the three journeys → Reference → Troubleshooting and FAQ → About. Every step that can move a motor repeats the first-run procedure as a safety callout: wheels off the ground, a current-limited supply, a low duty cap and a hand on the power switch.
- **Site.** Hand-built, with no site generator.
  - Content is Markdown under `guide/`. A Python script in `tools/` renders it into hand-written HTML templates with our own CSS and JS, builds the navigation and checks links.
  - Search is Pagefind, a pinned build tool that indexes the built HTML.
  - The design is done in code from the GUI tokens, with no Figma phase. `host/gui/theme/gen_theme.py` also writes the guide's CSS variables (light and dark) from `tokens.json`, so the GUI and the site share one colour source.
  - Fonts and icons are the GUI's bundled Inter, JetBrains Mono and Lucide (`host/gui/resources/`).
  - Every script, font and image is self-hosted: no CDN and no analytics.
  - The site works at phone width, by keyboard and in print.
- **Interactive pieces.** Three: the home page's "what do you have?" path picker, clickable SVG wiring diagrams of the reference build, and the web flasher. A control-loop playground and a troubleshooting wizard are not planned.
- **URL.** The guide is the root of this repository's GitHub Pages site. The apt repository keeps its paths (`dists/`, `pool/` and the key), so apt users see no change. One tag job builds both into the single `gh-pages` commit. The apt `index.html` becomes the guide's "Install from apt" page.
- **First flash.** A "Flash in the browser" page uses a pinned, vendored copy of ESP Web Tools (Chrome or Edge over Web Serial) to write the release's merged image. The esptool command is the fallback. Later updates go through the GUI's firmware update.
- **Publishing.** Only `vX.Y.Z` tags publish, so the guide always matches the latest release that apt installs. Download links point at that tag's release assets. CI for PRs and branches builds and checks the site and uploads it as an artifact, marked as an unreleased preview.
- **Screenshots.** The GUI's scenario tooling renders them against vdev in the Linux CI job, in light and dark, from a list the guide owns. The site job takes them as an artifact, so nothing large is committed. A local build without them shows placeholders. A tag build fails without them.
- **ROS.** Lyrical (Ubuntu 26.04, the Pi) is the main path, with a Lyrical/Jazzy tab wherever commands differ. Commands that differ by PC get Windows/Ubuntu tabs.
- **One source per fact.** The guide owns the user text. `docs/pi-setup.md` and the install, run and launch parts of the GUI, agent and bringup READMEs move into it (UG3) and leave a link behind. The repository docs keep build, development and contract content.

## UG1 Site, build and publishing

**Depends on** H4d.

Scope:
- `guide/` (content, templates, assets) and a build script in `tools/` with `build`, `serve` and `check`, writing to `build/guide/`. The Markdown features the content needs: OS and ROS tabs, callouts (note, warning, safety), copy buttons, figures with light and dark images, and generated tables. Pinned Python dependencies, installed by `setup_linux.sh` and documented for Windows.
- The `guide` validate check:
  - the build with no warnings;
  - every internal link and anchor resolves, and every page is in the navigation;
  - no external script, style or font URL;
  - alt text on every image;
  - the Pagefind index is built;
  - unit tests for the build script.
- The theme: `gen_theme.py` writes the CSS variables, and the `gen` check fails when they are stale.
- The web flasher: the vendored ESP Web Tools, with its licence in the notices, and a manifest generated at publish time from the release's `flash_args.json` and merged image (`tools/package_fw.py`). The page warns that a first flash erases the device.
- Screenshots: the guide's scenario list, rendered in the Linux GUI job and passed on as an artifact (`host/gui/README.md`, "Shell screenshots").
- Publishing: the tag job builds the apt repository and the guide into one tree and force-pushes `gh-pages` as before.
  - Guide paths never clash with `dists/`, `pool/` or the key file, and the tree has a `.nojekyll`.
  - `apt_repo.py verify` passes on the combined tree, and a check confirms the apt paths are unchanged.
  - PR CI uploads the combined preview, signed with the throwaway key.
- `guide/CLAUDE.md` (with an `AGENTS.md` symlink) holds the writing rules below and the build commands.
- Content: the home page with the path picker and buttons for the latest release, the downloads page (release assets, apt, checksums and the key fingerprint), and the flasher page. The other sections exist in the navigation as headings for UG2 and UG3.

Writing rules for `guide/CLAUDE.md`, refined in UG1's plan:
- **Voice.** Plain English, second person, short sentences.
- **Pages.** One task per page, with what you need at the top and what you will have at the end.
- **Steps.** Each numbered step says what you should see afterwards.
- **Terms.** Linked to the glossary the first time they appear on a page.
- **Commands.** Copyable, tried, and shown without a prompt or output mixed in.
- **Screenshots.** Taken from scenarios only, with alt text that says what matters in the picture.
- **Safety.** A callout before anything that can move a motor.

Checks: browser checks. Bare board: a first flash of the release image from the built site's flasher on Windows with Chrome or Edge, then a GUI connect. It erases the board, so ask the user first and restore "Board state" in [host-status.md](host-status.md) afterwards.

Review: independent for the workflow and publish changes. The PR states the publish job's permissions and that the signing secret is read only by the apt signing step. Self for the site.

## UG2 Start here, concepts and the PC bench journey

**Depends on** UG1.

Scope:
- Start here: what DriveLink is, how the pieces fit (a diagram of the ESP, the PC, the Pi, the agent and ROS), what you need (the reference build and the alternatives), and safety.
- Concepts, one screen each:
  - firmware and the serial link;
  - modules, instances and loops (the self-describing device);
  - actuators, encoders and controllers (open loop, velocity, position, PID);
  - the IMU;
  - arming, disarming, the watchdog, faults and safe mode;
  - saving and reverting;
  - the agent, leases and tokens;
  - ROS 2 in five minutes;
  - the glossary.
- The PC bench journey, with screenshots:
  1. Install the app (the Windows installer, or apt or `.deb` on Ubuntu).
  2. Flash.
  3. Connect, and take a tour of the GUI.
  4. Wire it, with the clickable diagrams.
  5. Build the layout (loops, instances, pins, labels, limits, save).
  6. The first spin (first-run procedure, the manual command, polarity).
  7. Monitor.
  8. Tune and identify.
  9. Add the IMU and calibrate it.
  10. Update the firmware.
- The reference build's parts list, confirmed with the user in the plan.

Checks: browser checks. Every GUI step is walked through against vdev, and the screenshots match it.

Review: self.

## UG3 Pi, ROS, reference and troubleshooting

**Depends on** UG2.

Scope:
- The Pi robot journey: Ubuntu on the Pi, the UART and the wiring to the ESP, installing the agent from apt, adding the device, the token and the `drivelink` group, the GUI over the LAN (discovery, the token, observer and lease), using several clients safely, upgrades and removal.
- The ROS 2 journey: installing ROS and the DriveLink ROS packages, the example diff-drive with teleop, describing your own robot (URDF/xacro, joints to labels, the IMU), the controllers YAML, diagnostics with `ack_fault` and `recover`, driving on the floor.
- Reference:
  - Generated at build time from `protocol/defs/`, so they cannot drift: the module types with their parameters, units and ranges, and the status, fault and diag codes.
  - Also: the GUI pages, the agent's configuration and service, the ROS hardware parameters and interfaces, the bringup launch arguments, packages and versions, ports and network (TCP 47620, mDNS), and limits.
- Troubleshooting and FAQ, by symptom:
  - no port, or a protocol mismatch;
  - ARM refused, one entry for each ARM check;
  - a motor or encoder running backwards;
  - fault codes;
  - the Pi UART;
  - an agent not found on the LAN, a token refused, or a lease held by another client;
  - the ROS component in `ERROR`, or DDS delivering nothing.
- About: licences and notices, release notes, the source, and how to report issues.
- The moves: `docs/pi-setup.md` and the user parts of the GUI, agent and bringup READMEs become links to the guide. Deferred rows in [host-status.md](host-status.md) that cite those docs point at the guide pages instead.

Checks: browser checks. The Pi and ROS commands run from the installed packages in the ROS containers (`ros-deb-install`) or in WSL against vdev behind the agent.

Review: self.
