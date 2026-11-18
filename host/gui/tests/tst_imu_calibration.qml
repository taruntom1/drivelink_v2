import QtQuick
import QtTest
import "../modules/imu.bno055/calibration.js" as Cal

// The imu.bno055 calibration panel's pure logic (H2f): the F6 blob, the PC
// file, the import review, the sensor state, guidance, the orientation
// projection and the write → apply sequence.
TestCase {
    name: "ImuCalibration"

    function sample() {
        return { hasSic: true, accel: { x: 1, y: -2, z: 3, radius: 1000 }, gyro: { x: -1, y: 0, z: 256 },
                 mag: { x: -32768, y: 32767, z: 0, radius: 0 }, sic: [16384, 0, 0, 0, 16384, 0, 0, 0, -16384] }
    }
    // docs/protocol/imu.md: version 1 | has_sic | 20 × i16 little-endian.
    readonly property string sampleHex: "01 01 01 00 fe ff 03 00 e8 03 ff ff 00 00 00 01 00 80 ff 7f 00 00 00 00 " +
                                        "00 40 00 00 00 00 00 00 00 40 00 00 00 00 00 00 00 c0"
    function provenance() {
        return { type: "imu.bno055", createdUtc: "2026-11-18T09:05:03Z", deviceId: "1cdbd446ff38", deviceName: "bench-rig",
                 instance: "imu", firmware: "2.0.0", gui: "drivelink-gui 0.1", levels: { sys: 3, gyro: 3, accel: 3, mag: 3 },
                 actualMode: 12 }
    }
    function target() { return { type: "imu.bno055", deviceId: "1cdbd446ff38", instance: "imu" } }

    function test_blob_encode_matches_the_wire_layout() {
        compare(Cal.encodeBlob(sample()), sampleHex)
        compare(sampleHex.split(" ").length, Cal.BLOB_BYTES)
        var noSic = sample(); noSic.hasSic = false
        compare(Cal.encodeBlob(noSic).substr(0, 5), "01 00")
    }

    function test_blob_round_trip_and_refusals() {
        var d = Cal.decodeBlob(sampleHex)
        verify(d.ok, d.error)
        compare(JSON.stringify(d.cal), JSON.stringify(sample()))
        // Without spaces and in upper case too.
        verify(Cal.decodeBlob(sampleHex.replace(/ /g, "").toUpperCase()).ok)
        compare(Cal.decodeBlob("01 01").error, "The blob has 2 bytes, not 42")
        compare(Cal.decodeBlob("zz").error, "The blob is not hex")
        compare(Cal.decodeBlob("02" + sampleHex.substr(2)).error, "Blob version 2 is not supported")
        compare(Cal.decodeBlob("01 02" + sampleHex.substr(5)).error, "The blob's has_sic flag is 2")
        // A negative radius (accel radius = 0xFFFF).
        var bad = sampleHex.split(" "); bad[8] = "ff"; bad[9] = "ff"
        verify(Cal.decodeBlob(bad.join(" ")).error.indexOf("accel.radius") === 0)
    }

    function test_calibration_problems() {
        compare(Cal.calibrationProblem(sample()), "")
        var c = sample(); c.accel.x = 40000
        compare(Cal.calibrationProblem(c), "accel.x must be an integer -32768 to 32767")
        c = sample(); c.mag.radius = -1
        compare(Cal.calibrationProblem(c), "mag.radius must be an integer 0 to 32767")
        c = sample(); c.gyro.z = 1.5
        compare(Cal.calibrationProblem(c), "gyro.z must be an integer -32768 to 32767")
        c = sample(); delete c.gyro.y
        compare(Cal.calibrationProblem(c), "gyro.y is missing")
        c = sample(); c.sic = c.sic.slice(0, 8)
        compare(Cal.calibrationProblem(c), "sic must list nine values")
        c = sample(); c.hasSic = 1
        compare(Cal.calibrationProblem(c), "has_sic must be true or false")
    }

    function test_file_round_trip() {
        var text = Cal.buildFile(sample(), provenance(), "left bracket, cable down")
        var data = JSON.parse(text)
        compare(data.format, "drivelink.imu-calibration")
        compare(data.version, 1)
        compare(data.type, "imu.bno055")
        compare(data.source.device_id, "1cdbd446ff38")
        compare(data.levels.mag, 3)
        compare(data.sic.length, 9)
        verify(data.blob === undefined)   // decoded fields only
        var parsed = Cal.parseFile(text, target())
        verify(parsed.ok, parsed.error)
        compare(JSON.stringify(parsed.cal), JSON.stringify(sample()))
        compare(parsed.warnings.length, 0)
        compare(parsed.source.note, "left bracket, cable down")
        compare(parsed.source.createdUtc, "2026-11-18T09:05:03Z")
        compare(Cal.encodeBlob(parsed.cal), sampleHex)
    }

    function test_file_refusals() {
        compare(Cal.parseFile("{", target()).error, "The file is not JSON")
        compare(Cal.parseFile("[]", target()).error, "Not a DriveLink IMU calibration file")
        var data = JSON.parse(Cal.buildFile(sample(), provenance(), ""))
        data.version = 2
        verify(Cal.parseFile(JSON.stringify(data), target()).error.indexOf("File version 2") === 0)
        data = JSON.parse(Cal.buildFile(sample(), provenance(), ""))
        data.type = "imu.mock"
        compare(Cal.parseFile(JSON.stringify(data), target()).error, "The file is for imu.mock; this instance is imu.bno055")
        data = JSON.parse(Cal.buildFile(sample(), provenance(), ""))
        delete data.mag
        compare(Cal.parseFile(JSON.stringify(data), target()).error, "Incomplete or invalid calibration: mag is missing")
        data = JSON.parse(Cal.buildFile(sample(), provenance(), ""))
        data.sic[4] = "16384"
        compare(Cal.parseFile(JSON.stringify(data), target()).error,
                "Incomplete or invalid calibration: sic[4] must be an integer -32768 to 32767")
    }

    function test_file_warnings() {
        var p = provenance(); p.deviceId = "aabbccddeeff"; p.instance = "imu_left"; p.levels.mag = 2
        var parsed = Cal.parseFile(Cal.buildFile(sample(), p, ""), target())
        verify(parsed.ok)
        compare(parsed.warnings.length, 3)
        verify(parsed.warnings[0].indexOf("Read from another device (bench-rig · aabbccddeeff)") === 0)
        compare(parsed.warnings[1], "Read from instance imu_left, not imu")
        compare(parsed.warnings[2], "Not fully calibrated when read (Magnetometer below 3 / 3)")
        p = provenance(); p.deviceId = ""
        parsed = Cal.parseFile(Cal.buildFile(sample(), p, ""), target())
        compare(parsed.warnings, ["The file does not say which device it was read from"])
        // Unknown keys are not carried into the calibration.
        var data = JSON.parse(Cal.buildFile(sample(), provenance(), "")); data.accel.extra = 5
        compare(Cal.parseFile(JSON.stringify(data), target()).cal.accel.extra, undefined)
    }

    function test_suggested_name() {
        compare(Cal.suggestedName(provenance()), "bench-rig-imu-20261118-090503.imucal.json")
        var p = provenance(); p.deviceName = ""; p.instance = "a/b"
        compare(Cal.suggestedName(p), "1cdbd446ff38-a_b-20261118-090503.imucal.json")
        compare(Cal.utcText(new Date(Date.UTC(2026, 0, 2, 3, 4, 5))), "2026-01-02T03:04:05Z")
    }

    function live(values, telemetry, note) {
        var rows = []
        for (var n in values) rows.push({ name: n, number: values[n], value: String(values[n]) })
        return { telemetry: telemetry, note: note || "", signals: rows }
    }
    function fullSample(levels) {
        return { roll: Math.PI / 2, pitch: 0, yaw: -Math.PI / 4, "quaternion.w": 1, "quaternion.x": 0, "quaternion.y": 0,
                 "quaternion.z": 0, valid: 1, sample_count: 42, sample_age_us: 8000, actual_mode: 12, available: 0x1FF,
                 "calibration_status.sys": levels[0], "calibration_status.gyro": levels[1],
                 "calibration_status.accel": levels[2], "calibration_status.mag": levels[3] }
    }

    function test_sensor_state() {
        compare(Cal.sensorState(null).state, "unavailable")
        compare(Cal.sensorState({ note: "Waiting for the first sample", signals: [] }).state, "waiting")
        compare(Cal.sensorState({ note: "Not running: failed", signals: [{ name: "valid", number: null }] }).state, "unavailable")
        var s = Cal.sensorState(live(fullSample([3, 3, 2, 1]), true))
        compare(s.state, "live")
        compare(s.levels.sys, 3); compare(s.levels.mag, 1)
        fuzzyCompare(s.euler.roll, 90, 1e-9); fuzzyCompare(s.euler.yaw, -45, 1e-9)
        compare(s.quat.w, 1)
        compare(s.sampleCount, 42); compare(s.mode, 12)
        // No new samples: the pose stays, levels are not shown.
        s = Cal.sensorState(live(fullSample([3, 3, 3, 3]), false))
        compare(s.state, "stale"); compare(s.levels.sys, -1); verify(s.euler !== null)
        // The sensor reports invalid data (CONFIG, a failed read).
        var v = fullSample([3, 3, 3, 3]); v.valid = 0
        compare(Cal.sensorState(live(v, true)).state, "stale")
        // No sample yet.
        v = fullSample([0, 0, 0, 0]); v.sample_age_us = Cal.NO_SAMPLE
        s = Cal.sensorState(live(v, true))
        compare(s.state, "waiting"); compare(s.euler, null)
        // A mode without calibration status (bit 8).
        v = fullSample([3, 3, 3, 3]); v.available = 0x3F
        compare(Cal.sensorState(live(v, true)).levels.gyro, -1)
    }

    function test_read_block_and_guidance() {
        var full = Cal.sensorState(live(fullSample([3, 3, 3, 3]), true))
        compare(Cal.readBlock(full, ""), "")
        compare(Cal.readBlock(full, "Not allowed"), "Not allowed")
        compare(Cal.readBlock(Cal.sensorState(live(fullSample([3, 3, 3, 2]), true)), ""),
                "Read requires all four calibration levels at 3 / 3")
        compare(Cal.readBlock(Cal.sensorState(live(fullSample([3, 3, 3, 3]), false)), ""), "Read needs live IMU data")
        var rows = Cal.guidance(Cal.sensorState(live(fullSample([1, 3, 2, 0]), true)))
        compare(rows.map(function(r) { return r.name }), ["gyro", "accel", "mag", "sys"])
        verify(rows[0].done); verify(!rows[1].done); compare(rows[3].level, 1)
        compare(Cal.levelText(-1), "Unavailable"); compare(Cal.levelText(2), "Calibrating"); compare(Cal.levelText(3), "Calibrated")
        compare(Cal.degText(2.44), "2.4°"); compare(Cal.degText(null), "—")
        compare(Cal.quatText(0.73123), "0.7312")
        compare(Cal.ageText(8000), "8 ms"); compare(Cal.ageText(Cal.NO_SAMPLE), "—"); compare(Cal.ageText(2500000), "2.5 s")
    }

    function test_orientation_axes() {
        compare(Cal.axes(null), [])
        compare(Cal.axes({ w: 0, x: 0, y: 0, z: 0 }), [])
        var id = Cal.axes({ w: 1, x: 0, y: 0, z: 0 })
        compare(id.length, 3)
        var by = {}
        for (var i = 0; i < 3; ++i) by[id[i].axis] = id[i]
        // Isometric: X down-right, Y down-left, Z straight up.
        verify(by.X.x > 0 && by.X.y > 0)
        verify(by.Y.x < 0 && by.Y.y > 0)
        fuzzyCompare(by.Z.x, 0, 1e-9); verify(by.Z.y < 0)
        // Farthest first.
        verify(id[0].depth <= id[1].depth && id[1].depth <= id[2].depth)
        // 90° about Z (not normalised): X takes Y's place.
        var r = Cal.axes({ w: 2, x: 0, y: 0, z: 2 })
        for (i = 0; i < 3; ++i) if (r[i].axis === "X") { fuzzyCompare(r[i].x, by.Y.x, 1e-9); fuzzyCompare(r[i].y, by.Y.y, 1e-9) }
        var v = Cal.rotate({ w: Math.SQRT1_2, x: Math.SQRT1_2, y: 0, z: 0 }, [0, 1, 0])
        fuzzyCompare(v[2], 1, 1e-9)
    }

    // ---- The sequence --------------------------------------------------------

    function view(results, running, extra) {
        var v = { results: results, running: running, linked: true, nodeKey: "114:0",
                  sensor: { state: "live", sampleCount: 10 }, now: 0 }
        for (var k in extra) v[k] = extra[k]
        return v
    }
    function started() {
        var seq = Cal.newApply("114:0", "114:0:771", "114:0:772", sampleHex, "a.imucal.json")
        return Cal.markSent(seq, Cal.sentOutcome("114:0:771", { "114:0:771": { state: "running" } }, "114:0:771"))
    }

    function test_sequence_happy_path() {
        var seq = started()
        compare(seq.phase, "write"); compare(seq.steps[0].state, "running"); verify(Cal.busy(seq))
        // Still running: nothing changes.
        var step = Cal.advance(seq, view({ "114:0:771": { state: "running" } }, "114:0:771"))
        compare(step.send, ""); compare(step.seq.phase, "write")
        // Write done: send apply.
        step = Cal.advance(seq, view({ "114:0:771": { state: "ok", text: "Done" } }, ""))
        compare(step.send, "apply"); compare(step.seq.phase, "apply"); compare(step.seq.steps[0].state, "ok")
        seq = Cal.markSent(step.seq, Cal.sentOutcome("114:0:772", { "114:0:772": { state: "running" } }, "114:0:772"))
        compare(seq.steps[1].state, "running")
        // Apply done: wait for two new samples.
        var results = { "114:0:771": { state: "ok" }, "114:0:772": { state: "ok" } }
        step = Cal.advance(seq, view(results, "", { now: 100 }))
        compare(step.seq.phase, "confirming"); compare(step.seq.confirmCount, 10)
        seq = step.seq
        seq = Cal.advance(seq, view(results, "", { now: 200, sensor: { state: "live", sampleCount: 20 } })).seq
        compare(seq.phase, "confirming")   // one new count could predate the pause
        seq = Cal.advance(seq, view(results, "", { now: 300, sensor: { state: "stale", sampleCount: 30 } })).seq
        compare(seq.phase, "confirming")   // stale samples do not count
        seq = Cal.advance(seq, view(results, "", { now: 400, sensor: { state: "live", sampleCount: 30 } })).seq
        compare(seq.phase, "applied"); verify(!Cal.busy(seq))
    }

    function test_sequence_no_sample_then_applied() {
        var seq = started()
        seq = Cal.advance(seq, view({ "114:0:771": { state: "ok" } }, "")).seq
        seq = Cal.markSent(seq, { state: "running", text: "" })
        var results = { "114:0:771": { state: "ok" }, "114:0:772": { state: "ok" } }
        seq = Cal.advance(seq, view(results, "", { now: 0 })).seq
        seq = Cal.advance(seq, view(results, "", { now: Cal.CONFIRM_MS + 1 })).seq
        compare(seq.phase, "noSample"); verify(!Cal.busy(seq))
        seq = Cal.advance(seq, view(results, "", { sensor: { state: "live", sampleCount: 11 } })).seq
        seq = Cal.advance(seq, view(results, "", { sensor: { state: "live", sampleCount: 12 } })).seq
        compare(seq.phase, "applied")
    }

    function test_sequence_write_refused_or_uncertain_sends_no_apply() {
        var step = Cal.advance(started(), view({ "114:0:771": { state: "refused", text: "Busy" } }, ""))
        compare(step.send, ""); compare(step.seq.phase, "failed")
        compare(step.seq.note, "Write refused: Busy. Nothing was applied.")
        compare(step.seq.steps[1].state, "pending")
        step = Cal.advance(started(), view({ "114:0:771": { state: "uncertain", text: "No reply" } }, ""))
        compare(step.send, ""); compare(step.seq.phase, "uncertain")
        // The link closed: the result is gone.
        step = Cal.advance(started(), view({}, "", { linked: false }))
        compare(step.send, ""); compare(step.seq.phase, "uncertain")
        compare(step.seq.steps[0].text, "The link closed before the reply; it may have run. It was not retried.")
        // A reply after a Boot.
        step = Cal.advance(started(), view({ "114:0:771": { state: "ok", rebooted: true } }, ""))
        compare(step.send, ""); compare(step.seq.phase, "ended")
        // Write ok but the link is gone: no apply.
        step = Cal.advance(started(), view({ "114:0:771": { state: "ok" } }, "", { linked: false }))
        compare(step.send, ""); compare(step.seq.phase, "uncertain")
    }

    function test_sequence_apply_failures_keep_the_write() {
        var seq = Cal.advance(started(), view({ "114:0:771": { state: "ok" } }, "")).seq
        seq = Cal.markSent(seq, { state: "running", text: "" })
        var step = Cal.advance(seq, view({ "114:0:771": { state: "ok" }, "114:0:772": { state: "refused", text: "Fault" } }, ""))
        compare(step.seq.phase, "failed"); compare(step.seq.steps[0].state, "ok"); compare(step.seq.steps[1].state, "refused")
        verify(step.seq.note.indexOf("Write succeeded") === 0)
        step = Cal.advance(seq, view({ "114:0:771": { state: "ok" }, "114:0:772": { state: "uncertain" } }, ""))
        compare(step.seq.phase, "uncertain")
    }

    function test_sequence_not_sent_and_reselect() {
        // runAction did not send (no running key, only an old outcome).
        var seq = Cal.newApply("114:0", "114:0:771", "114:0:772", sampleHex, "")
        seq = Cal.markSent(seq, Cal.sentOutcome("114:0:771", { "114:0:771": { state: "ok" } }, ""))
        compare(seq.phase, "failed"); compare(seq.steps[0].text, "Not sent")
        seq = Cal.newApply("114:0", "114:0:771", "114:0:772", sampleHex, "")
        seq = Cal.markSent(seq, Cal.sentOutcome("114:0:771", { "114:0:771": { state: "refused", text: "Paused actions are blocked" } }, ""))
        compare(seq.phase, "failed"); compare(seq.steps[0].text, "Paused actions are blocked")
        // Another instance selected mid-sequence: abandoned, nothing sent.
        var step = Cal.advance(started(), view({ "114:0:771": { state: "ok" } }, "", { nodeKey: "114:1" }))
        compare(step.send, ""); compare(step.seq.phase, "abandoned")
        compare(Cal.advance(null, view({}, "")).send, "")
    }

    function test_store() {
        Cal.forgetAll()
        var key = Cal.storeKey("dev", "114:0")
        compare(Cal.recall(key), {})
        Cal.remember(key, "imported", { fileName: "a" })
        var got = Cal.recall(key); got.imported.fileName = "changed"
        compare(Cal.recall(key).imported.fileName, "a")   // copies, not references
        Cal.remember(key, "imported", null)
        compare(Cal.recall(key).imported, undefined)
        compare(Cal.recall(Cal.storeKey("other", "114:0")), {})
        Cal.forgetAll()
    }
}
