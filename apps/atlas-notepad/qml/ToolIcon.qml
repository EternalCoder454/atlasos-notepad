// A formatting toolbar button: an icon from icons/, its name as the tooltip.
// (The module keeps the folders: this file is at qml/, the icons at icons/.)
import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

QQC2.AbstractButton {
    id: control

    property string iconName

    implicitWidth: Kirigami.Units.iconSizes.small + Kirigami.Units.smallSpacing * 3
    implicitHeight: implicitWidth
    // Clicking a format keeps the caret in the text.
    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    Accessible.name: text

    QQC2.ToolTip.text: text
    QQC2.ToolTip.visible: hovered && text.length > 0
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay

    background: Rectangle {
        radius: 6
        color: control.down || control.checked ? Qt.alpha(Kirigami.Theme.textColor, 0.12)
             : control.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.07) : "transparent"
    }
    contentItem: Item {
        Kirigami.Icon {
            anchors.centerIn: parent
            width: Kirigami.Units.iconSizes.small
            height: width
            source: Qt.resolvedUrl("../icons/" + control.iconName + ".svg")
            isMask: true
            color: Kirigami.Theme.textColor
        }
    }
}
