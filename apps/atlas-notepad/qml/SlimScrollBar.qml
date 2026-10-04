// The slim overlay scrollbar Atlas.Ui pages use, for the editor.
import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

QQC2.ScrollBar {
    id: bar

    policy: QQC2.ScrollBar.AsNeeded
    implicitWidth: orientation === Qt.Vertical ? 10 : 0
    implicitHeight: orientation === Qt.Horizontal ? 10 : 0
    padding: 2
    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        radius: Math.min(width, height) / 2
        color: Qt.alpha(Kirigami.Theme.textColor, bar.pressed ? 0.45 : bar.hovered ? 0.35 : 0.22)
        opacity: bar.active || bar.hovered ? 1 : 0
        Behavior on opacity {
            NumberAnimation {
                duration: Kirigami.Units.longDuration
            }
        }
    }
    background: null
}
