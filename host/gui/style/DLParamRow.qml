import QtQuick
import QtQuick.Templates as T

// Figma ParamRow: name, the editor chosen by item type (number, GPIO, enum,
// bool), and the row's state as a note. A left bar marks Unsaved (yellow) or
// Pending (violet). Pending never looks live: a staged value shows
// "current → pending · after reboot".
//
// A typed value is committed on Enter or when the field loses focus, and only
// when it differs from what the device holds and from what was last sent.
// `cancel()` (Esc or DISARM) puts the device value back without sending.
Item {
    id: row

    property string name
    property string editor: "number"   // number, gpio, enum, bool
    property string unit
    property string description
    property string valueText
    property string pendingText
    property bool isPending: false
    property bool unsaved: false
    property bool locked: false
    property bool readOnly: false
    property bool rebootRequired: false
    property string rangeText
    property var enums: []
    property real valueBits: 0
    property string editState         // sending, applied, clamped, rejected, uncertain
    property string editNote
    property string editText
    // Why the editor is disabled (armed, rebooting, busy); empty when enabled.
    property string blockedReason
    property real nameWidth: 160
    property real editorWidth: 200

    signal commitText(string text)
    signal commitValue(var bits)
    signal clearEdit()
    signal committedByEnter()

    readonly property string shown: isPending ? pendingText : valueText
    readonly property bool editing: field.activeFocus
    readonly property bool edited: field.visible && field.activeFocus && field.text !== shown
    readonly property bool usable: !readOnly && !locked && blockedReason.length === 0
    property string lastSent
    // The user has typed since the last commit or reset: their text stays.
    property bool typing: false

    function commit(text) {
        typing = false
        if (!field.visible) return
        // Nothing new: the device's value, the value just sent (still sending,
        // or clamped and not yet read back), or a refused value not edited.
        if (text === shown || text === lastSent) return
        if ((editState === "rejected" || editState === "uncertain") && text === editText) return
        lastSent = text
        commitText(text)
    }
    function cancel() {
        if (editState === "rejected" || editState === "uncertain") clearEdit()
        typing = false
        lastSent = ""
        field.text = shown
    }
    // valueBits is the staged value of a pending row, else the current one.
    function enumIndex() {
        for (let i = 0; i < row.enums.length; ++i) if (row.enums[i].value === row.valueBits) return i
        return -1
    }
    // A field shows the device's value (a kept refusal: its text) unless the
    // user is typing, or the sent text is still on its way.
    function resetText() {
        if (field.activeFocus && (typing || editState === "sending")) return
        // An unanswered send shows what was read back, not the typed text.
        field.text = editState === "rejected" ? editText : shown
        if (editState !== "sending") lastSent = ""
    }
    onShownChanged: resetText()
    onEditStateChanged: resetText()
    Component.onCompleted: resetText()

    implicitWidth: 760
    implicitHeight: Theme.sizeControl

    Rectangle {
        width: 2; height: parent.height - Theme.space4
        anchors.verticalCenter: parent.verticalCenter
        visible: row.isPending || row.unsaved
        color: row.isPending ? Theme.statePending : Theme.stateUnsaved
    }
    Text {
        id: nameText
        x: Theme.space8
        width: row.nameWidth - Theme.space8
        anchors.verticalCenter: parent.verticalCenter
        text: row.name
        font: Theme.fontBodyDefault
        color: Theme.textPrimary
        elide: Text.ElideRight
        HoverHandler { id: nameHover }
        DLToolTip { visible: nameHover.hovered && row.description.length > 0; text: row.description }
    }
    DLReasonHover {
        id: editorSlot
        x: row.nameWidth
        anchors.verticalCenter: parent.verticalCenter
        reason: row.readOnly ? qsTr("Read-only") : row.locked ? qsTr("Locked while armed; disarm first") : row.blockedReason
        width: row.editorWidth; height: Theme.sizeControl
        // The control the reason hover watches; the editors sit inside it.
        Item {
            width: parent.width; height: parent.height
            enabled: row.usable
            DLNumberField {
                id: field
                objectName: "paramField"
                anchors.fill: parent
                visible: row.editor === "number" || row.editor === "gpio"
                unit: row.editor === "gpio" ? "" : row.unit
                placeholderText: row.editor === "gpio" ? qsTr("No pin") : ""
                status: row.locked ? DLNumberField.Locked
                      : row.editState === "rejected" || row.editState === "uncertain" ? DLNumberField.Rejected
                      : row.edited ? DLNumberField.Edited
                      : row.editState === "clamped" ? DLNumberField.Clamped
                      : row.isPending ? DLNumberField.Pending : DLNumberField.Normal
                onAccepted: { row.commit(text); row.committedByEnter() }
                onEditingFinished: row.commit(text)
                onTextEdited: {
                    row.typing = true
                    if ((row.editState === "rejected" || row.editState === "uncertain") && text !== row.editText) row.clearEdit()
                }
            }
            DLComboBox {
                objectName: "paramEnum"
                anchors.fill: parent
                visible: row.editor === "enum"
                model: row.enums
                textRole: "name"
                currentIndex: row.enumIndex()
                displayText: row.shown
                onActivated: function(index) {
                    row.commitValue(row.enums[index].value)
                    currentIndex = Qt.binding(row.enumIndex)
                }
            }
            DLSwitch {
                objectName: "paramSwitch"
                anchors.verticalCenter: parent.verticalCenter
                visible: row.editor === "bool"
                checked: row.valueBits !== 0
                onToggled: {
                    row.commitValue(checked ? 1 : 0)
                    checked = Qt.binding(function() { return row.valueBits !== 0 })
                }
            }
        }
    }
    Row {
        anchors.left: editorSlot.right; anchors.leftMargin: Theme.space12
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space8
        DLBadge {
            anchors.verticalCenter: parent.verticalCenter
            visible: row.rebootRequired
            size: DLBadge.Sm
            tone: row.isPending ? DLBadge.Pending : DLBadge.Neutral
            iconName: "rotate-ccw"
            text: qsTr("reboot")
        }
        Text {
            id: note
            objectName: "paramNote"
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - (row.rebootRequired ? 80 : 0)
            elide: Text.ElideRight
            font: Theme.fontCaption
            readonly property var content: {
                if (row.edited) return [qsTr("Not sent · Enter sends"), Theme.stateUnsaved]
                if (row.editState === "sending") return [qsTr("Sending…"), Theme.textSecondary]
                if (row.editState === "rejected" || row.editState === "uncertain") return [row.editNote, Theme.stateFault]
                if (row.editState === "clamped") return [row.editNote, Theme.accentText]
                if (row.locked) return [qsTr("Locked while armed"), Theme.stateLocked]
                if (row.isPending && row.valueText.length > 0 && row.valueText !== row.pendingText)
                    return [qsTr("Current %1 → Pending %2 · after reboot").arg(row.valueText).arg(row.pendingText), Theme.statePending]
                if (row.isPending) return [qsTr("Pending · after reboot"), Theme.statePending]
                if (row.editState === "applied") return [qsTr("Applied"), Theme.stateOk]
                if (row.unsaved) return [qsTr("Changed since the last save"), Theme.textSecondary]
                return [row.rangeText, Theme.textTertiary]
            }
            text: content[0]
            color: content[1]
            HoverHandler { id: noteHover }
            DLToolTip { visible: noteHover.hovered && note.truncated; text: note.text }
        }
    }
}
