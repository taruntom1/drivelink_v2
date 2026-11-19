# drivelink-cli

Since core v9, `info` includes `device_id` (12 lowercase hex digits) and `name`.
`name [TEXT]` reads or renames; `name ""` clears. Names allow 0..16 UTF-8 bytes
without C0/C1 controls, also while armed. SAVE persists; REVERT restores.

Generic CLI over libdrivelink. `--help` lists commands and arguments.
Options precede the command; supply `--endpoint tcp://127.0.0.1:8765` or
`--endpoint serial://COM3` (921600 baud by default).

```sh
drivelink-cli --endpoint tcp://127.0.0.1:8765 --json types
drivelink-cli --endpoint tcp://127.0.0.1:8765 add 112 motor
drivelink-cli --endpoint tcp://127.0.0.1:8765 set 112:0:1=0.2
drivelink-cli --endpoint tcp://127.0.0.1:8765 save
drivelink-cli --endpoint tcp://127.0.0.1:8765 reboot
drivelink-cli --endpoint tcp://127.0.0.1:8765 action 112:0:768 0.5
```

The numeric mock IDs above are examples. `types`, `items TYPE`, `enums TYPE ITEM`
and `codes` discover definitions. `instances [pending]`, `add`, `remove`, `label`
and `discard` edit the layout. An add is pending until SAVE and reboot.
`get REF...`, `get-pending REF...` and `set REF=VALUE...` support batches, with
Refs written as `type:instance:item`. IDs accept decimal or `0x` hexadecimal.
Param values are parsed from device descriptors (booleans, integers, float or
enum names). GET/SET show raw slot bits and typed values. A pending parameter
write is a successful command with `pending_reboot: true`.

Actions use `action REF [ARG...] [--blob HEX]`, with descriptor-defined scalar
args/results and up to 512 blob bytes. `status [COUNT]` and `events [COUNT]`
watch bounded event queues. `diag-info`, `diag-entries`, `diag-clear`, `save`,
`revert`, `factory-reset`, `reboot` and `exit-safe-mode` use the current firmware
services. `bench-crash` is a test hook rejected by normal firmware and normal
vdev. Every connection checks core v10 before any requested mutation.

F4c source parameters have `is_source: true` in item JSON (flag 0x80).
Configure them through `set`, using the numeric packed reference
`type << 24 | instance << 16 | item_id`, then `save` and `reboot`.
For example, `set 115:0:2=1912602880` connects fusion.mock instance 0's
IMU source to imu.mock instance 0's yaw (type 114, signal 256).
Discover and subscribe to `<name>.value` and `<name>.age_us` on the consumer
loop normally. Age 4294967295 means no valid sample; zero disconnects a
source parameter. Sources stay sampled while disarmed.

Default output is readable key/value text. With `--json`, stdout holds only
`{"ok":true,"data":...}` or `{"ok":false,"error":...,"exit_code":...,"result":...,"detail":...}`.
Watch commands produce one JSON envelope per line (NDJSON). Exit codes: 0
success, 1 internal failure, 2 usage/value error, 3 connection, 4 timeout,
5 device rejection, 6 malformed/incompatible protocol. Help does not open a port.
Writes never retry; a write timeout has an unknown outcome.

The CLI consumes typed library results and owns JSON formatting in
`src/format.cpp`. CTest's `host-cli-golden` compares 60 readable/JSON outputs
against vdev; `host-cli-replay` compares 12 deterministic event, action,
subscription, capture and CSV outputs. Fixtures were captured from the original
2793125 CLI before migration, with only documented dynamic fields normalized.
Linux replay also interrupts an in-flight testgen START and requires DISARM
plus the original null-status output when no STATUS has been read. Run these
through the normal host validation checks; do not re-record fixtures to make
a changed implementation pass.

Loops are built-in type 5. For example, `add 5 fusion` allocates its loop id;
`set 5:2:1=250` changes its rate, and `set 5:2:2=true` stages its real-time flag.
Use the returned id rather than assuming 2. Add modules with `add TYPE LABEL LOOP`.
SAVE/reboot applies additions, removals and reboot-required settings. `info`
reports running loop ids/flags; status overruns are objects with loop_id and
overruns. Safe mode reports no running loops, while instances lists offline config.

