.pragma library
// The imu.bno055 calibration panel's pure logic (H2f), shared by imu.mock:
// the F6 action blob, the PC calibration file, the import review, the sensor
// state read from the live signals, guidance, the orientation projection and
// the write → apply sequence. No QML, no device calls: tst_imu_calibration.qml
// tests every function. `.pragma library` also makes `store` one object per
// engine, so a panel that is reloaded (another type selected and back, a
// reconnect) finds its imported file and last read again.
//
// Blob (docs/protocol/imu.md): 42 bytes, little-endian,
// version u8 = 1 | has_sic u8 (0/1) | 20 × i16: accel x, y, z, radius;
// gyro x, y, z; mag x, y, z, radius; SIC 0..8 (row-major, Q14).

var BLOB_VERSION = 1
var BLOB_BYTES = 42
var FILE_FORMAT = "drivelink.imu-calibration"
var FILE_VERSION = 1
var FILE_SUFFIX = ".imucal.json"
// Action, signal and level names of the F6 contract.
var ACTION_READ = "calibration_read"
var ACTION_WRITE = "calibration_write"
var ACTION_APPLY = "calibration_apply"
var LEVELS = ["sys", "gyro", "accel", "mag"]
var LEVEL_TITLES = { sys: "System", gyro: "Gyroscope", accel: "Accelerometer", mag: "Magnetometer" }
var NO_SAMPLE = 4294967295
// After the apply reply, how long to wait for a new valid sample before
// saying none arrived (the sequence keeps watching).
var CONFIRM_MS = 3000

// ---- Blob ------------------------------------------------------------------

function i16ok(v) { return Number.isInteger(v) && v >= -32768 && v <= 32767 }
function radiusOk(v) { return Number.isInteger(v) && v >= 0 && v <= 32767 }

// The twenty values in blob order.
function blobValues(cal) {
    return [cal.accel.x, cal.accel.y, cal.accel.z, cal.accel.radius,
            cal.gyro.x, cal.gyro.y, cal.gyro.z,
            cal.mag.x, cal.mag.y, cal.mag.z, cal.mag.radius].concat(cal.sic)
}

// Why a calibration object is not encodable ("" when it is).
function calibrationProblem(cal) {
    if (!cal || typeof cal !== "object") return "No calibration values"
    if (typeof cal.hasSic !== "boolean") return "has_sic must be true or false"
    var groups = { accel: ["x", "y", "z", "radius"], gyro: ["x", "y", "z"], mag: ["x", "y", "z", "radius"] }
    for (var g in groups) {
        if (!cal[g] || typeof cal[g] !== "object") return g + " is missing"
        for (var i = 0; i < groups[g].length; ++i) {
            var f = groups[g][i], v = cal[g][f]
            if (v === undefined) return g + "." + f + " is missing"
            if (f === "radius" ? !radiusOk(v) : !i16ok(v))
                return g + "." + f + (f === "radius" ? " must be an integer 0 to 32767" : " must be an integer -32768 to 32767")
        }
    }
    if (!Array.isArray(cal.sic) || cal.sic.length !== 9) return "sic must list nine values"
    for (var k = 0; k < 9; ++k)
        if (!i16ok(cal.sic[k])) return "sic[" + k + "] must be an integer -32768 to 32767"
    return ""
}

function hex2(b) { return (b < 16 ? "0" : "") + b.toString(16) }

// The blob as hex text with spaces (what Connection.runAction parses).
function encodeBlob(cal) {
    var bytes = [BLOB_VERSION, cal.hasSic ? 1 : 0]
    var values = blobValues(cal)
    for (var i = 0; i < values.length; ++i) {
        var u = values[i] & 0xFFFF
        bytes.push(u & 0xFF, u >> 8)
    }
    return bytes.map(hex2).join(" ")
}

