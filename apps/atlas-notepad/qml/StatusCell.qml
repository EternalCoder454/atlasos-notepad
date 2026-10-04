// One cell of the status line: an optional symbol and a short text in the
// small font. With `clickable` it gets a hover background and emits
// clicked(); with a `menu` a click pops it up above the cell. It never takes
// the keyboard focus from the editor.
import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Atlas.Ui

T.AbstractButton {
    id: control

    property bool clickable: false
    property string toolTip
    // A Material Symbols name ("check", "cloud"), or empty for none.
    property string symbol
    // Opened above the cell on a click; leave unset for none.
    property QtObject menu: null
    // Set by StatusBar.refresh(); the slim line draws no separators.
    property bool leadingSeparator: false
    // The theme's small font, and never bigger than 85% of the body text.
    readonly property font smallFont: Qt.font({
        family: Kirigami.Theme.defaultFont.family,
        pointSize: Math.max(6, Math.min(Kirigami.Theme.smallFont.pointSize, Kirigami.Theme.defaultFont.pointSize * 0.85))
    })

    implicitWidth: row.implicitWidth + leftPadding + rightPadding
    implicitHeight: Math.round(Kirigami.Units.gridUnit * 1.2)
    leftPadding: Kirigami.Units.smallSpacing + 2
    rightPadding: leftPadding
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    Accessible.role: clickable ? Accessible.Button : Accessible.StaticText
    Accessible.name: control.text
    Accessible.description: control.toolTip

    onClicked: {
        if (control.clickable && control.menu) {
            control.menu.popup(control, control.mirrored ? control.width - control.menu.implicitWidth : 0, 0);
            // Wholly above the cell, so it never covers the cells beside it;
            // bound, as the menu's size settles after it opens.
            control.menu.y = Qt.binding(() => -control.menu.height - Kirigami.Units.smallSpacing);
        }
    }

    QQC2.ToolTip.visible: control.toolTip.length > 0 && control.hovered
    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
    QQC2.ToolTip.text: control.toolTip

    background: Rectangle {
        radius: 5
        color: Qt.alpha(Kirigami.Theme.textColor, !control.clickable ? 0 : control.down ? 0.14 : control.hovered ? 0.08 : 0)
    }

    FontMetrics {
        id: metrics
        font: control.smallFont
    }

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            anchors.centerIn: parent
            spacing: Kirigami.Units.smallSpacing
            Symbol {
                visible: control.symbol.length > 0
                name: control.symbol
                // In pixels: the height of a line of the small font.
                size: Math.round(metrics.height)
                opacity: 0.8
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                font: control.smallFont
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Kirigami.Theme.textColor
                opacity: 0.8
            }
        }
    }
}
