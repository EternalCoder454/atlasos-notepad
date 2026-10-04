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
    // MarkdownEditor.Format bits at the caret or selection.
    required property int formats
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

    // `force`: a menu item ran, so the editor takes the focus back; else it
    // does only when nothing else (the find bar) took it.
    signal editorFocusRequested(bool force)
    // Set when an item of one of the menus ran, until the menu closes.
    property bool menuTriggered: false

    readonly property real gap: Kirigami.Units.smallSpacing
    readonly property real pitch: buttonSize + gap
    readonly property int buttonCount: 9
    // The height with everything showing, from sizes alone (the two dividers
    // are 1px with a gap above and below).
    readonly property real fullHeight: buttonCount * buttonSize + (buttonCount - 1) * gap + 2 * (1 + 3 * gap) + inset * 2
    readonly property real available: Math.max(buttonSize + inset * 2, (parent ? parent.height : 0) - topInset - edgeMargin * 2)
    // Too short for everything: the dividers go, the buttons are a plain
    // column of equal steps, and it shows whole buttons that scroll one
    // button at a time, with a chevron in the room kept at each end.
    readonly property bool scrolls: fullHeight > available
    readonly property real edge: scrolls ? Math.round(buttonSize * 0.45) : inset
    readonly property int shown: Math.max(1, Math.floor((available - edge * 2 + gap) / pitch))

    // fullHeight is worked out by hand: when everything shows, the layout
    // must agree.
    function checkFullHeight() {
        if (!scrolls && Math.abs(column.implicitHeight + inset * 2 - fullHeight) > 1) {
            console.warn("ToolCapsule: fullHeight", fullHeight, "differs from the layout's", column.implicitHeight + inset * 2);
        }
    }
    onFullHeightChanged: checkFullHeight()
    Component.onCompleted: checkFullHeight()

    width: Math.round(Kirigami.Units.gridUnit * 2.2)
    height: scrolls ? shown * pitch - gap + edge * 2 : fullHeight
    anchors.right: parent ? parent.right : undefined
    anchors.rightMargin: edgeMargin
    y: Math.round(topInset + edgeMargin + Math.max(0, ((parent ? parent.height : 0) - topInset - edgeMargin * 2 - height) / 2))
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
        // The MarkdownEditor.Format bit that makes it look on.
        property int bit: 0
        // Not `action`: AbstractButton's own would trigger it a second time.
        property QQC2.Action command
        round: true
        tipSide: "left"
        implicitWidth: capsule.buttonSize
        implicitHeight: capsule.buttonSize
        symbol: iconName
        checked: bit !== 0 && (capsule.formats & bit) !== 0
        text: command ? command.text.replace("&", "") : ""
        shortcutText: command ? App.shortcutText(command.keys ?? command.shortcut) : ""
        enabled: command ? command.enabled : false
        onClicked: command.trigger()
    }
    component Divider: Rectangle {
        Layout.alignment: Qt.AlignHCenter
        visible: !capsule.scrolls
        Layout.topMargin: Kirigami.Units.smallSpacing
        Layout.bottomMargin: Kirigami.Units.smallSpacing
        implicitWidth: Math.round(capsule.buttonSize * 0.55)
        implicitHeight: 1
        color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
    }
    // A menu item that runs a window action. Not `action:`, so `checked` is
    // ours (the action's own is bound to the setting it toggles). With
    // `showMark` it has a check mark that follows `marked`; a click would
    // toggle it and end that binding, so it is bound again on every run.
    component Entry: ContextMenuItem {
        id: entry
        property QQC2.Action command
        property bool showMark: false
        property bool marked: false
        checkable: showMark
        checked: marked
        text: command ? command.text.replace("&", "") : ""
        shortcutText: command ? App.shortcutText(command.keys ?? command.shortcut) : ""
        enabled: command ? command.enabled : false
        onTriggered: {
            capsule.menuTriggered = true;
            entry.checked = Qt.binding(() => entry.marked);
            if (command) {
                command.trigger();
            }
        }
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
        anchors.topMargin: capsule.edge
        anchors.bottomMargin: capsule.edge
        contentWidth: width
        contentHeight: column.implicitHeight
        // One button at a time: contentY is a whole number of steps.
        function scrollBy(steps) {
            const max = Math.max(0, contentHeight - height);
            contentY = Math.max(0, Math.min(max, (Math.round(contentY / capsule.pitch) + steps) * capsule.pitch));
        }
        onHeightChanged: scrollBy(0)
        clip: true
        // Not draggable: a drag would stop between buttons, and a press on
        // a button would turn into a flick. The wheel and the chevrons scroll.
        interactive: false
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: column
            width: flick.width
            spacing: Kirigami.Units.smallSpacing

            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_bold"
                command: capsule.actions.bold
                bit: MarkdownEditor.FmtBold
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_italic"
                command: capsule.actions.italic
                bit: MarkdownEditor.FmtItalic
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "code"
                command: capsule.actions.code
                bit: MarkdownEditor.FmtCode
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
                checked: capsule.heading > 0
                enabled: capsule.actions.heading1.enabled
                onClicked: {
                    capsule.popupLeft(headingMenu, headingButton);
                }
                ContextMenu {
                    id: headingMenu
                    onClosed: {
                        capsule.editorFocusRequested(capsule.menuTriggered);
                        capsule.menuTriggered = false;
                    }
                    Entry {
                        command: capsule.actions.heading1
                        showMark: true
                        marked: capsule.heading === 1
                    }
                    Entry {
                        command: capsule.actions.heading2
                        showMark: true
                        marked: capsule.heading === 2
                    }
                    Entry {
                        command: capsule.actions.bodyText
                        showMark: true
                        marked: capsule.heading === 0
                    }
                }
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_list_bulleted"
                command: capsule.actions.bulletList
                bit: MarkdownEditor.FmtBullet
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "checklist"
                command: capsule.actions.checklist
                bit: MarkdownEditor.FmtTask
            }
            Tool {
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_quote"
                command: capsule.actions.quote
                bit: MarkdownEditor.FmtQuote
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
                    onClosed: {
                        capsule.editorFocusRequested(capsule.menuTriggered);
                        capsule.menuTriggered = false;
                    }
                    Entry {
                        command: capsule.actions.strikethrough
                    }
                    Entry {
                        command: capsule.actions.numberedList
                    }
                    ContextMenuSeparator {}
                    Entry {
                        command: capsule.actions.toggleFormatted
                        showMark: true
                        marked: !capsule.formatted
                    }
                }
            }
        }

        WheelHandler {
            enabled: capsule.scrolls
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            // A mouse notch is 120 units and one button; a touchpad sends
            // small deltas, which add up to a button each 120.
            property real carried: 0
            onWheel: event => {
                const dy = event.angleDelta.y;
                if (dy === 0) {
                    return;
                }
                if (carried * dy < 0) {
                    carried = 0; // the other way
                }
                carried += dy;
                const steps = Math.trunc(carried / 120);
                if (steps !== 0) {
                    carried -= steps * 120;
                    flick.scrollBy(-steps);
                }
            }
        }
    }

    // Past either end of a scrolling strip: a chevron in the room kept
    // there, which a tap or click also scrolls by (the strip is not
    // draggable, so this is how touch reaches the rest).
    component Hint: Item {
        id: hint
        property bool atTop: true
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: atTop ? parent.top : undefined
        anchors.bottom: atTop ? undefined : parent.bottom
        height: capsule.edge
        visible: capsule.scrolls && (atTop ? flick.contentY > 1 : flick.contentY < flick.contentHeight - flick.height - 1)
        Accessible.ignored: true

        Symbol {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: hint.atTop ? parent.top : undefined
            anchors.bottom: hint.atTop ? undefined : parent.bottom
            anchors.topMargin: hint.atTop ? 1 : 0
            anchors.bottomMargin: hint.atTop ? 0 : 1
            name: hint.atTop ? "keyboard_arrow_up" : "keyboard_arrow_down"
            // Inside the margin: it never covers the first or last button.
            size: Math.max(1, capsule.edge - 2)
            Accessible.ignored: true
            opacity: 0.7
        }
        TapHandler {
            onTapped: flick.scrollBy(hint.atTop ? -1 : 1)
        }
    }
    Hint {
        atTop: true
    }
    Hint {
        atTop: false
    }
}
