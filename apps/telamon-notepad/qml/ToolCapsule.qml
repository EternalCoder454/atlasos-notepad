// The tool capsule, for Markdown or (with `code`) for code files: a tall rounded strip of icon buttons floating at
// the editor's right edge. It stays quiet (faded) until the pointer comes
// within `nearDistance` of it or one of its menus is open. It acts through the
// window's actions, so the capsule, the menus and the shortcuts do the same
// thing, and its buttons never take the keyboard focus from the editor.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Telamon.Ui
import net.eterneon.telamon.notepad

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
    // A code file is open: the code tools show instead of the Markdown ones.
    property bool code: false
    // The code popover (language, indentation) is open.
    property bool popoverOpen: false
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
    // A button has the keyboard focus (Tab): full strength, like the pointer.
    property bool focusInside: false
    readonly property bool near: pointerNear || focusInside || lazyHeadingMenu.visible || lazyMoreMenu.visible || lazyCaseMenu.visible || popoverOpen
    readonly property real nearDistance: 80

    // The language and indentation button was pressed (the popover opens
    // beside it).
    signal codeSettingsRequested(Item button)
    // `force`: a menu item ran, so the editor takes the focus back; else it
    // does only when nothing else (the find bar) took it.
    signal editorFocusRequested(bool force)
    // Set when an item of one of the menus ran, until the menu closes.
    property bool menuTriggered: false

    readonly property real gap: Kirigami.Units.smallSpacing
    readonly property real pitch: buttonSize + gap
    readonly property int buttonCount: code ? 8 : 9
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
    // Only while shown: a hidden capsule's layout counts none of its items.
    function checkFullHeight() {
        if (visible && !scrolls && Math.abs(column.implicitHeight + inset * 2 - fullHeight) > 1) {
            console.warn("ToolCapsule: fullHeight", fullHeight, "differs from the layout's", column.implicitHeight + inset * 2);
        }
    }
    // Later: the layout has not polished yet when a size has just changed,
    // and a callLater would still run before it.
    Timer {
        id: heightCheck
        interval: 250
        onTriggered: capsule.checkFullHeight()
    }
    onFullHeightChanged: heightCheck.restart()
    onScrollsChanged: heightCheck.restart()
    onVisibleChanged: heightCheck.restart()
    onCodeChanged: heightCheck.restart()
    Component.onCompleted: heightCheck.restart()

    width: Math.round(Kirigami.Units.gridUnit * 2.2)
    height: scrolls ? shown * pitch - gap + edge * 2 : fullHeight
    anchors.right: parent ? parent.right : undefined
    anchors.rightMargin: edgeMargin
    y: Math.round(topInset + edgeMargin + Math.max(0, ((parent ? parent.height : 0) - topInset - edgeMargin * 2 - height) / 2))
    opacity: near ? 1 : 0.35
    Accessible.role: Accessible.ToolBar
    Accessible.name: capsule.code ? qsTr("Code tools") : qsTr("Markdown tools")

    Behavior on opacity {
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
        }
    }

    component Tool: SymbolButton {
        property string iconName
        // The MarkdownEditor.Format bit that makes it look on.
        property int bit: 0
        // For a toggle that is not a format bit (line numbers).
        property bool on: false
        // Which variant of the capsule shows it.
        property bool forCode: false
        visible: forCode === capsule.code
        // Not `action`: AbstractButton's own would trigger it a second time.
        property QQC2.Action command
        round: true
        keyboardFocus: true
        tipSide: "left"
        implicitWidth: capsule.buttonSize
        implicitHeight: capsule.buttonSize
        symbol: iconName
        checked: on || (bit !== 0 && (capsule.formats & bit) !== 0)
        text: command ? command.text.replace("&", "") : ""
        shortcutText: command ? App.shortcutText(command.keys ?? command.shortcut) : ""
        enabled: command ? command.enabled : false
        onClicked: command.trigger()
        Keys.onEscapePressed: capsule.editorFocusRequested(true) // the editor keeps Tab
    }
    component Divider: Rectangle {
        property bool forCode: false
        Layout.alignment: Qt.AlignHCenter
        visible: !capsule.scrolls && forCode === capsule.code
        Layout.topMargin: Kirigami.Units.smallSpacing
        Layout.bottomMargin: Kirigami.Units.smallSpacing
        implicitWidth: Math.round(capsule.buttonSize * 0.55)
        implicitHeight: 1
        color: TelamonStyle.separator
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
    property Item menuOpener: null
    // A menu closed: an item ran (the editor takes the focus), else the
    // button that had the focus gets it back, else as for a click.
    function menuClosed() {
        const opener = menuOpener;
        const ran = menuTriggered;
        menuOpener = null;
        menuTriggered = false;
        if (!ran && opener && opener.visible && opener.enabled) {
            // The popup gives its own focus back (as a popup reason, which
            // SymbolButton still draws as a ring); if it didn't, take it.
            Qt.callLater(() => {
                if (!opener.activeFocus) {
                    opener.forceActiveFocus(Qt.TabFocusReason);
                }
            });
        } else {
            editorFocusRequested(ran);
        }
    }
    // Opens `menu` to the left of `button`, clear of the capsule.
    function popupLeft(menu, button) {
        // A menu opened from the keyboard returns to its button when it
        // closes unused; one opened by a click behaves as before.
        menuOpener = button.activeFocus ? button : null;
        menu.popup(button, 0, 0);
        // Bound: the menu's width settles after it opens.
        menu.x = Qt.binding(() => -menu.width - Kirigami.Units.smallSpacing);
    }

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: TelamonStyle.chromeBackground
        border.width: 1
        border.color: TelamonStyle.controlBorder
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
                keyboardFocus: true
                Layout.alignment: Qt.AlignHCenter
                round: true
                tipSide: "left"
                implicitWidth: capsule.buttonSize
                implicitHeight: capsule.buttonSize
                symbol: "format_h1"
                visible: !capsule.code
                Keys.onEscapePressed: capsule.editorFocusRequested(true)
                text: qsTr("Heading")
                tipEnabled: !lazyHeadingMenu.visible
                checked: capsule.heading > 0
                enabled: capsule.actions.heading1.enabled
                onClicked: {
                    capsule.popupLeft(lazyHeadingMenu.get(), headingButton);
                }
                Lazy {
                    id: lazyHeadingMenu
                    host: headingButton
                    component: Component {
                        ContextMenu {
                            id: headingMenu
                            onClosed: capsule.menuClosed()
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
                keyboardFocus: true
                Layout.alignment: Qt.AlignHCenter
                round: true
                tipSide: "left"
                implicitWidth: capsule.buttonSize
                implicitHeight: capsule.buttonSize
                symbol: "add"
                visible: !capsule.code
                Keys.onEscapePressed: capsule.editorFocusRequested(true)
                text: qsTr("More")
                tipEnabled: !lazyMoreMenu.visible
                onClicked: capsule.popupLeft(lazyMoreMenu.get(), moreButton)
                Lazy {
                    id: lazyMoreMenu
                    host: moreButton
                    component: Component {
                        ContextMenu {
                            id: moreMenu
                            onClosed: capsule.menuClosed()
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
            }

            // Code tools.
            Tool {
                forCode: true
                Layout.alignment: Qt.AlignHCenter
                iconName: "comment"
                command: capsule.actions.toggleComment
            }
            Tool {
                forCode: true
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_indent_increase"
                command: capsule.actions.indent
            }
            Tool {
                forCode: true
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_indent_decrease"
                command: capsule.actions.outdent
            }
            Divider {
                forCode: true
            }
            Tool {
                forCode: true
                Layout.alignment: Qt.AlignHCenter
                iconName: "sort"
                command: capsule.actions.sortLines
            }
            SymbolButton {
                id: caseButton
                keyboardFocus: true
                visible: capsule.code
                Layout.alignment: Qt.AlignHCenter
                round: true
                tipSide: "left"
                implicitWidth: capsule.buttonSize
                implicitHeight: capsule.buttonSize
                symbol: "text_fields"
                Keys.onEscapePressed: capsule.editorFocusRequested(true)
                text: qsTr("Change Case")
                tipEnabled: !lazyCaseMenu.visible
                enabled: capsule.actions.upperCase.enabled
                onClicked: capsule.popupLeft(lazyCaseMenu.get(), caseButton)
                Lazy {
                    id: lazyCaseMenu
                    host: caseButton
                    component: Component {
                        ContextMenu {
                            id: caseMenu
                            onClosed: capsule.menuClosed()
                            Entry {
                                command: capsule.actions.upperCase
                            }
                            Entry {
                                command: capsule.actions.lowerCase
                            }
                            Entry {
                                command: capsule.actions.titleCase
                            }
                        }
                    }
                }
            }
            Tool {
                forCode: true
                Layout.alignment: Qt.AlignHCenter
                iconName: "space_bar"
                command: capsule.actions.trimSpaces
            }
            Divider {
                forCode: true
            }
            Tool {
                forCode: true
                Layout.alignment: Qt.AlignHCenter
                iconName: "format_list_numbered"
                command: capsule.actions.codeLineNumbers
                on: capsule.actions.codeLineNumbers.checked
            }
            SymbolButton {
                id: languageButton
                keyboardFocus: true
                visible: capsule.code
                Layout.alignment: Qt.AlignHCenter
                round: true
                tipSide: "left"
                implicitWidth: capsule.buttonSize
                implicitHeight: capsule.buttonSize
                symbol: "data_object"
                Keys.onEscapePressed: capsule.editorFocusRequested(true)
                text: qsTr("Language and Indentation")
                tipEnabled: !capsule.popoverOpen
                checked: capsule.popoverOpen
                onClicked: capsule.codeSettingsRequested(languageButton)
            }
        }
    }

    // A mouse notch is 120 units and one button; a touchpad sends small
    // deltas, which add up to a button each 120 and are forgotten after a
    // pause. On the capsule, so the margins with the chevrons take it too.
    property real carried: 0
    Timer {
        id: wheelIdle
        interval: 300
        onTriggered: capsule.carried = 0
    }
    WheelHandler {
        enabled: capsule.scrolls
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            const dy = event.angleDelta.y;
            if (dy === 0) {
                return;
            }
            if (capsule.carried * dy < 0 || event.phase === Qt.ScrollBegin) {
                capsule.carried = 0; // the other way, or a new swipe
            }
            capsule.carried += dy;
            wheelIdle.restart();
            const steps = Math.trunc(capsule.carried / 120);
            if (steps !== 0) {
                capsule.carried -= steps * 120;
                flick.scrollBy(-steps);
            }
        }
    }

    // Tab onto a button that is scrolled out: the strip follows, a whole
    // button at a time.
    function reveal(item) {
        if (!scrolls) {
            return;
        }
        const i = Math.round(item.mapToItem(column, 0, 0).y / pitch);
        const first = Math.round(flick.contentY / pitch);
        if (i < first) {
            flick.scrollBy(i - first);
        } else if (i > first + shown - 1) {
            flick.scrollBy(i - (first + shown - 1));
        }
    }
    // A new window: what the old one had focused says nothing now.
    readonly property var window: Window.window
    onWindowChanged: focusInside = false
    Connections {
        target: capsule.window
        function onActiveFocusItemChanged() {
            const window = capsule.window;
            let item = window.activeFocusItem;
            const focused = item;
            while (item && item !== column) {
                item = item.parent;
            }
            const was = capsule.focusInside;
            capsule.focusInside = !!(item && focused);
            if (capsule.focusInside) {
                capsule.reveal(focused);
            } else if (was && (!focused || focused === window.contentItem)) {
                // The focused button went disabled or hidden: focus fell to
                // nothing, so typing would go nowhere.
                Qt.callLater(() => capsule.editorFocusRequested(false));
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
        // The tap area reaches out past the capsule's end, by no more than
        // the margin kept around it, so it never covers a button, the tab
        // bar or the text.
        readonly property real reach: Math.min(Math.round(capsule.buttonSize * 0.4), capsule.edgeMargin)
        height: capsule.edge + reach
        anchors.topMargin: atTop ? -reach : 0
        anchors.bottomMargin: atTop ? 0 : -reach
        visible: capsule.scrolls && (atTop ? flick.contentY > 1 : flick.contentY < flick.contentHeight - flick.height - 1)
        Accessible.ignored: true

        Symbol {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: hint.atTop ? parent.top : undefined
            anchors.bottom: hint.atTop ? undefined : parent.bottom
            anchors.topMargin: hint.atTop ? hint.reach + 1 : 0
            anchors.bottomMargin: hint.atTop ? 0 : hint.reach + 1
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
