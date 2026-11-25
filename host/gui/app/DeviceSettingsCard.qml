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
    readonly property var trial: backend.linkTrial || ({})
    function countdownText(ms) { return qsTr("%1 s left").arg(Math.ceil(Math.max(0, ms || 0) / 1000)) }

    // A note in the card: info (safe mode), pending, or warning (link change).
    component Note: Rectangle {
        id: note
        property string text
        property bool pending: false
        property bool warning: false
        width: parent.width
        height: noteText.implicitHeight + 2 * Theme.space12
        radius: Theme.radiusMd
        color: warning ? Theme.stateArmedSubtle : pending ? Theme.statePendingSubtle : Theme.bgSelected
        Rectangle {
            width: 3; height: parent.height
            color: note.warning ? Theme.stateArmed : note.pending ? Theme.statePending : Theme.accentDefault
        }
        DLIcon {
            x: Theme.space12 + 3; y: Theme.space12
            name: note.warning ? "triangle-alert" : note.pending ? "rotate-ccw" : "info"
            size: Theme.sizeIconMd
            color: note.warning ? Theme.stateArmed : note.pending ? Theme.statePending : Theme.accentText
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
    Note {
        objectName: "settingsLinkWarningNote"
        visible: card.trial.staged === true
        warning: true
        text: card.trial.pinsChanged === true
            ? qsTr("Serial link pin change pending (%1 → %2). After Save and reboot, move the UART wires to TX %3 / RX %4 within 120 s or the board reverts to %1.")
                .arg(card.trial.runningText || "").arg(card.trial.nextText || "").arg(card.trial.nextTx || 0).arg(card.trial.nextRx || 0)
            : qsTr("Serial link rate change pending (%1 → %2). After Save and reboot, the new rate runs on a 30 s trial and reverts to %1 unless confirmed.")
                .arg(card.trial.runningText || "").arg(card.trial.nextText || "")
    }
    // Active serial link trial banner with live countdown and Confirm link button.
    Rectangle {
        objectName: "settingsLinkTrialBanner"
        visible: card.trial.active === true
        width: parent.width
        height: trialCol.implicitHeight + 2 * Theme.space12
        radius: Theme.radiusMd
        color: Theme.stateArmedSubtle
        Rectangle { width: 3; height: parent.height; color: Theme.stateArmed }
        Column {
            id: trialCol
            x: Theme.space12 + 3; y: Theme.space12
            width: parent.width - x - Theme.space12
            spacing: Theme.space8
            Row {
                width: parent.width
                spacing: Theme.space8
                DLIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "triangle-alert"; size: Theme.sizeIconMd; color: Theme.stateArmed
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Serial link on trial")
                    font: Theme.fontBodyStrong; color: Theme.textPrimary
                }
                DLBadge {
                    objectName: "settingsLinkTrialCountdown"
                    anchors.verticalCenter: parent.verticalCenter
                    text: card.countdownText(card.trial.remainingMs)
                    tone: DLBadge.Armed
                    size: DLBadge.Sm
                }
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: qsTr("Running %1. Reverts to %2 when the timer expires unless confirmed. ARM and firmware updates are paused while the trial runs.")
                    .arg(card.trial.runningText || "").arg(card.trial.confirmedText || "")
                font: Theme.fontBodyDefault; color: Theme.textPrimary
            }
            DLReasonHover {
                reason: card.trial.confirmBlocked || ""
                DLButton {
                    objectName: "confirmLinkButton"
                    kind: DLButton.Primary
                    iconName: "check"
                    text: card.trial.confirmBusy ? qsTr("Confirming…") : qsTr("Confirm link")
                    enabled: !card.trial.confirmBlocked && !card.trial.confirmBusy
                    onClicked: card.backend.confirmLink()
                }
            }
        }
    }
    // Dismissible link-trial outcome banner (Confirmed, Reverted, Deferred, etc.).
    Rectangle {
        objectName: "settingsLinkOutcomeBanner"
        readonly property string outcome: card.trial.outcome || ""
        readonly property bool isOk: outcome === "confirmed"
        readonly property bool isFault: outcome === "reverted" || outcome === "wrong_device" || outcome === "not_rebooted"
        visible: outcome.length > 0
        width: parent.width
        height: outcomeCol.implicitHeight + 2 * Theme.space12
        radius: Theme.radiusMd
        color: isOk ? Theme.stateOkSubtle : isFault ? Theme.stateFaultSubtle : Theme.bgSelected
        Rectangle {
            width: 3; height: parent.height
            color: parent.isOk ? Theme.stateOk : parent.isFault ? Theme.stateFault : Theme.accentDefault
        }
        DLIcon {
            x: Theme.space12 + 3; y: Theme.space12
            name: parent.isOk ? "check" : parent.isFault ? "triangle-alert" : "info"
            size: Theme.sizeIconMd
            color: parent.isOk ? Theme.stateOk : parent.isFault ? Theme.stateFault : Theme.accentText
        }
        Column {
            id: outcomeCol
            x: Theme.space12 + 3 + Theme.sizeIconMd + Theme.space8; y: Theme.space12
            width: parent.width - x - dismissOutcomeBtn.width - Theme.space16
            spacing: Theme.space4
            Text {
                width: parent.width; wrapMode: Text.Wrap
                text: card.trial.outcomeText || ""
                font: Theme.fontBodyStrong; color: Theme.textPrimary
            }
            Text {
                width: parent.width; wrapMode: Text.Wrap
                visible: text.length > 0
                text: card.trial.outcomeDetail || ""
                font: Theme.fontBodyDefault; color: Theme.textSecondary
            }
        }
        DLIconButton {
            id: dismissOutcomeBtn
            objectName: "dismissLinkOutcomeButton"
            anchors.right: parent.right; anchors.rightMargin: Theme.space8
            anchors.top: parent.top; anchors.topMargin: Theme.space8
            size: DLIconButton.Sm; iconName: "x"; toolTipText: qsTr("Dismiss")
            onClicked: card.backend.dismissLinkOutcome()
        }
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
                // another client's lease), the others the common blocks. Link
                // rows are also locked while a serial link trial is active.
                readonly property bool linkTrialLocked: entry.group === "link" && card.info.linkTrialActive === true
                blockedReason: linkTrialLocked ? qsTr("Confirm or wait for the active link trial to finish")
                             : entry.rebootRequired ? (card.info.blocked || "") : (card.info.paramBlocked || "")
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