Groups: `groups` lists slots and limits. `sub-config GROUP LOOP DIVIDER REF...`
and `cmd-config GROUP LOOP REF...` return the new epoch/channel. Use
`sub-enable GROUP true|false`, `sub-clear GROUP`, or `cmd-clear GROUP` to change
resources. `sub-stream GROUP [COUNT]` prints typed samples and filters stale
epochs. `cmd-send GROUP COUNTER [VALUE...]` parses configured input types; no
values means keepalive. It drains its frame with an ordered GET_INFO before
closing; the stream itself has no acknowledgement. Commands received while disarmed are ignored, so
sending command values cannot change device inputs. NaN/Inf are rejected.


F4b adds `arm`, `disarm` (MSG_TYPE 0x03), and `ack-fault`. ARM failures include
the device result/detail; acknowledging a fault never clears its active cause.
`cmd-run GROUP PERIOD_MS COUNT [VALUE...]` validates typed values, arms, sends
repeated frames and disarms when done or on SIGINT/error. The period must be
less than half watchdog_ms. With no values it sends prefix-only keepalives.
`keepalive GROUP PERIOD_MS COUNT` sends those frames without arming or taking
ownership of an existing arm session. Both workflows stop on connection errors;
command streams have no acknowledgement, so status/diag supervision is separate.
CLI values remain finite and explicit. The library's managed command API can
map NaN through consumer-supplied finite holds; these CLI commands keep their
existing manual command/keepalive behavior.

`sub-csv GROUP COUNT` writes timestamp_us, counter and descriptor-typed values
to stdout as CSV. Redirect to a file; omit --json. `sub-stream` retains NDJSON.
Read group capacities through `get 0:0:18 0:0:19 0:0:20`; GROUP LIST has no
capacity prefix, while the Client::groups JSON convenience fields remain.
`bench-stall LOOP DURATION_MS` is refused by normal firmware and vdev. A
bench-enabled firmware / vdev --allow-crash schedules a deliberate loop stall.

Capture (F7a): `capture-config LOOP DIVIDER SAMPLES PRE_TRIGGER TRIGGER REF...`
with TRIGGER `manual`, `testgen_start`, `value_change REF` or `threshold REF rising|falling VALUE`
(the value is parsed with the trigger item's descriptor). `capture-arm`,
`capture-trigger`, `capture-abort` and `capture-status` map to the ops.
`capture-wait [TIMEOUT_MS]` returns once the capture is done, with the
CaptureDone event when it arrived during the call. `capture-read` pages every
sample of a done capture and adds typed rows; `capture-read --csv` prints
`index,time_us,REF...`, where time is relative to the trigger sample
(`(index - trigger_index) × period_us × divider`). `events` decodes 0xC1.

## Through an agent

`drivelink-agent` (Linux; [agent protocol](../../docs/protocol/agent.md)) owns
the device ports and shares each device with several clients. Point the CLI at
the agent's `tcp://` endpoint and name the device:

```sh
export DRIVELINK_AGENT_TOKEN=...            # or --token TOKEN
drivelink-cli --endpoint tcp://127.0.0.1:47620 agent-devices
drivelink-cli --endpoint tcp://127.0.0.1:47620 --device base info
drivelink-cli --endpoint tcp://127.0.0.1:47620 --device 02444c000001 --json get 0:0:4
```

`--device` takes an alias or a 12-hex device id; with it every command above
works through the agent. It needs a `tcp://` endpoint and a token (`--token`,
else the `DRIVELINK_AGENT_TOKEN` environment variable); otherwise it is a usage
error (exit 2). Without `--device` the token is ignored. `--client-name NAME`
(default `drivelink-cli`) is what the agent shows other clients.

`agent-devices` (no `--device`) lists the agent's devices, one line each:
`ALIAS online|offline ID NAME ENDPOINT clients=N`, followed by
` holder=CLIENT_ID:NAME` when a client holds the device's lease (`-` for an
unknown ID or an empty name). With `--json` the envelope's `data.devices` is an
array of `{alias, online, device_id, name, endpoint, clients, holder_id,
holder_name}`; `device_id` is `null` until the agent has read it, `holder_id` is
0 and `holder_name` empty when nobody holds the lease. Agent refusals (bad token, unknown or offline
device, a missing lease) are device rejections: exit 5, with the agent's
`result` and `detail`. An unreachable agent is exit 3 and a timeout 4, as
for a direct endpoint.

