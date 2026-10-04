// A two-way pill switch for the top bar ("Formatted | Syntax"): the chosen
// side has the accent tint, as the current tab does. The two sides are radio
// buttons to a screen reader. It never takes the keyboard focus from the
// editor (the shortcut for the same choice is on the action).
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Templates as T
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Item {
    id: pill

    // Index of the chosen side: 0 for first, 1 for second.
    property int currentIndex: 0
    // Each: { text, toolTip }.
    property var items: []
    property string shortcutText

    signal chosen(int index)

    implicitWidth: row.implicitWidth + 4
    implicitHeight: Math.round(Kirigami.Units.gridUnit * 1.4)
    Accessible.role: Accessible.RadioButton
    Accessible.ignored: true

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Qt.alpha(Kirigami.Theme.textColor, 0.07)
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 2
        spacing: 0

        Repeater {
            model: pill.items
            delegate: T.AbstractButton {
                id: side

                required property int index
                required property var modelData
                readonly property bool on: index === pill.currentIndex

                Layout.fillHeight: true
                implicitWidth: label.implicitWidth + Kirigami.Units.largeSpacing * 2
                text: modelData.text
                checkable: true
                checked: on
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.toolTip ?? modelData.text
                Accessible.checkable: true
                Accessible.checked: on
                Accessible.onPressAction: side.clicked()
                // A press on the chosen side keeps it chosen.
                onClicked: {
                    side.checked = Qt.binding(() => side.on);
                    if (!side.on) {
                        pill.chosen(side.index);
                    }
                }

                QQC2.ToolTip.visible: side.hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                QQC2.ToolTip.text: pill.shortcutText.length > 0 ? qsTr("%1 (%2)").arg(modelData.toolTip ?? modelData.text).arg(pill.shortcutText) : (modelData.toolTip ?? modelData.text)

                background: Rectangle {
                    radius: height / 2
                    color: side.on ? Qt.alpha(Kirigami.Theme.highlightColor, side.down ? 0.34 : 0.26) : Qt.alpha(Kirigami.Theme.textColor, side.down ? 0.12 : side.hovered ? 0.08 : 0)
                    Behavior on color {
                        ColorAnimation {
                            duration: Kirigami.Units.shortDuration
                        }
                    }
                }
                contentItem: Text {
                    id: label
                    text: side.text
                    font: Kirigami.Theme.smallFont
                    textFormat: Text.PlainText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: Kirigami.Theme.textColor
                    opacity: side.on ? 1 : 0.8
                }
            }
        }
    }
}
