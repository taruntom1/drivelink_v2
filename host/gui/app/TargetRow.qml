import QtQuick
import DriveLink.Style

// A Wiring (`ref:` param) or Sources row: the name, a RefPicker/SourcePicker
// and the row's state under it. A refused target keeps the proposed text with
// the reason; the pending layout is unchanged. A picked target is sent once.
// A source row also shows its live value and age right of the picker and its
// state (missing, stale, no sample) under it; an invalid sample's value and a
// pending target's are never shown.
Column {
    id: row

    required property var modelData
    property var backend
    property bool source: false
    // Why reboot params cannot change now (armed, busy); empty when they can.
    property string blocked
    // A source's live state: {state, valueText, ageText, note}.
    property var live: ({})

    readonly property string editState: modelData.editState || ""
    readonly property bool showsValue: source && (live.state === "valid" || live.state === "stale")
    readonly property real liveWidth: source ? 132 : 0
    spacing: Theme.space4

    Row {
        width: parent.width
        spacing: Theme.space12
        Text {
            width: 120
            anchors.verticalCenter: parent.verticalCenter
            text: row.modelData.name
            font: row.source ? Theme.fontMonoDefault : Theme.fontBodyDefault
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
        DLReasonHover {
            width: parent.width - 120 - Theme.space12 - (row.source ? row.liveWidth + Theme.space12 : 0)
            reason: row.blocked || row.modelData.blocked || ""
            DLRefPicker {
                objectName: (row.source ? "source_" : "wiring_") + row.modelData.name
                width: parent.width
                enabled: !(row.blocked || row.modelData.blocked)
                chipText: row.modelData.chip || ""
                valueText: row.editState === "rejected" || row.editState === "sending"
                           ? row.modelData.editText : row.modelData.valueText
                status: row.editState === "rejected" || row.editState === "uncertain" ? DLRefPicker.Rejected
                      : row.modelData.isPending ? DLRefPicker.Pending
                      : row.modelData.unset ? DLRefPicker.Unset : DLRefPicker.Normal
                onAboutToOpen: candidates = row.backend.layoutCandidates(row.modelData.key)
                onPicked: function(value) { row.backend.setParamValue(row.modelData.key, value) }
            }
        }
        Column {
            visible: row.source
            width: row.liveWidth
            anchors.verticalCenter: parent.verticalCenter
            Text {
                objectName: "sourceValue_" + row.modelData.name
                width: parent.width; elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
                text: row.showsValue ? row.live.valueText : "—"
                font: Theme.fontBodyNumeric
                color: row.live.state === "stale" ? Theme.textSecondary : row.showsValue ? Theme.textPrimary : Theme.textTertiary
            }
            Text {
                objectName: "sourceAge_" + row.modelData.name
                width: parent.width; elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
                visible: row.showsValue
                text: qsTr("age %1").arg(row.live.ageText || "")
                font: Theme.fontCaption; color: Theme.textSecondary
            }
        }
    }
    Text {
        objectName: "targetState_" + row.modelData.name
        x: 120 + Theme.space12
        width: parent.width - x
        wrapMode: Text.Wrap
        readonly property var content: {
            if (row.editState === "sending") return [qsTr("Sending…"), Theme.textSecondary]
            if (row.editState === "rejected" || row.editState === "uncertain")
                return [qsTr("%1. Current: %2").arg(row.modelData.editNote).arg(row.modelData.valueText), Theme.stateFault]
            if (row.modelData.isPending && row.modelData.currentText)
                return [qsTr("Current: %1 → Pending: %2 · after reboot").arg(row.modelData.currentText).arg(row.modelData.valueText), Theme.statePending]
            if (row.modelData.isPending) return [qsTr("Pending · after reboot"), Theme.statePending]
            if (row.modelData.unset) return [row.source ? qsTr("Not connected") : qsTr("Not wired: choose a target"), Theme.stateUnsaved]
            if (row.source && row.live.note)
                return [row.live.note, row.live.state === "missing" ? Theme.stateFault
                                       : row.live.state === "stale" ? Theme.stateUnsaved : Theme.textSecondary]
            return ["", Theme.textSecondary]
        }
        visible: text.length > 0
        text: content[0]
        color: content[1]
        font: Theme.fontCaption
    }
}
