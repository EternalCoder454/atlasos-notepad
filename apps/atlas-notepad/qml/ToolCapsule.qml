// The Markdown tool capsule: a tall rounded strip of icon buttons floating at
// the editor's right edge. It stays quiet (faded) until the pointer comes
// within `nearDistance` of it or one of its menus is open. It acts through the
// window's actions, so the capsule, the menus and the shortcuts do the same
// thing, and its buttons never take the keyboard focus from the editor.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Atlas.Ui
import net.eterneon.atlas.notepad

Item {
    id: capsule

    // The window's actions, by name (Main.qml `actions`).
    required property var actions
    // The caret line's heading level (0 for body text).
    required property int heading
    // The Formatted view is shown (not the Markdown syntax).
    required property bool formatted
    // Room kept free above (a find bar), in pixels.
    property real topInset: 0

    readonly property real edgeMargin: Math.round(Kirigami.Units.smallSpacing * 2)
    readonly property real buttonSize: Math.round(Kirigami.Units.gridUnit * 1.4)
    readonly property real inset: Math.round(Kirigami.Units.smallSpacing * 1.5)
    // Room the editor keeps free at its right edge.
    readonly property real reserve: width + edgeMargin * 2
    // Whether the pointer is within `nearDistance` of the capsule: the parent
    // watches it, as a handler on the capsule would only see the capsule.
    property bool pointerNear: false
    readonly property bool near: pointerNear || headingMenu.visible || moreMenu.visible
    readonly property real nearDistance: 80

    signal editorFocusRequested

    width: Math.round(Kirigami.Units.gridUnit * 2.2)
    height: Math.min(column.implicitHeight + inset * 2, (parent ? parent.height : 0) - topInset - edgeMargin * 2)
    anchors.right: parent ? parent.right : undefined
    anchors.rightMargin: edgeMargin
    y: Math.round(topInset + edgeMargin + Math.max(0, (parent.height - topInset - edgeMargin * 2 - height) / 2))
    opacity: near ? 1 : 0.35
    Accessible.role: Accessible.ToolBar
    Accessible.name: qsTr("Markdown tools")

    Behavior on opacity {
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
        }
    }

    component Tool: SymbolButton {
        property string iconName
        // Not `action`: AbstractButton's own would trigger it a second time.
        property QQC2.Action command
        round: true
        tipSide: "left"
        implicitWidth: capsule.buttonSize
        implicitHeight: capsule.buttonSize
        symbol: iconName
        text: command ? command.text.replace("&", "") : ""
        shortcutText: command ? App.shortcutText(command.keys ?? command.shortcut) : ""
        enabled: command ? command.enabled : false
        onClicked: command.trigger()
    }
    component Divider: Rectangle {
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: Kirigami.Units.smallSpacing
        Layout.bottomMargin: Kirigami.Units.smallSpacing
        implicitWidth: Math.round(capsule.buttonSize * 0.55)
        implicitHeight: 1
        color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
    }
    // A menu item that runs a window action. Not `action:`, so `checked` is
    // ours (the action's own is bound to the setting it toggles).
    component Entry: ContextMenuItem {
        property QQC2.Action command
        text: command ? command.text.replace("&", "") : ""
        shortcutText: command ? App.shortcutText(command.keys ?? command.shortcut) : ""
        enabled: command ? command.enabled : false
        onTriggered: command.trigger()
    }
    // Opens `menu` to the left of `button`, clear of the capsule.
    function popupLeft(menu, button) {
        menu.popup(button, 0, 0);
        // Bound: the menu's width settles after it opens.
        menu.x = Qt.binding(() => -menu.width - Kirigami.Units.smallSpacing);
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.18)
    }

    // Past the window's height the strip scrolls with the wheel.
    Flickable {
        id: flick
        anchors.fill: parent
        anchors.topMargin: capsule.inset
        anchors.bottomMargin: capsule.inset
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: column
            width: flick.width
            spacing: Kirigami.Units.smallSpacing

            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_bold"
                command: capsule.actions.bold
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_italic"
                command: capsule.actions.italic
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "code"
                command: capsule.actions.code
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "add_link"
                command: capsule.actions.link
            }
            Divider {}
            SymbolButton {
                id: headingButton
                Layout.alignment: Qt.AlignHCenter
                round: true
                tipSide: "left"
                implicitWidth: capsule.buttonSize
                implicitHeight: capsule.buttonSize
                symbol: "format_h1"
                text: qsTr("Heading")
                tipEnabled: !headingMenu.visible
                checkable: true
                checked: capsule.heading > 0
                enabled: capsule.actions.heading1.enabled
                onClicked: {
                    headingButton.checked = Qt.binding(() => capsule.heading > 0);
                    capsule.popupLeft(headingMenu, headingButton);
                }
                ContextMenu {
                    id: headingMenu
                    onClosed: capsule.editorFocusRequested()
                    Entry {
                        command: capsule.actions.heading1
                        checkable: true
                        checked: capsule.heading === 1
                    }
                    Entry {
                        command: capsule.actions.heading2
                        checkable: true
                        checked: capsule.heading === 2
                    }
                    Entry {
                        command: capsule.actions.bodyText
                        checkable: true
                        checked: capsule.heading === 0
                    }
                }
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_list_bulleted"
                command: capsule.actions.bulletList
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "checklist"
                command: capsule.actions.checklist
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_quote"
                command: capsule.actions.quote
            }
            Divider {}
            SymbolButton {
                id: moreButton
                Layout.alignment: Qt.AlignHCenter
                round: true
                tipSide: "left"
                implicitWidth: capsule.buttonSize
                implicitHeight: capsule.buttonSize
                symbol: "add"
                text: qsTr("More")
                tipEnabled: !moreMenu.visible
                onClicked: capsule.popupLeft(moreMenu, moreButton)
                ContextMenu {
                    id: moreMenu
                    onClosed: capsule.editorFocusRequested()
                    Entry {
                        command: capsule.actions.strikethrough
                    }
                    Entry {
                        command: capsule.actions.numberedList
                    }
                    ContextMenuSeparator {}
                    Entry {
                        action: capsule.actions.toggleFormatted
                    }
                }
            }
        }

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: event => {
                const max = Math.max(0, flick.contentHeight - flick.height);
                flick.contentY = Math.max(0, Math.min(max, flick.contentY - event.angleDelta.y / 2));
            }
        }
    }
}
