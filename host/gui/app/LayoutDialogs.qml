import QtQuick
import DriveLink.Style

// The Layout screen's dialogs, parented to the shell body like the other
// shell dialogs (not Qt modals: the shell's scrim stays under the status bar,
// so DISARM is live above them). Add and Rename stay open until the device
// answers; a refusal is shown next to the button and nothing is staged.
Item {
    id: root

    property var backend
    property Item dialogParent
    readonly property bool anyOpen: addInstance.visible || addLoop.visible || labelDialog.visible || discardDialog.visible

    function openAddInstance() { addInstance.open() }
    function openAddLoop() { addLoop.open() }
    function openLabel(key, label) { labelDialog.key = key; labelDialog.current = label; labelDialog.open() }
    function openDiscard() { discardDialog.open() }
    function cancel() { addInstance.reject(); addLoop.reject(); labelDialog.reject(); discardDialog.reject() }

    Connections {
        target: root.backend
        function onLayoutResult(op, ok, message) {
            const dialog = op === "addInstance" ? addInstance : op === "addLoop" ? addLoop : op === "label" ? labelDialog : null
            if (!dialog || !dialog.visible) return
            dialog.busy = false
            if (ok) dialog.close(); else dialog.refusal = message
        }
    }

    // G7b "Add instance, loop choice": type list with each type's need, label,
    // and the compatible loops with the device's default first.
    DLDialog {
        id: addInstance
        objectName: "addInstanceDialog"
        parent: root.dialogParent
        width: 560
        title: qsTr("Add instance")
        text: qsTr("The instance is staged in the pending layout and starts after Save and a reboot.")
        acceptText: qsTr("Add")
        closeOnAccept: false
        property var types: []
        property int typeIndex: -1
        property bool busy: false
        property string refusal
        readonly property var type: typeIndex >= 0 && typeIndex < types.length ? types[typeIndex] : null
        readonly property var loops: type ? type.loops.filter(function(l) { return l.compatible }) : []
        acceptEnabled: !busy && type !== null && loops.length > 0 && instanceLabel.text.length > 0
                       && root.backend.nameProblem(instanceLabel.text).length === 0
        onAboutToShow: {
            types = root.backend.addInstanceTypes(); typeIndex = types.length > 0 ? 0 : -1
            instanceLabel.text = ""; refusal = ""; busy = false
        }
        onAcceptRequested: {
            refusal = ""; busy = true
            root.backend.addInstance(type.type, instanceLabel.text, loops[loopCombo.currentIndex].id)
        }
        extra: [
            Text { text: qsTr("Type"); font: Theme.fontLabelDefault; color: Theme.textSecondary },
            ListView {
                id: typeList
                objectName: "typeList"
                width: parent ? parent.width : 500
                height: Math.min(contentHeight, 5 * 44)
                clip: true
                model: addInstance.types
                delegate: DLMenuItem {
                    required property var modelData
                    required property int index
                    width: typeList.width
                    text: modelData.name
                    detail: modelData.need
                    highlighted: index === addInstance.typeIndex
                    onClicked: { addInstance.typeIndex = index; addInstance.refusal = "" }
                }
            },
            Text { text: qsTr("Label"); font: Theme.fontLabelDefault; color: Theme.textSecondary },
            DLTextField {
                id: instanceLabel
                objectName: "instanceLabel"
                width: parent ? parent.width : 500
                placeholderText: qsTr("1 to 16 bytes, unique on the device")
                error: text.length > 0 && root.backend.nameProblem(text).length > 0
                onTextEdited: addInstance.refusal = ""
            },
            Text { text: qsTr("Loop"); font: Theme.fontLabelDefault; color: Theme.textSecondary },
            DLComboBox {
                id: loopCombo
                objectName: "loopCombo"
                width: parent ? parent.width : 500
                visible: addInstance.loops.length > 0
                model: addInstance.loops.map(function(l) {
                    return l.name + " · " + l.rate + " Hz" + (l.realtime ? " · " + qsTr("real-time") : "")
                        + (l.id === addInstance.type.defaultLoop ? " · " + qsTr("device default") : "")
                })
                // The device's default (lowest compatible loop id) first.
                currentIndex: Math.max(0, addInstance.loops.findIndex(function(l) { return addInstance.type && l.id === addInstance.type.defaultLoop }))
            },
            Row {
                visible: addInstance.type !== null && addInstance.loops.length === 0
                spacing: Theme.space8
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: addInstance.type ? qsTr("No loop suits %1: it %2.").arg(addInstance.type.name).arg(addInstance.type.need) : ""
                    font: Theme.fontCaption; color: Theme.stateFault
                }
                DLButton {
                    objectName: "noLoopAddLoop"
                    kind: DLButton.Ghost; text: qsTr("Add loop"); iconName: "plus"
                    onClicked: { addInstance.reject(); addLoop.open() }
                }
            },
            Text {
                objectName: "addInstanceRefusal"
                width: parent ? parent.width : 500
                visible: addInstance.refusal.length > 0
                text: addInstance.refusal
                wrapMode: Text.Wrap
                font: Theme.fontCaption; color: Theme.stateFault
            }
        ]
    }

    // G7b "Add loop": label and the loop type's params with their defaults;
    // core is the discovered placement enum with Any core by default.
    DLDialog {
        id: addLoop
        objectName: "addLoopDialog"
        parent: root.dialogParent
        width: 560
        title: qsTr("Add loop")
        text: qsTr("The loop is staged in the pending layout and starts after Save and a reboot. Automatic placement is chosen at startup and can change after a reboot.")
        acceptText: qsTr("Add loop")
        closeOnAccept: false
        property var fields: []
        property var values: ({})
        property bool busy: false
        property string refusal
        acceptEnabled: !busy && loopLabel.text.length > 0 && root.backend.nameProblem(loopLabel.text).length === 0
        onAboutToShow: {
            fields = root.backend.addLoopFields()
            const v = {}
            for (const f of fields) v[f.id] = f.editor === "bool" ? (f.bits ? "true" : "false") : f.value
            values = v; loopLabel.text = ""; refusal = ""; busy = false
        }
        onAcceptRequested: { refusal = ""; busy = true; root.backend.addLoop(loopLabel.text, values) }
        extra: [
            Text { text: qsTr("Label"); font: Theme.fontLabelDefault; color: Theme.textSecondary },
            DLTextField {
                id: loopLabel
                objectName: "loopLabel"
                width: parent ? parent.width : 500
                placeholderText: qsTr("1 to 16 bytes, unique on the device")
                error: text.length > 0 && root.backend.nameProblem(text).length > 0
                onTextEdited: addLoop.refusal = ""
            },
            Repeater {
                model: addLoop.fields
                delegate: Row {
                    id: fieldRow
                    required property var modelData
                    spacing: Theme.space12
                    height: Theme.sizeControl
                    Text {
                        width: 140; anchors.verticalCenter: parent.verticalCenter
                        text: fieldRow.modelData.name
                        font: Theme.fontBodyDefault; color: Theme.textPrimary
                    }
                    DLNumberField {
                        objectName: "loopField_" + fieldRow.modelData.name
                        width: 200
                        visible: fieldRow.modelData.editor === "number"
                        unit: fieldRow.modelData.unit
                        text: fieldRow.modelData.value
                        onTextEdited: { const v = addLoop.values; v[fieldRow.modelData.id] = text; addLoop.values = v }
                    }
                    DLSwitch {
                        objectName: "loopField_" + fieldRow.modelData.name
                        anchors.verticalCenter: parent.verticalCenter
                        visible: fieldRow.modelData.editor === "bool"
                        checked: fieldRow.modelData.bits !== 0
                        onToggled: { const v = addLoop.values; v[fieldRow.modelData.id] = checked ? "true" : "false"; addLoop.values = v }
                    }
                    DLComboBox {
                        objectName: "loopField_" + fieldRow.modelData.name
                        width: 200
                        visible: fieldRow.modelData.editor === "enum"
                        model: fieldRow.modelData.enums
                        textRole: "name"
                        currentIndex: fieldRow.modelData.enums.findIndex(function(e) { return e.value === fieldRow.modelData.bits })
                        onActivated: function(index) {
                            const v = addLoop.values; v[fieldRow.modelData.id] = fieldRow.modelData.enums[index].name; addLoop.values = v
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: fieldRow.modelData.range
                        font: Theme.fontCaption; color: Theme.textTertiary
                    }
                }
            },
            Text {
                objectName: "addLoopRefusal"
                width: parent ? parent.width : 500
                visible: addLoop.refusal.length > 0
                text: addLoop.refusal
                wrapMode: Text.Wrap
                font: Theme.fontCaption; color: Theme.stateFault
            }
        ]
    }

    DLDialog {
        id: labelDialog
        objectName: "labelDialog"
        parent: root.dialogParent
        title: qsTr("Rename %1").arg(current)
        text: qsTr("Labels are 1 to 16 bytes and unique on the device. The new label applies after Save and a reboot.")
        acceptText: qsTr("Rename")
        closeOnAccept: false
        property string key
        property string current
        property bool busy: false
        property string refusal
        acceptEnabled: !busy && labelField.text.length > 0 && labelField.text !== current
                       && root.backend.nameProblem(labelField.text).length === 0
        onAboutToShow: { labelField.text = current; refusal = ""; busy = false; labelField.selectAll(); labelField.forceActiveFocus() }
        onAcceptRequested: { refusal = ""; busy = true; root.backend.setNodeLabel(key, labelField.text) }
        extra: [
            DLTextField {
                id: labelField
                objectName: "labelField"
                width: parent ? parent.width : 400
                error: text.length > 0 && root.backend.nameProblem(text).length > 0
                onTextEdited: labelDialog.refusal = ""
                onAccepted: if (labelDialog.acceptEnabled) labelDialog.requestAccept()
            },
            Text {
                objectName: "labelRefusal"
                width: parent ? parent.width : 400
                visible: labelDialog.refusal.length > 0
                text: labelDialog.refusal
                wrapMode: Text.Wrap
                font: Theme.fontCaption; color: Theme.stateFault
            }
        ]
    }

    // G4 "Discard all confirm": lists what DISCARD_PENDING drops.
    DLDialog {
        id: discardDialog
        objectName: "discardDialog"
        parent: root.dialogParent
        destructive: true
        title: qsTr("Discard all pending changes?")
        text: qsTr("The device drops every staged layout change and reboot param. Live changes stay. There is no per-change undo.")
        acceptText: qsTr("Discard all")
        onAccepted: root.backend.discardPending()
        extra: [
            Repeater {
                model: (root.backend.layout.pendingChanges || []).slice(0, 12)
                delegate: PendingChangeText { width: discardDialog.availableWidth }
            }
        ]
    }
}
