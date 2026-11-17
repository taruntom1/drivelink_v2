import QtQuick
import DriveLink.Style

// G4/G7b Layout: the Instances card (tree by loop, Add menu, counts against
// the device's limits) and the selected loop's or instance's detail (header
// with Rename and Remove, Sources, Params, Wiring, the loop's instances).
// Everything comes from the device's descriptors; refusals show next to what
// was refused. The live half (H2c-2): Signals (live) and Actions for an
// instance, Statistics for a loop, each source's value, age and state, and a
// module type's custom panel as a second tab when one is compiled in.
Item {
    id: page
    objectName: "layoutPage"

    property var backend
    readonly property var info: backend.layout
    readonly property var node: info.node || ({})
    readonly property bool hasNode: node.key !== undefined
    readonly property bool isLoop: node.isLoop === true
    readonly property var counts: info.counts || ({})
    readonly property var paramCounts: info.paramCounts || ({})
    readonly property var wiring: info.wiring || []
    readonly property var sources: info.sources || []
    readonly property var members: info.members || []
    readonly property int actionCount: actionsRepeater.count
    readonly property var live: backend.live || ({})
    readonly property var liveSignals: live.signals || []
    readonly property var liveSources: live.sources || ({})
    readonly property var statistics: live.statistics || ({})
    readonly property string panelUrl: info.panel || ""
    // 0 Params, 1 the custom panel.
    property int tab: 0
    readonly property string nodeKey: node.key || ""
    onNodeKeyChanged: tab = 0
    onPanelUrlChanged: if (panelUrl.length === 0) tab = 0
    readonly property real treeWidth: Math.max(340, Math.min(440, width * 0.24))
    readonly property real sideWidth: Math.max(360, Math.min(480, width * 0.25))

    signal addInstanceRequested()
    signal addLoopRequested()
    signal labelRequested(string key, string label)

    function rateText(hz) { return hz >= 1000 && hz % 100 === 0 ? (hz / 1000) + " kHz" : hz + " Hz" }
    function limitText(n, limit, what) { return limit > 0 ? qsTr("%1 of %2 %3").arg(n).arg(limit).arg(what) : qsTr("%1 %2").arg(n).arg(what) }

    // Instances card.
    Rectangle {
        id: tree
        x: Theme.space24; y: Theme.space24
        width: page.treeWidth; height: page.height - 2 * Theme.space24
        color: Theme.bgSurface
        radius: Theme.radiusLg
        border.width: Theme.borderWidth
        border.color: Theme.borderDefault

        Text {
            x: Theme.space24; y: Theme.space24
            text: qsTr("Instances")
            font: Theme.fontTitleSection; color: Theme.textPrimary
        }
        DLButton {
            id: addButton
            objectName: "addButton"
            anchors.right: parent.right; anchors.rightMargin: Theme.space24; y: Theme.space16
            text: qsTr("Add")
            iconName: "plus"
            onClicked: addMenu.open()
            DLMenu {
                id: addMenu
                y: addButton.height + Theme.space4
                DLMenuItem {
                    objectName: "addInstanceItem"
                    text: qsTr("Instance")
                    detail: page.info.addInstanceBlocked || page.limitText(page.counts.instances, page.counts.instanceLimit, qsTr("instances"))
                    enabled: !page.info.addInstanceBlocked
                    onTriggered: page.addInstanceRequested()
                }
                DLMenuItem {
                    objectName: "addLoopItem"
                    text: qsTr("Loop")
                    detail: page.info.addLoopBlocked || page.limitText(page.counts.loops, page.counts.loopLimit, qsTr("loops"))
                    enabled: !page.info.addLoopBlocked
                    onTriggered: page.addLoopRequested()
                }
            }
        }
        Text {
            id: countText
            objectName: "layoutCounts"
            x: Theme.space24; y: addButton.y + addButton.height + Theme.space8
            width: parent.width - 2 * Theme.space24
            text: page.limitText(page.counts.loops, page.counts.loopLimit, qsTr("loops")) + " · "
                + page.limitText(page.counts.instances, page.counts.instanceLimit, qsTr("module instances"))
            font: Theme.fontCaption; color: Theme.textSecondary
            elide: Text.ElideRight
        }
        ListView {
            id: treeView
            objectName: "layoutTree"
            x: Theme.space16; y: countText.y + countText.height + Theme.space16
            width: parent.width - 2 * Theme.space16; height: parent.height - y - Theme.space16
            clip: true
            spacing: Theme.space2
            model: page.backend.layoutTree
            delegate: DLTreeItem {
                required property string key
                required isLoop
                required depth
                required label
                required typeName
                required health
                required healthText
                required pending
                required rateHz
                required realtime
                required budget
                required hasBudget
                objectName: "tree_" + key
                width: treeView.width
                selected: key === page.info.selected
                onClicked: page.backend.selectNode(key)
            }
        }
        Text {
            anchors.centerIn: treeView
            width: treeView.width - 2 * Theme.space16
            visible: treeView.count === 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("No loops. Add a loop, then the instances that run in it.")
            font: Theme.fontBodyDefault; color: Theme.textSecondary
        }
    }

    // The selected node's detail.
    Item {
        id: detail
        anchors.left: tree.right; anchors.leftMargin: Theme.space24
        anchors.right: parent.right; anchors.rightMargin: Theme.space24
        y: Theme.space24; height: page.height - 2 * Theme.space24
        visible: page.hasNode

        Column {
            id: header
            width: parent.width - actions.width - Theme.space16
            spacing: Theme.space4
            Row {
                spacing: Theme.space8
                Text {
                    objectName: "nodeLabel"
                    text: page.node.label || ""
                    font: Theme.fontTitlePage; color: Theme.textPrimary
                }
                DLBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: (page.node.pending || "").length > 0
                    size: DLBadge.Sm; tone: DLBadge.Pending
                    text: page.node.pending === "add" ? qsTr("pending add") : page.node.pending === "remove" ? qsTr("pending remove")
                        : page.node.pending === "label" ? qsTr("label pending") : qsTr("pending change")
                }
            }
            Text {
                objectName: "nodeSubtitle"
                width: parent.width; elide: Text.ElideRight
                text: page.isLoop
                    ? [qsTr("loop"), page.node.realtime ? qsTr("real-time") : qsTr("non-real-time"), page.rateText(page.node.rateHz || 0), page.node.healthText || ""].join(" · ")
                    : [page.node.typeName || "", qsTr("%1 loop").arg(page.node.loopName || ""), page.node.healthText || ""].join(" · ")
                font: Theme.fontMonoSmall; color: Theme.textSecondary
            }
        }
        Row {
            id: actions
            anchors.right: parent.right
            spacing: Theme.space8
            DLReasonHover {
                reason: page.node.renameBlocked || ""
                DLButton {
                    objectName: "renameNode"
                    text: qsTr("Rename"); iconName: "pencil"
                    enabled: !page.node.renameBlocked
                    onClicked: page.labelRequested(page.node.key, page.node.label)
                }
            }
            DLReasonHover {
                reason: page.node.removeBlocked || ""
                DLButton {
                    objectName: "removeNode"
                    kind: DLButton.Danger
                    text: page.isLoop ? qsTr("Remove loop") : qsTr("Remove instance")
                    iconName: "trash-2"
                    enabled: !page.node.removeBlocked
                    onClicked: page.backend.removeNode(page.node.key)
                }
            }
        }
        Row {
            id: tabs
            y: header.height + Theme.space16
            DLTabButton { objectName: "paramsTab"; text: qsTr("Params"); checked: page.tab === 0; onClicked: page.tab = 0 }
            DLTabButton {
                objectName: "panelTab"
                visible: page.panelUrl.length > 0
                text: panelLoader.item && panelLoader.item.title ? panelLoader.item.title : qsTr("Panel")
                checked: page.tab === 1
                onClicked: page.tab = 1
            }
        }
        Rectangle {
            y: tabs.y + tabs.height
            width: parent.width; height: Theme.borderWidth
            color: Theme.borderDefault
        }

        // A module type's own panel (DriveLink.Modules/<type name>/Panel.qml),
        // given the backend and the selected node's key.
        Loader {
            id: panelLoader
            objectName: "panelLoader"
            y: tabs.y + tabs.height + Theme.space16
            width: parent.width; height: parent.height - y
            visible: page.tab === 1
            active: page.panelUrl.length > 0
            source: page.panelUrl
            onLoaded: {
                item.backend = Qt.binding(() => page.backend)
                item.nodeKey = Qt.binding(() => page.nodeKey)
            }
        }

        Item {
            id: body
            y: tabs.y + tabs.height + Theme.space16
            width: parent.width; height: parent.height - y
            visible: page.tab === 0
            readonly property bool hasSide: page.isLoop || page.wiring.length > 0 || page.liveSignals.length > 0
                                            || page.actionCount > 0

            Column {
                id: mainColumn
                width: body.hasSide ? body.width - page.sideWidth - Theme.space24 : body.width
                height: body.height
                spacing: Theme.space24

                Card {
                    id: sourcesCard
                    objectName: "sourcesCard"
                    width: parent.width
                    visible: page.sources.length > 0
                    title: qsTr("Sources")
                    Text {
                        width: parent.width; wrapMode: Text.Wrap
                        text: qsTr("Each source reads a signal or input of another instance, in any loop. A new target applies after Save and a reboot.")
                        font: Theme.fontCaption; color: Theme.textSecondary
                    }
                    Repeater {
                        model: page.sources
                        delegate: TargetRow {
                            width: sourcesCard.width - 2 * Theme.space24; backend: page.backend; source: true
                            blocked: page.info.blocked || ""
                            live: page.liveSources[modelData.key] || ({})
                        }
                    }
                    Text {
                        width: parent.width; wrapMode: Text.Wrap
                        text: qsTr("Invalid sampled values are hidden. A stale value is the last sampled value; its age alone does not identify a paused or stalled loop.")
                        font: Theme.fontCaption; color: Theme.textTertiary
                    }
                }

                Rectangle {
                    id: paramsCard
                    objectName: "paramsCard"
                    width: parent.width
                    height: Math.max(160, mainColumn.height - (sourcesCard.visible ? sourcesCard.height + mainColumn.spacing : 0))
                    visible: (page.paramCounts.total || 0) > 0
                    color: Theme.bgSurface
                    radius: Theme.radiusLg
                    border.width: Theme.borderWidth
                    border.color: Theme.borderDefault

                    Row {
                        id: paramsTitle
                        x: Theme.space24; y: Theme.space24
                        spacing: Theme.space8
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: page.isLoop ? qsTr("Loop parameters") : qsTr("Params")
                            font: Theme.fontTitleSection; color: Theme.textPrimary
                        }
                        DLFilterChip {
                            objectName: "filterAll"
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("All")
                            checked: page.info.paramFilter === 0
                            onClicked: page.backend.setParamFilter(0, filterField.text)
                        }
                        DLFilterChip {
                            objectName: "filterChanged"
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Changed %1").arg(page.paramCounts.changed || 0)
                            tone: (page.paramCounts.changed || 0) > 0 ? DLBadge.Unsaved : DLBadge.Neutral
                            checked: page.info.paramFilter === 1
                            onClicked: page.backend.setParamFilter(1, filterField.text)
                        }
                        DLFilterChip {
                            objectName: "filterPending"
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Pending %1").arg(page.paramCounts.pending || 0)
                            tone: (page.paramCounts.pending || 0) > 0 ? DLBadge.Pending : DLBadge.Neutral
                            checked: page.info.paramFilter === 2
                            onClicked: page.backend.setParamFilter(2, filterField.text)
                        }
                    }
                    DLTextField {
                        id: filterField
                        objectName: "paramFilter"
                        anchors.right: parent.right; anchors.rightMargin: Theme.space24; y: Theme.space16
                        width: Math.min(280, parent.width / 3)
                        placeholderText: qsTr("Filter params")
                        onTextChanged: page.backend.setParamFilter(page.info.paramFilter || 0, text)
                    }
                    Text {
                        id: paramsHint
                        x: Theme.space24; y: paramsTitle.y + paramsTitle.height + Theme.space8
                        width: parent.width - 2 * Theme.space24
                        visible: page.isLoop
                        text: qsTr("rate_hz applies live while disarmed. The other fields take effect after Save and a reboot.")
                        font: Theme.fontCaption; color: Theme.textSecondary
                        wrapMode: Text.Wrap
                    }
                    ListView {
                        id: paramList
                        objectName: "paramList"
                        x: Theme.space24
                        y: (paramsHint.visible ? paramsHint.y + paramsHint.height : paramsTitle.y + paramsTitle.height) + Theme.space16
                        width: parent.width - 2 * Theme.space24; height: parent.height - y - Theme.space16
                        clip: true
                        spacing: Theme.space8
                        model: page.backend.params
                        section.property: "group"
                        section.delegate: Text {
                            required property string section
                            height: section.length > 0 ? 28 : 0
                            visible: section.length > 0
                            verticalAlignment: Text.AlignBottom
                            text: section.toUpperCase()
                            font: Theme.fontLabelCaps; color: Theme.textSecondary
                        }
                        delegate: DLParamRow {
                            id: paramRow
                            required property string key
                            required name
                            required unit
                            required description
                            required editor
                            required valueText
                            required pendingText
                            required isPending
                            required unsaved
                            required locked
                            required readOnly
                            required rebootRequired
                            required rangeText
                            required enums
                            required valueBits
                            required editState
                            required editNote
                            required editText
                            objectName: "param_" + name
                            width: paramList.width
                            nameWidth: Math.min(220, width * 0.25)
                            editorWidth: Math.min(240, width * 0.3)
                            blockedReason: rebootRequired ? (page.info.blocked || "") : (page.info.paramBlocked || "")
                            onCommitText: function(text) { page.backend.setParamText(key, text) }
                            onCommitValue: function(bits) { page.backend.setParamValue(key, bits) }
                            onClearEdit: page.backend.clearParamEdit(key)
                            onCommittedByEnter: safetyKeys.editCommitted()
                            Connections {
                                target: safetyKeys
                                function onEscapeRequested() { if (paramRow.editing) paramRow.cancel() }
                            }
                        }
                    }
                }
            }

            Flickable {
                id: sideScroll
                objectName: "sideScroll"
                visible: body.hasSide
                anchors.right: parent.right
                width: page.sideWidth; height: body.height
                contentHeight: sideColumn.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
            Column {
                id: sideColumn
                width: page.sideWidth
                spacing: Theme.space24

                Card {
                    id: wiringCard
                    objectName: "wiringCard"
                    width: parent.width
                    visible: !page.isLoop && page.wiring.length > 0
                    title: qsTr("Wiring")
                    Text {
                        width: parent.width; wrapMode: Text.Wrap
                        text: qsTr("Reboot required: changes go to the pending layout. Only instances in the same loop can be wired.")
                        font: Theme.fontCaption; color: Theme.textSecondary
                    }
                    Repeater {
                        model: page.wiring
                        delegate: TargetRow { width: wiringCard.width - 2 * Theme.space24; backend: page.backend; blocked: page.info.blocked || "" }
                    }
                }
                Card {
                    id: statisticsCard
                    objectName: "statisticsCard"
                    width: parent.width
                    visible: page.isLoop
                    title: qsTr("Statistics · live")
                    Text {
                        objectName: "statisticsNote"
                        width: parent.width; wrapMode: Text.Wrap
                        visible: text.length > 0
                        text: page.live.note || ""
                        font: Theme.fontCaption; color: Theme.textSecondary
                    }
                    InfoRow { objectName: "statTicks"; labelWidth: 140; label: qsTr("Ticks"); value: page.statistics.ticks || "—" }
                    InfoRow { objectName: "statOverruns"; labelWidth: 140; label: qsTr("Overruns"); value: page.statistics.overruns || "—" }
                    InfoRow { objectName: "statLastRun"; labelWidth: 140; label: qsTr("Last run"); value: page.statistics.lastRun || "—" }
                    InfoRow { objectName: "statLongestRun"; labelWidth: 140; label: qsTr("Longest run"); value: page.statistics.longestRun || "—" }
                    InfoRow { objectName: "statBudget"; labelWidth: 140; label: qsTr("Budget"); value: page.statistics.budget || "—" }
                }
                Card {
                    id: signalsCard
                    objectName: "signalsCard"
                    width: parent.width
                    visible: !page.isLoop && page.liveSignals.length > 0
                    title: qsTr("Signals · live")
                    contentSpacing: Theme.space8
                    Text {
                        objectName: "signalsNote"
                        width: parent.width; wrapMode: Text.Wrap
                        visible: text.length > 0
                        text: page.live.note || ""
                        font: Theme.fontCaption; color: Theme.textSecondary
                    }
                    Row {
                        width: parent.width
                        Text { width: parent.width * 0.5; text: qsTr("SIGNAL"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
                        Text { width: parent.width * 0.3; horizontalAlignment: Text.AlignRight; text: qsTr("VALUE"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
                        Text { width: parent.width * 0.2; leftPadding: Theme.space8; text: qsTr("UNIT"); font: Theme.fontLabelCaps; color: Theme.textSecondary }
                    }
                    Repeater {
                        model: page.liveSignals
                        delegate: Row {
                            id: signalRow
                            required property var modelData
                            objectName: "signal_" + modelData.name
                            width: signalsCard.width - 2 * Theme.space24
                            Text {
                                width: signalRow.width * 0.5; elide: Text.ElideRight
                                text: signalRow.modelData.name
                                font: Theme.fontMonoSmall
                                color: signalRow.modelData.subscribed ? Theme.textPrimary : Theme.textTertiary
                            }
                            Text {
                                objectName: "value"
                                width: signalRow.width * 0.3; elide: Text.ElideRight
                                horizontalAlignment: Text.AlignRight
                                text: signalRow.modelData.value || "—"
                                font: Theme.fontBodyNumeric
                                color: signalRow.modelData.value ? Theme.textPrimary : Theme.textTertiary
                            }
                            Text {
                                width: signalRow.width * 0.2; elide: Text.ElideRight; leftPadding: Theme.space8
                                text: signalRow.modelData.unit
                                font: Theme.fontCaption; color: Theme.textSecondary
                            }
                        }
                    }
                    Text {
                        objectName: "signalsHidden"
                        width: parent.width; wrapMode: Text.Wrap
                        visible: (page.live.hidden || 0) > 0
                        text: qsTr("%n signal(s) not shown: one subscription group holds %1 entries", "", page.live.hidden || 0)
                              .arg(page.live.entries || 0)
                        font: Theme.fontCaption; color: Theme.textTertiary
                    }
                }
                Card {
                    id: actionsCard
                    objectName: "actionsCard"
                    width: parent.width
                    visible: !page.isLoop && page.actionCount > 0
                    title: qsTr("Actions")
                    Repeater {
                        id: actionsRepeater
                        model: page.backend.actions
                        delegate: ActionRow { width: actionsCard.width - 2 * Theme.space24; backend: page.backend }
                    }
                }
                Card {
                    id: membersCard
                    objectName: "membersCard"
                    width: parent.width
                    visible: page.isLoop
                    title: qsTr("Instances in this loop")
                    Text {
                        visible: page.members.length === 0
                        width: parent.width; wrapMode: Text.Wrap
                        text: qsTr("No instance runs in this loop.")
                        font: Theme.fontBodyDefault; color: Theme.textSecondary
                    }
                    Repeater {
                        model: page.members
                        delegate: DLTreeItem {
                            required property var modelData
                            width: membersCard.width - 2 * Theme.space24
                            depth: 1
                            label: modelData.label
                            typeName: modelData.typeName
                            healthText: modelData.healthText
                            health: modelData.health
                            pending: modelData.pending
                            onClicked: page.backend.selectNode(modelData.key)
                        }
                    }
                    Text {
                        visible: page.members.length > 0
                        width: parent.width; wrapMode: Text.Wrap
                        text: page.node.removeBlocked || ""
                        font: Theme.fontCaption; color: Theme.textSecondary
                    }
                }
            }
            }
        }
    }
}