// {ok, cal} or {ok: false, error} from a blob's hex text.
function decodeBlob(hex) {
    var text = String(hex || "").replace(/\s+/g, "")
    if (!/^[0-9a-fA-F]*$/.test(text) || text.length % 2) return { ok: false, error: "The blob is not hex" }
    if (text.length !== 2 * BLOB_BYTES)
        return { ok: false, error: "The blob has " + text.length / 2 + " bytes, not " + BLOB_BYTES }
    var bytes = []
    for (var i = 0; i < text.length; i += 2) bytes.push(parseInt(text.substr(i, 2), 16))
    if (bytes[0] !== BLOB_VERSION) return { ok: false, error: "Blob version " + bytes[0] + " is not supported" }
    if (bytes[1] > 1) return { ok: false, error: "The blob's has_sic flag is " + bytes[1] }
    var v = []
    for (var k = 0; k < 20; ++k) {
        var u = bytes[2 + 2 * k] | (bytes[3 + 2 * k] << 8)
        v.push(u >= 0x8000 ? u - 0x10000 : u)
    }
    var cal = { hasSic: bytes[1] === 1,
                accel: { x: v[0], y: v[1], z: v[2], radius: v[3] },
                gyro: { x: v[4], y: v[5], z: v[6] },
                mag: { x: v[7], y: v[8], z: v[9], radius: v[10] },
                sic: v.slice(11, 20) }
    var problem = calibrationProblem(cal)
    return problem ? { ok: false, error: problem } : { ok: true, cal: cal }
}

// ---- The PC file -----------------------------------------------------------

function pad2(n) { return (n < 10 ? "0" : "") + n }
// "2026-11-18T09:05:03Z" (UTC, seconds).
function utcText(date) {
    return date.getUTCFullYear() + "-" + pad2(date.getUTCMonth() + 1) + "-" + pad2(date.getUTCDate()) + "T" +
           pad2(date.getUTCHours()) + ":" + pad2(date.getUTCMinutes()) + ":" + pad2(date.getUTCSeconds()) + "Z"
}

// The file text for a calibration read from the device. `read` holds the
// provenance captured when it was read: type, createdUtc, deviceId,
// deviceName, instance, firmware, gui, levels {sys, gyro, accel, mag},
// actualMode; `note` is the operator's free text (mounting, robot).
function buildFile(cal, read, note) {
    var file = {
        format: FILE_FORMAT,
        version: FILE_VERSION,
        type: read.type,
        created_utc: read.createdUtc,
        source: { device_id: read.deviceId || "", device_name: read.deviceName || "", instance: read.instance || "",
                  firmware: read.firmware || "", gui: read.gui || "" },
        levels: { sys: read.levels.sys, gyro: read.levels.gyro, accel: read.levels.accel, mag: read.levels.mag },
        actual_mode: read.actualMode,
        has_sic: cal.hasSic,
        accel: { x: cal.accel.x, y: cal.accel.y, z: cal.accel.z, radius: cal.accel.radius },
        gyro: { x: cal.gyro.x, y: cal.gyro.y, z: cal.gyro.z },
        mag: { x: cal.mag.x, y: cal.mag.y, z: cal.mag.z, radius: cal.mag.radius },
        sic: cal.sic.slice(),
        note: note || ""
    }
    return JSON.stringify(file, null, 2) + "\n"
}

// A suggested file name: <device>-<instance>-<yyyymmdd-hhmmss>.imucal.json.
function suggestedName(read) {
    var safe = function(s) { return String(s || "").replace(/[^A-Za-z0-9_.-]+/g, "_") }
    var t = String(read.createdUtc || "").replace(/[-:]/g, "").replace("T", "-").replace("Z", "")
    var parts = [safe(read.deviceName || read.deviceId), safe(read.instance), t].filter(function(p) { return p.length > 0 })
    return (parts.length ? parts.join("-") : "imu-calibration") + FILE_SUFFIX
}

function isObject(v) { return v !== null && typeof v === "object" && !Array.isArray(v) }

