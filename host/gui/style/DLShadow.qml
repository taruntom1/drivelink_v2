import QtQuick

// Figma's Elevation/Popup and Elevation/Dialog drop shadows, drawn as stacked
// translucent rectangles so they render with every scene graph backend
// (the offscreen tests and screenshots use the software renderer, which has
// no shader effects). Put it first in a background item; it fills its parent.
Item {
    id: root

    // false: Elevation/Popup (menus, tooltips, toasts); true: Elevation/Dialog.
    property bool dialog: false
    property real radius: 0
    readonly property real blur: dialog ? Theme.elevationDialogBlur : Theme.elevationPopupBlur
    readonly property real offsetY: dialog ? Theme.elevationDialogOffsetY : Theme.elevationPopupOffsetY
    readonly property color color: dialog ? Theme.elevationDialogColor : Theme.elevationPopupColor
    readonly property int steps: 6

    anchors.fill: parent
    z: -1

    Repeater {
        model: root.steps

        Rectangle {
            required property int index
            readonly property real grow: root.blur * (index + 1) / (2 * root.steps)

            x: -grow
            y: root.offsetY - grow
            width: root.width + 2 * grow
            height: root.height + 2 * grow
            radius: root.radius + grow
            color: Qt.rgba(root.color.r, root.color.g, root.color.b, root.color.a / root.steps)
        }
    }
}
