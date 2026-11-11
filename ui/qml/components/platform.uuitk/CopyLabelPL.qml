import QtQuick 2.0
import QtQuick.Controls 2.2

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
            clipboard.copy(label.text)
            label.copied()
        }
    }

    ClipboardPL {
        id: clipboard
    }
}
