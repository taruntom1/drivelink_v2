import QtQuick
import QtQuick.Templates as T

// Figma ProgressBar: a 4 px track with the accent fill; indeterminate shows
// a sliding segment.
T.ProgressBar {
    id: control

    implicitWidth: 240
    implicitHeight: 4

    background: Rectangle {
        implicitHeight: 4
        radius: 2
        color: Theme.bgSecondary
    }
    contentItem: Item {
        clip: true
        Rectangle {
            visible: !control.indeterminate
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: Theme.accentDefault
        }
        Rectangle {
            id: segment
            visible: control.indeterminate
            width: parent.width / 3
            height: parent.height
            radius: 2
            color: Theme.accentDefault
            NumberAnimation on x {
                running: control.indeterminate && control.visible
                from: -segment.width; to: control.width
                duration: 1200; loops: Animation.Infinite
            }
        }
    }
}