`--lease` (a global option before the command, needs `--device`) takes the
device's control lease before the command runs, preempting the current holder
(who is disarmed). Arming, command groups and the exclusive operations (SAVE,
REVERT, FACTORY_RESET, REBOOT, layout edits, capture, test signals, OTA) need
the lease, or a device nobody holds that is not armed; otherwise they are
refused `Busy/LeaseRequired` (exit 5, detail 58). The lease ends when the CLI
process exits: the agent then disarms the device and clears the process's
command groups, so `--lease arm` disarms again at once; use a long-lived
client to keep an armed device. A refused take is a device rejection (exit 5).
`--lease` does not apply to `ota-upload` (it opens its own session), so OTA
through an agent works only while nobody holds the device's lease.

`agent-clients` (needs `--device`) lists the clients connected to the device,
one line each, `CLIENT_ID NAME`, with ` [holder]` on the lease holder. With
`--json`, `data.clients` is an array of `{client_id, name, holder}`.

Each CLI process is one agent client, and the agent clears a client's group
slots when it disconnects. `sub-config` therefore does not outlive its process:
`sub-stream` and `cmd-send` in a later process find the group unconfigured.
Use a long-lived client (the GUI, a script on the agent protocol) for groups.

## Test signals (F7b)

`testgen-start LOOP REF WAVE OFFSET AMPLITUDE F0 F1 DURATION_MS REPEAT`
requires an already armed device and an independently fed command watchdog.
WAVE is `step`, `square`, `ramp`, `sine`, `chirp_linear` or `chirp_log`.
Unused frequencies must be zero. Duration is per repetition; repeat 0 means
until STOP/DISARM. The target must be a running f32 input flagged Testgen.
Values are clamped to its descriptor limits.

`testgen-stop LOOP` is idempotent. STOP/completion holds the last generated
value until a command received after termination; ignored commands and
keepalives cannot restore an older value. DISARM resets inputs as usual.
`testgen-status LOOP` reports the run and configuration. `testgen-wait LOOP
[TIMEOUT_MS]` correlates loop/run, polls STATUS, and returns an optional matching
TestgenDone event. It does not feed the watchdog.

`testgen-run GROUP PERIOD_MS TIMEOUT_MS LOOP REF WAVE OFFSET AMPLITUDE F0 F1
DURATION_MS REPEAT` arms, supplies prefix-only command-group keepalives, starts
the test, waits within the supplied timeout, and disarms on completion,
interruption or failure. GROUP must already be configured; PERIOD_MS must be
less than half watchdog_ms. Both this workflow and `cmd-run` attempt DISARM
even when ARM may have applied but its reply was lost. Mutations are never
retried. See `docs/protocol/core.md` for exact waveform phases and wire fields.

## Time measurements (F8)

`time-ping` obtains one four-timestamp sample. `time-sync [DURATION_S]` defaults
to 180 s (1–3600), using a 100 ms warmup cadence for five seconds and 1 s
thereafter. Both support readable/JSON output. The measurement command reports
raw samples, bounded-estimator snapshots, held-out predictions, timeouts and
dropped event counts. JSON samples additionally expose `mapping_valid`,
`mapping_generation`, `mapped_host_us` and `mapped_uncertainty_us` from the
production Session fit; readable output is unchanged. Invalid mappings use null
host/uncertainty fields. Interrupt stops sampling; no firmware clock is set.
CLI sessions disable background periodic pings and retain this explicit cadence.
Through an agent (`--device`) both commands ping the agent: the `esp_*` fields
are the agent's monotonic times and the estimator fits the agent's clock;
`mapping_valid` says whether the Session's composed device clock (the agent's
ClockModel with this fit) is valid, and the mapped host fields stay null.
Use `host/test/time_smoke.py --cli <cli> --endpoint serial://COM3 --duration 180`
and repeat with `--traffic` for the bare-board stability check. See the
[time contract](../../docs/protocol/time.md) for the accuracy limits.
