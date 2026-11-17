import QtQuick
import DriveLink.Style

// One action of the selected instance (G7b Actions card): its name, where it
// runs, and Run. An action with arguments or a blob input opens an inline
// form in the card (no modal, so DISARM stays reachable); the backend checks
// the input with the descriptors before anything is sent. The last outcome
// shows under the row: typed results, a blob as size and hex with Copy, a
// refusal, or an uncertain outcome (sent once, never repeated).
Column {
    id: row

    // ActionTable roles.
    required property string key
    required property string name
    required property string description
    required property string placement
    required property var args
    required property var results
    required property string blob
    required property string blocked
    required property bool running
    required property var result
    property var backend

    readonly property bool blobIn: blob === "in" || blob === "both"
    readonly property bool needsForm: (args || []).length > 0 || blobIn
    readonly property var outcome: result || ({})
    property bool formOpen: false
    property var texts: []
    property string blobText
    // The backend's verdict on the form's input: {problem, field}.
    // Re-checked when the row's state changes too (armed, another action).
    readonly property var check: {
        row.blocked; row.running
        return formOpen ? backend.checkAction(key, texts, blobText) : ({})
    }
    readonly property string problem: check.problem || ""

    objectName: "action_" + name
    spacing: Theme.space8

    function open() {
        const t = []
        for (let i = 0; i < args.length; ++i) t.push("")
        texts = t; blobText = ""; formOpen = true
    }
    function setText(i, text) { const t = texts.slice(); t[i] = text; texts = t }
    function send() {
        if (problem.length > 0) return
        backend.runAction(key, texts, blobText)
        // Closed only once sent; a local refusal keeps the typed input.
        if (row.running) formOpen = false
    }

    Item {
        width: parent.width
        height: Theme.sizeControl
        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - runHover.width - Theme.space12
            Text {
                width: parent.width; elide: Text.ElideRight
                text: row.name
                font: Theme.fontMonoDefault; color: Theme.textPrimary
            }
            Text {
                width: parent.width; elide: Text.ElideRight
                text: (row.placement === "paused" ? qsTr("paused action") : qsTr("runs in loop"))
                    + (row.blob !== "none" ? " · " + qsTr("blob %1").arg(row.blob) : "")
                    + (row.description ? " · " + row.description : "")
                font: Theme.fontCaption; color: Theme.textSecondary
            }
        }
        DLReasonHover {
            id: runHover
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            reason: row.running ? "" : row.blocked
            DLButton {
                objectName: "actionRun_" + row.name
                text: row.running ? qsTr("Running…") : row.needsForm && !row.formOpen ? qsTr("Run…") : qsTr("Run")
                iconName: "play"
                enabled: !row.blocked && !row.running && !row.formOpen
                onClicked: row.needsForm ? row.open() : row.backend.runAction(row.key, [], "")
            }
        }
    }

    // The inline form: one field per argument, the blob as hex.
    Column {
        objectName: "actionForm"
        visible: row.formOpen
        width: parent.width
        spacing: Theme.space8
        // A count, not the args array: a row update (blocked, an outcome)
        // brings a new array, which would recreate the fields and lose text.
        Repeater {
            model: row.formOpen ? (row.args || []).length : 0
            delegate: DLTextField {
                required property int index
                readonly property var arg: (row.args || [])[index] || ({})
                objectName: "actionArg_" + index
                width: row.width
                monospace: true
                placeholderText: arg.name + " (" + arg.type + (arg.unit ? ", " + arg.unit : "") + ")"
                error: row.check.field === index && text.length > 0
                onTextChanged: row.setText(index, text)
                onAccepted: row.send()
            }
        }
        DLTextField {
            objectName: "actionBlob"
            visible: row.blobIn
            width: row.width
            monospace: true
            placeholderText: qsTr("Blob as hex bytes, at most 512 (e.g. 01 02 ff)")
            error: row.check.field === -2 && text.length > 0
            onTextChanged: row.blobText = text
            onAccepted: row.send()
        }
        Text {
            objectName: "actionProblem"
            width: row.width; wrapMode: Text.Wrap
            visible: text.length > 0
            text: row.problem
            font: Theme.fontCaption; color: Theme.stateFault
        }
        Row {
            spacing: Theme.space8
            DLButton {
                objectName: "actionSend"
                kind: DLButton.Primary
                text: qsTr("Run")
                iconName: "play"
                enabled: row.problem.length === 0
                onClicked: row.send()
            }
            DLButton {
                objectName: "actionCancel"
                kind: DLButton.Ghost
                text: qsTr("Cancel")
                onClicked: row.formOpen = false
            }
        }
    }

    // The last outcome.
    Text {
        objectName: "actionResult"
        width: parent.width; wrapMode: Text.Wrap
        readonly property var values: row.outcome.values || []
        visible: text.length > 0
        text: {
            const s = row.outcome.state || ""
            if (s === "ok") {
                const parts = values.map(v => v.name + " " + v.text + (v.unit ? " " + v.unit : ""))
                return parts.length > 0 ? parts.join(" · ") : (row.outcome.text || "")
            }
            return row.outcome.text || ""
        }
        font: (row.outcome.state || "") === "ok" ? Theme.fontMonoSmall : Theme.fontCaption
        color: row.outcome.state === "refused" || row.outcome.state === "invalid" || row.outcome.state === "uncertain"
            ? Theme.stateFault : row.outcome.state === "ok" ? Theme.stateOk : Theme.textSecondary
    }
    Column {
        objectName: "actionBlobOut"
        visible: row.outcome.blobHex !== undefined
        width: parent.width
        spacing: Theme.space4
        Item {
            width: parent.width; height: copyButton.height
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Blob · %n byte(s)", "", row.outcome.blobSize || 0)
                font: Theme.fontCaption; color: Theme.textSecondary
            }
            DLIconButton {
                id: copyButton
                objectName: "actionCopy"
                anchors.right: parent.right
                size: DLIconButton.Sm
                iconName: "copy"
                toolTipText: qsTr("Copy the hex bytes")
                onClicked: row.backend.copyToClipboard(row.outcome.blobHex || "")
            }
        }
        TextEdit {
            objectName: "actionBlobHex"
            width: parent.width
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            text: row.outcome.blobHex || ""
            font: Theme.fontMonoSmall; color: Theme.textPrimary
        }
    }
}