// Validates a file's text against the target instance. Refusals (nothing can
// be applied): not JSON, another format or version, a missing or
// out-of-range value, another module type. Warnings (shown in the review;
// Apply stays possible): another device, another instance label, levels
// below 3 when it was read, no provenance.
// target: {type, deviceId, instance}. Returns {ok: false, error} or
// {ok: true, cal, source: {type, createdUtc, deviceId, deviceName, instance,
// firmware, gui, levels, actualMode, note}, warnings: [text]}.
function parseFile(text, target) {
    var data
    try { data = JSON.parse(text) } catch (e) { return { ok: false, error: "The file is not JSON" } }
    if (!isObject(data) || data.format !== FILE_FORMAT)
        return { ok: false, error: "Not a DriveLink IMU calibration file" }
    if (data.version !== FILE_VERSION)
        return { ok: false, error: "File version " + data.version + " is not supported (this GUI reads version " + FILE_VERSION + ")" }
    if (typeof data.type !== "string" || !data.type.length) return { ok: false, error: "The file names no module type" }
    if (target && target.type && data.type !== target.type)
        return { ok: false, error: "The file is for " + data.type + "; this instance is " + target.type }
    var cal = { hasSic: data.has_sic, accel: data.accel, gyro: data.gyro, mag: data.mag, sic: data.sic }
    var problem = calibrationProblem(cal)
    if (problem) return { ok: false, error: "Incomplete or invalid calibration: " + problem }
    var src = isObject(data.source) ? data.source : {}
    var levels = isObject(data.levels) ? data.levels : {}
    var source = { type: data.type, createdUtc: String(data.created_utc || ""),
                   deviceId: String(src.device_id || ""), deviceName: String(src.device_name || ""),
                   instance: String(src.instance || ""), firmware: String(src.firmware || ""), gui: String(src.gui || ""),
                   levels: {}, actualMode: Number.isInteger(data.actual_mode) ? data.actual_mode : -1,
                   note: String(data.note || "") }
    var warnings = []
    var low = []
    for (var i = 0; i < LEVELS.length; ++i) {
        var l = levels[LEVELS[i]]
        source.levels[LEVELS[i]] = Number.isInteger(l) ? l : -1
        if (!Number.isInteger(l) || l < 3) low.push(LEVEL_TITLES[LEVELS[i]])
    }
    if (!source.deviceId) warnings.push("The file does not say which device it was read from")
    else if (target && target.deviceId && source.deviceId !== target.deviceId)
        warnings.push("Read from another device (" + (source.deviceName ? source.deviceName + " · " : "") + source.deviceId +
                      "). Offsets belong to one physical sensor and its mounting.")
    if (source.instance && target && target.instance && source.instance !== target.instance)
        warnings.push("Read from instance " + source.instance + ", not " + target.instance)
    if (low.length)
        warnings.push("Not fully calibrated when read (" + low.join(", ") + " below 3 / 3)")
    // Rebuilt in a fixed shape: unknown keys in the file are not carried.
    var clean = { hasSic: cal.hasSic, accel: { x: cal.accel.x, y: cal.accel.y, z: cal.accel.z, radius: cal.accel.radius },
                  gyro: { x: cal.gyro.x, y: cal.gyro.y, z: cal.gyro.z },
                  mag: { x: cal.mag.x, y: cal.mag.y, z: cal.mag.z, radius: cal.mag.radius }, sic: cal.sic.slice() }
    return { ok: true, cal: clean, source: source, warnings: warnings }
}

// ---- Live sensor state -----------------------------------------------------

function rad2deg(r) { return r * 180 / Math.PI }

