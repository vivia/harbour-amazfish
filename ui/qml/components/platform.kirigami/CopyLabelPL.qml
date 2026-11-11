import QtQuick 2.9
import QtQuick.Controls 2.2

// A label that would copy its own text when tapped. Plain QtQuick has no
// clipboard access, so copyable stays false and pages hide the tap hint.
Label {
    id: label

    property bool copyable: false
    signal copied

    anchors.left: parent.left
    anchors.right: parent.right
    wrapMode: Text.WordWrap
    font.underline: copyable
}
