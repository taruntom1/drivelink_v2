import QtQuick
import DriveLink.Style

// Device settings (issue 105 step 4): every writable system param from the
// device's own descriptors, in Layout's param rows. Reboot rows stage a value
// (violet) that Save and a reboot apply; the others apply at once and stay
// Unsaved until saved. The card has no Save or Discard: the status bar, the
// banner and the Configuration card own them. Esc and DISARM cancel an open
// edit unsent, as in Layout.
Card {
    id: card
    objectName: "settingsCard"

    required property var backend
    readonly property var info: backend.deviceSettings || ({})

    // A note in the card: info style (safe mode) or pending style.
    component Note: Rectangle {
        id: note
        property string text
        property bool pending: false
        width: parent.width
        height: noteText.implicitHeight + 2 * Theme.space12
        radius: Theme.radiusMd
        color: pending ? Theme.statePendingSubtle : Theme.bgSelected
        Rectangle {
            width: 3; height: parent.height
            color: note.pending ? Theme.statePending : Theme.accentDefault
        }
        DLIcon {
            x: Theme.space12 + 3; y: Theme.space12
            name: note.pending ? "rotate-ccw" : "info"
            size: Theme.sizeIconMd
            color: note.pending ? Theme.statePending : Theme.accentText
        }
        Text {
            id: noteText
            x: Theme.space12 + 3 + Theme.sizeIconMd + Theme.space8; y: Theme.space12
            width: parent.width - x - Theme.space12
            wrapMode: Text.Wrap
            text: note.text
            font: Theme.fontBodyDefault
            color: Theme.textPrimary
        }
    }

    visible: (info.total || 0) > 0
    title: qsTr("Device settings")
    contentSpacing: Theme.space8
    actions: [
        DLBadge {
            objectName: "settingsPendingBadge"
            visible: (card.info.pending || 0) > 0
            text: qsTr("%1 pending").arg(card.info.pending || 0)
            tone: DLBadge.Pending
        },
        DLBadge {
            objectName: "settingsUnsavedBadge"
            visible: card.info.unsaved === true
            text: qsTr("Unsaved")
            tone: DLBadge.Unsaved
        }
    ]

    Text {
        objectName: "settingsCaption"
        width: parent.width
        wrapMode: Text.Wrap
        text: qsTr("Rows marked reboot take effect after Save and a reboot; the others apply at once.")
        font: Theme.fontCaption
        color: Theme.textSecondary
    }
    Note {
        objectName: "settingsSafeNote"
        visible: card.info.safeMode === true
        text: qsTr("Safe mode: rows marked reboot run at their defaults. The saved values are kept and used after a normal boot.")
    }
    Note {
        objectName: "settingsSavedNote"
        visible: card.info.savedThenDiscarded === true
        pending: true
        text: qsTr("A reboot is pending. The next boot loads the last saved settings, which can differ from the values shown. Save and reboot keeps the values shown.")
    }
    Repeater {
        model: card.backend.systemParams
        delegate: Column {
            id: entry
            required property string key
            required property string group
            required property bool groupStart
            required property string safeNote
            required property string name
            required property string unit
            required property string description
            required property string editor
            required property string valueText
            required property string pendingText
            required property bool isPending
            required property bool unsaved
            required property bool locked
            required property bool readOnly
            required property bool rebootRequired
            required property string rangeText
            required property var enums
            required property var valueBits
            required property string editState
            required property string editNote
            required property string editText
            width: parent.width
            spacing: Theme.space8
            Text {
                visible: entry.groupStart
                topPadding: Theme.space8
                text: entry.group.toUpperCase()
                font: Theme.fontLabelCaps
                color: Theme.textSecondary
            }
            DLParamRow {
                id: paramRow
                objectName: "setting_" + entry.name
                width: parent.width
                name: entry.name; unit: entry.unit; description: entry.description; editor: entry.editor
                valueText: entry.valueText; pendingText: entry.pendingText; isPending: entry.isPending
                unsaved: entry.unsaved; locked: entry.locked; readOnly: entry.readOnly
                rebootRequired: entry.rebootRequired; rangeText: entry.rangeText; enums: entry.enums
                valueBits: entry.valueBits; editState: entry.editState; editNote: entry.editNote; editText: entry.editText
                nameWidth: Math.min(260, width * 0.3)
                editorWidth: Math.min(240, width * 0.3)
                neutralNote: entry.safeNote
                // The Layout rule: reboot rows follow the layout block (armed,
                // another client's lease), the others the common blocks.
                blockedReason: entry.rebootRequired ? (card.info.blocked || "") : (card.info.paramBlocked || "")
                blockedNote: entry.rebootRequired ? (card.info.rebootNote || "") : ""
                onCommitText: function(text) { card.backend.setParamText(entry.key, text) }
                onCommitValue: function(bits) { card.backend.setParamValue(entry.key, bits) }
                onClearEdit: card.backend.clearParamEdit(entry.key)
                onCommittedByEnter: safetyKeys.editCommitted()
                Connections {
                    target: safetyKeys
                    function onEscapeRequested() { if (paramRow.editing) paramRow.cancel() }
                }
            }
        }
    }
}