// The panel's view of the selected IMU from Connection.live: state is
// "unavailable" (not running or no telemetry), "waiting" (no sample yet),
// "stale" (samples stopped, or the sensor reports invalid data: the last
// pose stays, marked), or "live". levels[name] is 0..3, or -1 when it cannot
// be shown (not live, or the mode has no calibration status). euler in
// degrees (BNO055 Windows convention, as the device reports it) and quat
// {w, x, y, z} are null before a sample.
function sensorState(live) {
    var s = { state: "unavailable", note: "", levels: { sys: -1, gyro: -1, accel: -1, mag: -1 },
              euler: null, quat: null, valid: false, sampleCount: -1, ageUs: -1, mode: -1, available: 0 }
    if (!live) return s
    s.note = live.note || ""
    var by = {}
    var rows = live.signals || []
    for (var i = 0; i < rows.length; ++i)
        if (rows[i].number !== undefined && rows[i].number !== null) by[rows[i].name] = rows[i].number
    var has = function(n) { return by[n] !== undefined }
    if (!has("sample_count")) { s.state = s.note.indexOf("Waiting") === 0 ? "waiting" : "unavailable"; return s }
    s.sampleCount = by.sample_count
    s.ageUs = has("sample_age_us") ? by.sample_age_us : -1
    s.mode = has("actual_mode") ? by.actual_mode : -1
    s.available = has("available") ? by.available : 0
    s.valid = by.valid === 1
    if (s.ageUs === NO_SAMPLE) { s.state = "waiting"; return s }
    if (has("roll") && has("pitch") && has("yaw"))
        s.euler = { roll: rad2deg(by.roll), pitch: rad2deg(by.pitch), yaw: rad2deg(by.yaw) }
    if (has("quaternion.w") && has("quaternion.x") && has("quaternion.y") && has("quaternion.z"))
        s.quat = { w: by["quaternion.w"], x: by["quaternion.x"], y: by["quaternion.y"], z: by["quaternion.z"] }
    s.state = live.telemetry === true && s.valid ? "live" : "stale"
    if (s.state === "live" && (s.available & 0x100)) {
        for (var k = 0; k < LEVELS.length; ++k) {
            var v = by["calibration_status." + LEVELS[k]]
            s.levels[LEVELS[k]] = v === undefined ? -1 : v
        }
    }
    return s
}

function fullyCalibrated(sensor) {
    for (var i = 0; i < LEVELS.length; ++i) if (sensor.levels[LEVELS[i]] !== 3) return false
    return true
}

// Why Read from IMU is disabled ("" when it is enabled). `blocked` is the
// backend's reason for the action (Connection.checkAction).
function readBlock(sensor, blocked) {
    if (blocked) return blocked
    if (sensor.state !== "live") return "Read needs live IMU data"
    if (!fullyCalibrated(sensor)) return "Read requires all four calibration levels at 3 / 3"
    return ""
}

function levelText(level) {
    if (level < 0) return "Unavailable"
    return level >= 3 ? "Calibrated" : "Calibrating"
}

// Guidance (Bosch BNO055 datasheet rev 1.8, 3.11.1–3.11.5): a row per sensor
// with its level and what to do; `done` when it is at 3.
function guidance(sensor) {
    var text = {
        gyro: "Keep the sensor still on a stable surface for a few seconds.",
        accel: "Hold the sensor steady in about six different orientations, a few seconds each, including along each axis.",
        mag: "Move the sensor through varied orientations (a figure of eight), away from magnetic interference.",
        sys: "Rises to 3 once the sensors are calibrated and fusion has settled."
    }
    var rows = []
    var order = ["gyro", "accel", "mag", "sys"]
    for (var i = 0; i < order.length; ++i) {
        var n = order[i], l = sensor.levels[n]
        rows.push({ name: n, title: LEVEL_TITLES[n], level: l, done: l === 3, text: text[n] })
    }
    return rows
}

// "2.4°", "—" before a value.
function degText(v) { return v === null || v === undefined || !isFinite(v) ? "—" : v.toFixed(1) + "°" }
function quatText(v) { return v === null || v === undefined || !isFinite(v) ? "—" : v.toFixed(4) }
function ageText(us) {
    if (us < 0 || us === NO_SAMPLE) return "—"
    return us < 1000 ? us + " µs" : us < 1e6 ? (us / 1000).toFixed(0) + " ms" : (us / 1e6).toFixed(1) + " s"
}

// ---- Orientation preview ---------------------------------------------------

