// A small icon button like Atlas.Ui's ToolbarButton, drawn with a Material
// Symbol so the glyph is always the theme's text colour (an SVG used as a mask
// came out black in dark mode). It never takes the keyboard focus from the
// editor. `checkable` makes it a toggle with the accent tint. The tooltip is
// the text plus the optional `shortcutText`.
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Atlas.Ui

T.AbstractButton {
    id: control

    // Google's name for the symbol ("format_bold").
    property string symbol
    // The shortcut as shown to people, such as "Ctrl+B".
    property string shortcutText
    // A circle instead of a rounded square (the tool capsule).
    property bool round: false
    // Where the tooltip goes: "bottom" or "left".
    property string tipSide: "bottom"
    // False while something else (a menu) shows from the button.
    property bool tipEnabled: true

    implicitHeight: Math.round(Kirigami.Units.gridUnit * 1.5)
    implicitWidth: implicitHeight
    display: T.AbstractButton.IconOnly
    hoverEnabled: true
    // True: Tab reaches it (a click still never does).
    property bool keyboardFocus: false
    focusPolicy: keyboardFocus ? Qt.TabFocus : Qt.NoFocus
    Accessible.role: Accessible.Button
    Accessible.name: control.text
    Accessible.description: control.shortcutText
    Accessible.checkable: control.checkable
    Accessible.checked: control.checked

    QQC2.ToolTip {
        parent: control
        visible: control.tipEnabled && control.hovered && !control.down && control.text.length > 0
        delay: Kirigami.Units.toolTipDelay
        text: control.shortcutText.length > 0 ? qsTr("%1 (%2)").arg(control.text).arg(control.shortcutText) : control.text
        x: control.tipSide === "left" ? -width - Kirigami.Units.smallSpacing : Math.round((control.width - width) / 2)
        y: control.tipSide === "left" ? Math.round((control.height - height) / 2) : control.height + Kirigami.Units.smallSpacing
    }

    background: Rectangle {
        radius: control.round ? Math.min(width, height) / 2 : 6
        color: control.checked ? Qt.alpha(Kirigami.Theme.highlightColor, control.down ? 0.28 : 0.18) : Qt.alpha(Kirigami.Theme.textColor, control.down ? 0.12 : control.hovered ? 0.07 : 0)
        border.width: control.visualFocus ? 2 : 0
        border.color: Kirigami.Theme.highlightColor
        Behavior on color {
            ColorAnimation {
                duration: Kirigami.Units.shortDuration
            }
        }
    }

    contentItem: Item {
        Symbol {
            anchors.centerIn: parent
            name: control.symbol
            size: Kirigami.Units.iconSizes.smallMedium
            opacity: control.enabled ? 1 : 0.4
        }
    }
}
