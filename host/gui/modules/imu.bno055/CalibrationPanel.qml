import QtQuick
import QtCore
import QtQml.Models
import QtQuick.Dialogs
import DriveLink.Style
import "calibration.js" as Cal

// The imu.bno055 calibration panel (G7, H2f); imu.mock loads it too (the same
// F6 contract). Layout gives it `backend` (the Connection), `nodeKey` (the
// selected instance, "type:instance") and `dialogParent`; `dialogOpen` dims
// the page while its Apply review is open.
//
// This file holds the state and every device call; the cards only draw it
// and call the functions below. Contract for the cards (all read-only):
//   sensor        Cal.sensorState(backend.live): state, note, levels, euler,
//                 quat, valid, sampleCount, ageUs, mode
//   loopText      "sensor loop" (the loop the instance runs in)
//   readReason    why Read from IMU is disabled ("" when enabled)
//   lastRead      {cal, read (provenance), at} or null: the last Read
//   imported      {fileName, path, cal, source, warnings} or null
//   importError   the last refused import's reason ("" when none)
//   operation     {title, state: ok | running | refused | uncertain | info,
//                 text, steps: [{title, state, text}]} or null: Last operation
//   applyReason   why Apply is disabled ("" when enabled)
//   applying      an Apply or Read is in flight (buttons wait)
//   target        {deviceId, deviceName, instance, type}
// Functions: openImport(), openExport(), read(), requestApply() (opens the
// review), confirmApply(), cancelApply(), discardImport().
Item {
    id: panel
    objectName: "calibrationPanel"

    property var backend
    property string nodeKey
    property Item dialogParent
    readonly property string title: qsTr("Calibration")
    readonly property bool dialogOpen: applyDialog.visible

    readonly property var sensor: Cal.sensorState(backend ? backend.live : null)
    readonly property var nodeInfo: backend && backend.layout.node ? backend.layout.node : ({})
    readonly property var target: ({deviceId: backend ? backend.deviceId : "", deviceName: backend ? backend.deviceName : "",
                                     instance: nodeInfo.label || "", type: nodeInfo.typeName || ""})
    // The last device id seen: it reads empty while the link is lost, and
    // the panel's memory must not switch away mid-operation.
    property string deviceKey: ""
    Binding on deviceKey { when: panel.target.deviceId.length > 0; value: panel.target.deviceId; restoreMode: Binding.RestoreNone }
    readonly property string storeKey: deviceKey.length > 0 && nodeKey.length > 0 ? Cal.storeKey(deviceKey, nodeKey) : ""
    readonly property string loopText: nodeInfo.loopName ? qsTr("%1 loop").arg(nodeInfo.loopName) : ""

    // The instance's calibration actions by name (from the descriptors).
    property var actionKeys: ({})
    Instantiator {
        model: panel.backend ? panel.backend.actions : null
        delegate: QtObject {
            required property string key
            required property string name
        }
        onObjectAdded: function(index, object) { panel.rebuildKeys() }
        onObjectRemoved: function(index, object) { panel.rebuildKeys() }
        id: actionList
    }
    function rebuildKeys() {
        var keys = {}
        for (var i = 0; i < actionList.count; ++i) {
            var o = actionList.objectAt(i)
            if (o) keys[o.name] = o.key
        }
        actionKeys = keys
    }
    function blockOf(name, blob) {
        var key = actionKeys[name]
        if (!key || !backend) return qsTr("The instance has no %1 action").arg(name)
        // Re-evaluated with every backend change (configuration, arm, OTA, an action).
        void backend.actionRunning
        void backend.armState
        void backend.configAction
        var check = backend.checkAction(key, [], blob || "")
        return check.blocked ? check.problem : ""
    }

    property var lastRead: null
    property var imported: null
    property string importError: ""
    property var operation: null
    property var sequence: null      // the Apply in flight or last finished
    property var readStep: null      // {key, read (provenance)} while Read runs
    readonly property bool applying: Cal.busy(sequence) || readStep !== null
    readonly property string readReason: applying ? qsTr("Wait for the current operation")
                                                  : Cal.readBlock(sensor, blockOf(Cal.ACTION_READ))
    readonly property string applyReason: {
        if (applying) return qsTr("Wait for the current operation")
        if (!imported) return qsTr("Import a file first")
        var write = blockOf(Cal.ACTION_WRITE, Cal.encodeBlob(imported.cal))
        if (write) return write
        return blockOf(Cal.ACTION_APPLY)
    }

    // ---- Per-instance memory (survives reloads and reconnects) -------------
    // The key whose memory the panel shows: an operation still running when
    // the instance changes (or the panel unloads: the link dropped, another
    // type selected) is recorded there as interrupted, and nothing further
    // is sent for it.
    property string loadedKey: ""
    onStoreKeyChanged: {
        if (!storeKey) return
        interrupt()
        loadedKey = storeKey
        var saved = Cal.recall(storeKey)
        lastRead = saved.lastRead || null
        imported = saved.imported || null
        operation = saved.operation || null
        importError = ""
    }
    function keep(field, value) { if (loadedKey) Cal.remember(loadedKey, field, value) }
    function interrupt() {
        if (!Cal.busy(sequence) && readStep === null) return
        keep("operation", {title: operation ? operation.title : qsTr("Operation"), state: "uncertain",
                           text: qsTr("Interrupted: the panel closed while a step was running. It may have run; nothing further was sent."),
                           steps: operation ? operation.steps : []})
        sequence = null
        readStep = null
    }
    Component.onDestruction: interrupt()
    function setOperation(op) { operation = op; keep("operation", op) }

    // ---- Import -----------------------------------------------------------
    FileDialog {
        id: importDialog
        title: qsTr("Import an IMU calibration file")
        fileMode: FileDialog.OpenFile
        modality: Qt.NonModal
        nameFilters: [qsTr("IMU calibration (*%1 *.json)").arg(Cal.FILE_SUFFIX), qsTr("All files (*)")]
        onAccepted: panel.importFile(selectedFile.toString())
    }
    function openImport() { importDialog.open() }
    function fileName(path) { return String(path).replace(/\\/g, "/").split("/").pop() }
    // Validates locally; sends nothing.
    function importFile(url) {
        var file = backend.readTextFile(url)
        if (!file.ok) { rejectImport(fileName(url), file.error); return }
        var parsed = Cal.parseFile(file.text, target)
        if (!parsed.ok) { rejectImport(fileName(file.path), parsed.error); return }
        imported = {fileName: fileName(file.path), path: file.path, cal: parsed.cal, source: parsed.source, warnings: parsed.warnings}
        importError = ""
        keep("imported", imported)
        setOperation({title: qsTr("Import"), state: "info",
                      text: qsTr("%1 validated locally; nothing was sent").arg(imported.fileName), steps: []})
    }
    function rejectImport(name, why) {
        importError = why
        setOperation({title: qsTr("Import"), state: "refused",
                      text: qsTr("%1 rejected: %2. Nothing was sent.").arg(name).arg(why), steps: []})
    }
    function discardImport() { imported = null; importError = ""; keep("imported", null) }

    // ---- Read and Export --------------------------------------------------
    function read() {
        if (readReason) return
        var key = actionKeys[Cal.ACTION_READ]
        // Provenance as it is now: the levels that allowed the read.
        var provenance = {type: target.type, createdUtc: Cal.utcText(new Date()), deviceId: target.deviceId,
                          deviceName: target.deviceName, instance: target.instance,
                          firmware: backend.info.firmware || "", gui: ("drivelink-gui " + (Qt.application.version || "")).trim(),
                          levels: sensor.levels, actualMode: sensor.mode}
        if (sending) return
        var sent = send(key, "")
        if (sent.state !== "running") {
            setOperation({title: qsTr("Read from IMU"), state: "refused", text: sent.text, steps: []})
            return
        }
        readStep = {key: key, read: provenance}
        setOperation({title: qsTr("Read from IMU"), state: "running",
                      text: qsTr("Reading; the sensor loop pauses while the sensor is in CONFIG"), steps: []})
    }
    function finishRead() {
        if (!readStep || sending) return
        var o = Cal.stepOutcome(readStep.key, backend.actionResults, backend.actionRunning, backend.linked)
        if (o.state === "running") return
        var step = readStep
        readStep = null
        if (o.state !== "ok") {
            setOperation({title: qsTr("Read from IMU"), state: o.state === "rebooted" ? "uncertain" : o.state, text: o.text, steps: []})
            return
        }
        var decoded = Cal.decodeBlob(o.blobHex)
        if (!decoded.ok) {
            setOperation({title: qsTr("Read from IMU"), state: "refused", text: qsTr("The device returned an unreadable profile: %1").arg(decoded.error), steps: []})
            return
        }
        lastRead = {cal: decoded.cal, read: step.read, at: step.read.createdUtc}
        keep("lastRead", lastRead)
        setOperation({title: qsTr("Read from IMU"), state: "ok",
                      text: qsTr("Read; the profile is also staged on the device (Unsaved). Export saves it on this PC."), steps: []})
    }

    FileDialog {
        id: exportDialog
        title: qsTr("Export the IMU calibration")
        fileMode: FileDialog.SaveFile
        modality: Qt.NonModal
        defaultSuffix: "json"
        nameFilters: [qsTr("IMU calibration (*%1)").arg(Cal.FILE_SUFFIX), qsTr("All files (*)")]
        onAccepted: panel.exportFile(selectedFile.toString())
    }
    property string exportNote: ""
    function openExport() {
        if (!lastRead) return
        var folder = exportDialog.currentFolder.toString() || StandardPaths.writableLocation(StandardPaths.DocumentsLocation).toString()
        exportDialog.selectedFile = folder.replace(/\/$/, "") + "/" + Cal.suggestedName(lastRead.read)
        exportDialog.open()
    }
    function exportFile(url) {
        if (!lastRead) return
        var written = backend.writeTextFile(url, Cal.buildFile(lastRead.cal, lastRead.read, exportNote))
        setOperation(written.ok
            ? {title: qsTr("Export"), state: "ok", text: qsTr("Saved %1 on this PC").arg(fileName(written.path)), steps: []}
            : {title: qsTr("Export"), state: "refused", text: written.error, steps: []})
    }

    // ---- Apply ------------------------------------------------------------
    function requestApply() { if (!applyReason) applyDialog.open() }
    function cancelApply() { applyDialog.close() }
    // Write, then (only after the write's reply) apply. Each sent once.
    // runAction emits the backend's change signal before it returns, which
    // calls advanceSequence again: `sending` makes that nested call a no-op,
    // so a step is never observed (or sent) before markSent recorded it.
    property bool sending: false
    function send(key, blob) {
        sending = true
        try {
            backend.runAction(key, [], blob)
            return Cal.sentOutcome(key, backend.actionResults, backend.actionRunning)
        } finally {
            sending = false
        }
    }
    function confirmApply() {
        applyDialog.close()
        if (applyReason || !imported || sending) return
        var blob = Cal.encodeBlob(imported.cal)
        var seq = Cal.newApply(nodeKey, actionKeys[Cal.ACTION_WRITE], actionKeys[Cal.ACTION_APPLY], blob, imported.fileName)
        sequence = seq
        sequence = Cal.markSent(seq, send(seq.writeKey, blob))
        showSequence()
    }
    function advanceSequence() {
        if (!sequence || sending) return
        var step = Cal.advance(sequence, {results: backend.actionResults, running: backend.actionRunning,
                                          linked: backend.ready && backend.linked, nodeKey: nodeKey,
                                          sensor: sensor, now: Date.now()})
        var seq = step.seq
        if (step.send === "apply") {
            sequence = seq
            seq = Cal.markSent(seq, send(seq.applyKey, ""))
        }
        if (JSON.stringify(seq) !== JSON.stringify(sequence) || step.send) { sequence = seq; showSequence() }
    }
    function showSequence() {
        var s = sequence
        var state = s.phase === "applied" ? "ok" : Cal.busy(s) ? "running"
                  : s.phase === "failed" ? "refused" : s.phase === "noSample" ? "info" : "uncertain"
        var op = {title: qsTr("Apply %1").arg(s.label), state: state, text: s.note,
                  steps: s.steps.map(function(t) { return {title: t.title, state: t.state, text: t.text} })}
        // The memory keeps only finished sequences: a reloaded panel never
        // resumes one (nothing is ever re-sent).
        if (Cal.busy(s)) operation = op; else setOperation(op)
    }

    // Every backend change can end a step; the timer catches "no new sample".
    Connections {
        target: panel.backend
        ignoreUnknownSignals: true
        function onChanged() { panel.finishRead(); panel.advanceSequence() }
        function onLiveChanged() { panel.advanceSequence() }
    }
    Timer {
        interval: 250; repeat: true
        running: Cal.busy(panel.sequence) || panel.sequence !== null && panel.sequence.phase === "noSample"
        onTriggered: panel.advanceSequence()
    }

    ApplyDialog {
        id: applyDialog
        parent: panel.dialogParent ? panel.dialogParent : panel
        panel: panel
    }

    // ---- Layout (G7): levels across the top; orientation and guidance on
    // the left; calibration on this PC and the last operation on the right.
    Flickable {
        anchors.fill: parent
        contentHeight: content.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: content
            width: parent.width
            spacing: Theme.space24
            StatusBanner { width: parent.width; panel: panel }
            LevelsRow { width: parent.width; panel: panel }
            Row {
                width: parent.width
                spacing: Theme.space24
                readonly property real sideWidth: Math.max(300, Math.min(420, width * 0.32))
                Column {
                    width: parent.width - parent.sideWidth - parent.spacing
                    spacing: Theme.space24
                    OrientationCard { width: parent.width; panel: panel }
                    GuidanceCard { width: parent.width; panel: panel }
                }
                Column {
                    width: parent.sideWidth
                    spacing: Theme.space24
                    FileCard { width: parent.width; panel: panel }
                    OperationCard { width: parent.width; panel: panel }
                }
            }
        }
    }
}