// Rotates v by the unit quaternion q (sensor frame into the reference frame).
function rotate(q, v) {
    var n = Math.sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z)
    if (!(n > 1e-9)) return null
    var w = q.w / n, x = q.x / n, y = q.y / n, z = q.z / n
    // v' = v + 2w(u × v) + 2u × (u × v), u = (x, y, z)
    var cx = y * v[2] - z * v[1], cy = z * v[0] - x * v[2], cz = x * v[1] - y * v[0]
    var dx = y * cz - z * cy, dy = z * cx - x * cz, dz = x * cy - y * cx
    return [v[0] + 2 * (w * cx + dx), v[1] + 2 * (w * cy + dy), v[2] + 2 * (w * cz + dz)]
}

// A fixed isometric view of a reference-frame point: screen x to the right,
// screen y down, both within [-1, 1] for a unit vector; depth grows toward
// the viewer.
function isometric(p) {
    var c = Math.cos(Math.PI / 6), s = Math.sin(Math.PI / 6)
    return { x: (p[0] - p[1]) * c * 0.85, y: ((p[0] + p[1]) * s - p[2]) * 0.85, depth: p[0] + p[1] + p[2] }
}

// The sensor's X, Y and Z axes for the preview, farthest first:
// [{axis: "X", x, y, depth}], or [] for no or a degenerate quaternion.
function axes(q) {
    if (!q) return []
    var out = []
    var units = { X: [1, 0, 0], Y: [0, 1, 0], Z: [0, 0, 1] }
    for (var a in units) {
        var r = rotate(q, units[a])
        if (!r) return []
        var p = isometric(r)
        out.push({ axis: a, x: p.x, y: p.y, depth: p.depth })
    }
    out.sort(function(a, b) { return a.depth - b.depth })
    return out
}

// ---- One action step and the write → apply sequence --------------------------

// A step's outcome from Connection.actionResults / actionRunning:
// {state: "running" | "ok" | "rebooted" | "refused" | "uncertain", text}.
function stepOutcome(key, results, running, linked) {
    if (running === key) return { state: "running", text: "Running…" }
    var r = results ? results[key] : undefined
    if (!r) {
        return { state: "uncertain", text: linked
            ? "No outcome was recorded; it may have run. It was not retried."
            : "The link closed before the reply; it may have run. It was not retried." }
    }
    if (r.state === "ok") return r.rebooted ? { state: "rebooted", text: r.text || "Ran; the device has since rebooted" }
                                            : { state: "ok", text: r.text || "Done", blobHex: r.blobHex || "" }
    if (r.state === "uncertain") return { state: "uncertain", text: r.text || "No reply in time; it may have run" }
    if (r.state === "running") return { state: "running", text: "Running…" }
    return { state: "refused", text: r.text || "Refused" }
}

// Right after Connection.runAction(key): sent (running), refused locally, or
// not sent at all (an earlier outcome for the key is not this one's).
function sentOutcome(key, results, running) {
    if (running === key) return { state: "running", text: "Running…" }
    var r = results ? results[key] : undefined
    if (r && (r.state === "refused" || r.state === "invalid")) return { state: "refused", text: r.text || "Refused" }
    return { state: "refused", text: "Not sent" }
}

function copy(o) { return JSON.parse(JSON.stringify(o)) }

// A new Apply: write the blob, then apply it. Nothing is sent by this.
function newApply(nodeKey, writeKey, applyKey, blobHex, label) {
    return { nodeKey: nodeKey, writeKey: writeKey, applyKey: applyKey, blobHex: blobHex, label: label || "",
             phase: "write", note: "",
             steps: [{ name: "write", title: "Write to the device (staged in RAM)", state: "pending", text: "" },
                     { name: "apply", title: "Apply to the sensor", state: "pending", text: "Not sent" }],
             confirmCount: -1, confirmSeen: 0, confirmSince: 0 }
}

// Phases: write, apply (waiting for its reply), confirming (waiting for a new
// valid sample), and the ends: applied, noSample (applied; no new sample yet,
// still watching), failed, uncertain, ended (the device rebooted), abandoned.
function busy(seq) { return !!seq && (seq.phase === "write" || seq.phase === "apply" || seq.phase === "confirming") }

// After the panel called runAction for the current step.
function markSent(seq, outcome) {
    var s = copy(seq)
    var step = s.steps[s.phase === "write" ? 0 : 1]
    step.state = outcome.state; step.text = outcome.text
    if (outcome.state !== "running") { s.phase = "failed"; s.note = step.title + ": " + outcome.text }
    return s
}

