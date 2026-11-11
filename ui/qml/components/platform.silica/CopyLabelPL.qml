import QtQuick 2.0
import Sailfish.Silica 1.0

// A label that copies its own text when tapped.
Label {
    id: label

    property bool copyable: true
    signal copied

    anchors.left: parent.left
    anchors.right: parent.right
    wrapMode: Text.WordWrap
    font.underline: copyable

    MouseArea {
        anchors.fill: parent
        onClicked: {
            Clipboard.text = label.text
            label.copied()
        }
    }
}
