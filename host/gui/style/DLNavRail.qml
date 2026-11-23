import QtQuick

// Figma NavRail: the page items at the top, actions and Settings at the
// bottom. Controlled: a click emits activated(index) and the shell sets
// currentIndex. Footer page items are indexed after the top items. The page items
// and the footer items are enabled separately.
Rectangle {
    id: rail

    // [{ text, icon, objectName }]
    property var model: []
    property var footerModel: []
    property alias footerActions: actions.data
    property int currentIndex: 0
    property bool pagesEnabled: true
    property bool footerEnabled: true

    signal activated(int index)

    implicitWidth: Theme.sizeNavrail
    implicitHeight: 640
    color: Theme.bgSurface

    Rectangle {
        anchors.right: parent.right
        width: Theme.borderWidth
        height: parent.height
        color: Theme.borderDefault
    }

    Column {
        anchors.top: parent.top
        anchors.topMargin: Theme.space8
        spacing: Theme.space4

        Repeater {
            model: rail.model

            DLNavItem {
                required property var modelData
                required property int index
                objectName: modelData.objectName || ""
                enabled: rail.pagesEnabled
                text: modelData.text
                iconName: modelData.icon
                checked: rail.currentIndex === index
                onClicked: rail.activated(index)
            }
        }
    }

    Column {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space8
        spacing: Theme.space4

        Column {
            id: actions
            spacing: Theme.space4
            enabled: rail.footerEnabled
        }

        Repeater {
            model: rail.footerModel

            DLNavItem {
                required property var modelData
                required property int index
                objectName: modelData.objectName || ""
                enabled: rail.footerEnabled
                text: modelData.text
                iconName: modelData.icon
                checked: rail.currentIndex === rail.model.length + index
                onClicked: rail.activated(rail.model.length + index)
            }
        }
    }
}