// One observation of the backend. view: {results, running, linked, nodeKey,
// sensor (sensorState), now (ms)}. Returns {seq, send}: send is "apply" when
// the panel must now send calibration_apply (then call markSent).
function advance(seq, view) {
    if (!seq) return { seq: seq, send: "" }
    var s = copy(seq)
    if (busy(s) && view.nodeKey !== s.nodeKey) {
        // A running step keeps its own outcome; nothing further is sent.
        s.phase = "abandoned"; s.note = "Another instance was selected; nothing further was sent."
        return { seq: s, send: "" }
    }
    var stepFor = function(i, key) {
        var o = stepOutcome(key, view.results, view.running, view.linked)
        s.steps[i].state = o.state; s.steps[i].text = o.text
        return o
    }
    if (s.phase === "write" && s.steps[0].state === "running") {
        var w = stepFor(0, s.writeKey)
        if (w.state === "ok") {
            if (!view.linked) { s.phase = "uncertain"; s.note = "The link closed before Apply was sent; the staged calibration was not applied."; return { seq: s, send: "" } }
            s.phase = "apply"; s.steps[1].state = "pending"; s.steps[1].text = ""
            return { seq: s, send: "apply" }
        }
        if (w.state === "refused") { s.phase = "failed"; s.note = "Write refused: " + w.text + ". Nothing was applied." }
        else if (w.state === "uncertain") { s.phase = "uncertain"; s.note = "The write may have run; Apply was not sent." }
        else if (w.state === "rebooted") { s.phase = "ended"; s.note = "The device rebooted; Apply was not sent and the RAM stage is gone." }
        return { seq: s, send: "" }
    }
    if (s.phase === "apply" && s.steps[1].state === "running") {
        var a = stepFor(1, s.applyKey)
        if (a.state === "ok") {
            s.phase = "confirming"; s.confirmCount = view.sensor ? view.sensor.sampleCount : -1; s.confirmSeen = 0
            s.confirmSince = view.now
            s.note = "Applied; waiting for a new valid sample."
        } else if (a.state === "refused") { s.phase = "failed"; s.note = "Write succeeded (staged, Unsaved); Apply failed: " + a.text }
        else if (a.state === "uncertain") { s.phase = "uncertain"; s.note = "Write succeeded; Apply may have run. Check the levels before retrying." }
        else if (a.state === "rebooted") { s.phase = "ended"; s.note = "The device rebooted after Apply; the saved calibration is what runs now." }
        return { seq: s, send: "" }
    }
    if (s.phase === "confirming" || s.phase === "noSample") {
        if (!view.linked) {
            s.phase = "noSample"; s.note = "Applied; the link closed before a new sample arrived."
            return { seq: s, send: "" }
        }
        // Two new sample counts after the reply: a sample published before
        // the action paused the loop can still arrive after it, but not two.
        var sensor = view.sensor
        if (sensor && sensor.state === "live" && sensor.sampleCount !== s.confirmCount) {
            s.confirmCount = sensor.sampleCount; s.confirmSeen += 1
        }
        if (s.confirmSeen >= 2) {
            s.phase = "applied"; s.note = "Applied, and the sensor is sampling again. Save keeps it after a reboot."
        } else if (s.phase === "confirming" && view.now - s.confirmSince > CONFIRM_MS) {
            s.phase = "noSample"; s.note = "Applied, but no new valid sample has arrived yet."
        }
    }
    return { seq: s, send: "" }
}

// ---- Store (one per engine, see the header) ----------------------------------

var store = ({})
function storeKey(deviceId, nodeKey) { return String(deviceId || "") + "|" + String(nodeKey || "") }
function recall(key) { return store[key] ? copy(store[key]) : ({}) }
function remember(key, field, value) {
    if (!store[key]) store[key] = {}
    if (value === undefined || value === null) delete store[key][field]
    else store[key][field] = copy(value)
}
function forgetAll() { store = ({}) }
