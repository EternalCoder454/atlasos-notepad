// One tab's editor: the TextEdit in a Flickable, the line-number gutter, and
// for Markdown the MarkdownEditor and its decorations. Made the first time
// its tab is shown and kept after, so undo history and the layout survive
// switching tabs.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import Atlas.Ui
import net.eterneon.atlas.notepad

FocusScope {
    id: view

    required property Document document
    required property bool current

    readonly property alias edit: edit
    readonly property alias md: md
    readonly property alias flickable: flick
    readonly property Settings settings: App.settings
    readonly property bool formatted: document.markdown && document.formatted
    // The caret line's heading level, for the toolbar.
    readonly property int heading: {
        if (!document.markdown) {
            return 0;
        }
        edit.length; // re-read on edits too
        return md.headingAt(edit.cursorPosition);
    }

    readonly property real scrollbarWidth: 10 // SlimScrollBar.implicitWidth
    // Kept clear at the right edge (the window's tool capsule).
    property real rightInset: 0

    // Which formats the caret or selection is in (MarkdownEditor.Format
    // bits), for the tool capsule: read from the highlighter's layout.
    readonly property int formats: {
        if (!document.markdown) {
            return 0;
        }
        edit.length; // re-read on edits too
        return md.formatsAt(edit.selectionStart, edit.selectionEnd);
    }

    signal linkRequested

    function focusEditor() {
        edit.forceActiveFocus();
    }
    // The caret to the start of 1-based `line`, scrolled into view.
    function goToLine(line) {
        edit.cursorPosition = document.positionOfLine(line);
        focusEditor();
    }
    // Typed text: replaces the selection, one undo step.
    function insertText(text) {
        document.insertText(text);
    }
    // Web and mail links only (App.linkUrl says which).
    function openLink(url) {
        App.openLink(url);
    }
    function select(start, end) {
        edit.select(start, end);
    }

    // What the session keeps: the caret, the selection and the scroll.
    function saveViewState() {
        document.cursorPosition = edit.cursorPosition;
        document.selectionAnchor = edit.cursorPosition === edit.selectionStart ? edit.selectionEnd : edit.selectionStart;
        document.scrollY = flick.contentY;
    }
    // Once, when the text is in.
    property bool restored: false
    function restoreViewState() {
        if (restored || document.loading) {
            return;
        }
        restored = true;
        const length = edit.length;
        const position = Math.min(document.cursorPosition, length);
        const anchor = Math.min(document.selectionAnchor, length);
        if (anchor !== position) {
            edit.select(anchor, position);
        } else {
            edit.cursorPosition = position;
        }
        // After the layout has the text.
        Qt.callLater(() => flick.contentY = Math.max(0, Math.min(document.scrollY, flick.contentHeight - flick.height)));
    }

    Connections {
        target: view.document
        function onLoadingChanged() {
            view.restoreViewState();
        }
    }

    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    Rectangle {
        anchors.fill: parent
        color: Kirigami.Theme.backgroundColor
    }

    LineNumbers {
        id: gutter
        visible: view.settings.lineNumbers
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        width: visible ? implicitWidth : 0
        textEdit: edit
        flickable: flick
        font: edit.font
        padding: Kirigami.Units.largeSpacing
        color: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, 0.4)
        currentColor: Kirigami.Theme.textColor
    }

    // The wheel over the strip kept free for the capsule scrolls the text too.
    Item {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: view.rightInset
        WheelHandler {
            acceptedModifiers: Qt.NoModifier
            onWheel: event => {
                flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY - event.angleDelta.y / 2));
            }
        }
    }

    Flickable {
        id: flick
        objectName: view.current ? "view" : ""
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: gutter.right
        anchors.right: parent.right
        anchors.rightMargin: view.rightInset
        contentWidth: edit.width
        contentHeight: edit.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: view.settings.wordWrap ? Flickable.VerticalFlick : Flickable.HorizontalAndVerticalFlick
        // Wheel scrolling only: drags select text.
        interactive: false

        // Not interactive, so nothing returns it to bounds by itself: a
        // scroll made while the view was still small would leave the text
        // above the top once it grows.
        function clampScroll() {
            contentY = Math.max(0, Math.min(contentY, contentHeight - height));
            contentX = Math.max(0, Math.min(contentX, contentWidth - width));
        }
        onHeightChanged: clampScroll()
        onWidthChanged: clampScroll()
        onContentHeightChanged: clampScroll()
        onContentWidthChanged: clampScroll()

        function ensureVisible(r) {
            if (height <= 0 || width <= 0) {
                return; // not laid out yet (a hidden tab)
            }
            if (contentY >= r.y) {
                contentY = r.y;
            } else if (contentY + height <= r.y + r.height) {
                contentY = r.y + r.height - height; // clampScroll bounds it once contentHeight settles
            }
            if (!view.settings.wordWrap) {
                if (contentX >= r.x) {
                    contentX = Math.max(0, r.x - Kirigami.Units.gridUnit);
                } else if (contentX + width <= r.x + r.width) {
                    contentX = r.x + r.width - width + Kirigami.Units.gridUnit;
                }
            }
        }

        // Overlaid on the text, as in Atlas.Ui pages.
        QQC2.ScrollBar.vertical: SlimScrollBar {
            parent: flick.parent
            x: flick.x + flick.width - width
            y: flick.y
            height: flick.height
        }
        QQC2.ScrollBar.horizontal: SlimScrollBar {
            parent: flick.parent
            policy: view.settings.wordWrap ? QQC2.ScrollBar.AlwaysOff : QQC2.ScrollBar.AsNeeded
            x: flick.x
            y: flick.y + flick.height - height
            width: flick.width
        }

        // Ctrl+wheel zooms (every window: the zoom is a setting); the rest
        // scrolls.
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: event => {
                const steps = event.angleDelta.y / 120;
                view.settings.zoom = view.settings.zoom + Math.round(steps) * 10;
            }
        }

        TextEdit {
            id: edit
            objectName: view.current ? "editor" : ""
            width: view.settings.wordWrap ? flick.width : Math.max(flick.width, implicitWidth)
            focus: true
            textFormat: TextEdit.PlainText
            wrapMode: view.settings.wordWrap ? TextEdit.Wrap : TextEdit.NoWrap
            readOnly: view.document.readOnly || view.document.loading
            selectByMouse: true
            persistentSelection: true
            // Four spaces, as Windows Notepad.
            tabStopDistance: fontInfo.advanceWidth(" ") * 4
            // Set one by one: Qt.font({pointSize}) cuts a fractional size
            // (9 * 110% = 9.9) down to a whole one, so 110% looked like 100%.
            font.family: (view.formatted ? Kirigami.Theme.defaultFont : view.settings.font).family
            font.styleName: view.formatted ? "" : view.settings.font.styleName
            font.pointSize: view.settings.font.pointSize * view.settings.zoom / 100
            color: Kirigami.Theme.textColor
            selectionColor: Kirigami.Theme.highlightColor
            selectedTextColor: Kirigami.Theme.highlightedTextColor
            leftPadding: view.settings.lineNumbers ? Kirigami.Units.largeSpacing : Kirigami.Units.gridUnit
            // The capsule's margin already keeps text clear of the edge, but
            // not of the overlay scrollbar, which sits inside the view.
            rightPadding: view.rightInset > 0 ? scrollbarWidth + Kirigami.Units.smallSpacing : Kirigami.Units.gridUnit
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing
            onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
            onCursorPositionChanged: view.saveViewState()
            Accessible.role: Accessible.EditableText
            Accessible.name: view.document.title
            Accessible.multiLine: true

            // Delayed: the text's implicit height follows its width (wrap), and
            // a plain binding here loops with the layout.
            Binding {
                target: edit
                property: "height"
                value: Math.max(edit.implicitHeight, flick.height)
                delayed: true
            }

            FontMetrics {
                id: fontInfo
                font: edit.font
            }

            // Ctrl+click opens a link.
            TapHandler {
                acceptedModifiers: Qt.ControlModifier
                enabled: view.document.markdown
                onTapped: (point, button) => {
                    const url = md.linkAt(edit.positionAt(point.position.x, point.position.y));
                    view.openLink(url);
                }
            }
            // Over a link, where it really goes: the Formatted view hides the
            // target, and the text can name another site.
            HoverHandler {
                id: linkHover
                property string target
                property int at: -1
                function forget() {
                    target = "";
                    at = -1;
                }
                enabled: view.document.markdown
                onPointChanged: {
                    const position = edit.positionAt(point.position.x, point.position.y);
                    if (position !== at) {
                        at = position;
                        target = App.linkUrl(md.linkAt(position));
                    }
                }
                onHoveredChanged: {
                    if (!hovered) {
                        forget();
                    }
                }
            }
            // The text under the pointer changed: wait for it to move.
            Connections {
                target: edit
                function onTextChanged() {
                    linkHover.forget();
                }
            }
            Connections {
                target: flick
                function onContentYChanged() {
                    linkHover.forget();
                }
            }
            QQC2.ToolTip {
                x: linkHover.point.position.x
                y: linkHover.point.position.y + Kirigami.Units.gridUnit
                visible: linkHover.target.length > 0
                width: Math.min(implicitWidth, Kirigami.Units.gridUnit * 24)
                delay: Kirigami.Units.toolTipDelay
                // Plain text: the style's label would take a tag or "&" in
                // the URL as markup.
                contentItem: QQC2.Label {
                    text: qsTr("%1\nCtrl+click to open").arg(linkHover.target)
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
            }
            // The context menu.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: (point, button) => {
                    const position = edit.positionAt(point.position.x, point.position.y);
                    if (position < edit.selectionStart || position > edit.selectionEnd) {
                        edit.cursorPosition = position;
                    }
                    contextMenu.link = view.document.markdown ? md.linkAt(position) : "";
                    contextMenu.spelling = spell.wordAt(position);
                    contextMenu.popup(edit, point.position.x, point.position.y);
                }
            }

            // Over the text, the visible part only.
            SpellUnderlines {
                visible: spell.active
                spellChecker: spell
                y: flick.contentY
                width: edit.width
                height: flick.height
            }

            // Behind the text, over the visible part only.
            MarkdownDecorations {
                z: -1
                visible: view.document.markdown
                editor: md
                y: flick.contentY
                width: edit.width
                height: flick.height
            }
        }
        onContentYChanged: saveTimer.restart()
    }

    // The scroll position goes to the document a moment after scrolling stops.
    Timer {
        id: saveTimer
        interval: 300
        onTriggered: view.saveViewState()
    }

    SpellChecker {
        id: spell
        textEdit: edit
        active: view.settings.spellCheck && view.document.prose && !edit.readOnly
        plainText: !view.document.markdown
        underlineColor: Kirigami.Theme.negativeTextColor
    }

    MarkdownEditor {
        id: md
        textEdit: view.document.markdown ? edit : null
        spellChecker: spell
        formatted: view.document.formatted
        font: edit.font
        textColor: Kirigami.Theme.textColor
        dimColor: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, 0.55)
        linkColor: Kirigami.Theme.linkColor
        codeColor: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, 0.07)
        accentColor: Kirigami.Theme.highlightColor
    }

    ContextMenu {
        id: contextMenu

        property string link
        // The misspelled word under the click: {start, end, word, suggestions}.
        property var spelling: ({})
        readonly property bool misspelled: spelling.word !== undefined
        readonly property var suggestions: misspelled ? spelling.suggestions : []

        component Suggestion: ContextMenuItem {
            required property int index
            visible: contextMenu.suggestions.length > index
            text: visible ? contextMenu.suggestions[index] : ""
            onTriggered: spell.replace(contextMenu.spelling.start, contextMenu.spelling.end, contextMenu.spelling.word, text)
        }

        Suggestion { index: 0 }
        Suggestion { index: 1 }
        Suggestion { index: 2 }
        Suggestion { index: 3 }
        Suggestion { index: 4 }
        ContextMenuItem {
            visible: contextMenu.misspelled && contextMenu.suggestions.length === 0
            enabled: false
            text: qsTr("No Suggestions")
        }
        ContextMenuItem {
            visible: contextMenu.misspelled
            text: qsTr("Add to Dictionary")
            onTriggered: spell.addToDictionary(contextMenu.spelling.word)
        }
        ContextMenuItem {
            visible: contextMenu.misspelled
            text: qsTr("Ignore")
            onTriggered: spell.ignore(contextMenu.spelling.word)
        }
        ContextMenuSeparator {
            visible: contextMenu.misspelled
        }

        ContextMenuItem {
            visible: contextMenu.link.length > 0
            enabled: App.linkUrl(contextMenu.link).length > 0
            text: App.linkTarget(contextMenu.link).length > 0 ? qsTr("Open Link (%1)").arg(App.linkTarget(contextMenu.link)) : qsTr("Open Link")
            icon.name: "internet-services"
            onTriggered: view.openLink(contextMenu.link)
        }
        ContextMenuItem {
            visible: contextMenu.link.length > 0
            text: qsTr("Copy Link")
            icon.name: "edit-copy"
            onTriggered: App.copyToClipboard(contextMenu.link)
        }
        ContextMenuSeparator {
            visible: contextMenu.link.length > 0
        }
        ContextMenuItem {
            text: qsTr("Undo")
            icon.name: "edit-undo"
            shortcutText: "Ctrl+Z"
            enabled: edit.canUndo
            onTriggered: edit.undo()
        }
        ContextMenuItem {
            text: qsTr("Redo")
            icon.name: "edit-redo"
            shortcutText: "Ctrl+Y"
            enabled: edit.canRedo
            onTriggered: edit.redo()
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Cut")
            icon.name: "edit-cut"
            shortcutText: "Ctrl+X"
            enabled: !edit.readOnly && edit.selectionStart !== edit.selectionEnd
            onTriggered: edit.cut()
        }
        ContextMenuItem {
            text: qsTr("Copy")
            icon.name: "edit-copy"
            shortcutText: "Ctrl+C"
            enabled: edit.selectionStart !== edit.selectionEnd
            onTriggered: edit.copy()
        }
        ContextMenuItem {
            text: qsTr("Paste")
            icon.name: "edit-paste"
            shortcutText: "Ctrl+V"
            enabled: edit.canPaste
            onTriggered: edit.paste()
        }
        ContextMenuItem {
            text: qsTr("Delete")
            icon.name: "edit-delete"
            enabled: !edit.readOnly && edit.selectionStart !== edit.selectionEnd
            onTriggered: edit.remove(edit.selectionStart, edit.selectionEnd)
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            text: qsTr("Select All")
            icon.name: "edit-select-all"
            shortcutText: "Ctrl+A"
            onTriggered: edit.selectAll()
        }
        ContextMenuSeparator {
            visible: view.document.markdown
        }
        ContextMenuItem {
            visible: view.document.markdown
            enabled: !edit.readOnly
            text: qsTr("Link…")
            icon.name: "insert-link"
            shortcutText: "Ctrl+K"
            onTriggered: view.linkRequested()
        }
        ContextMenuItem {
            visible: view.document.markdown
            enabled: !edit.readOnly && edit.selectionStart !== edit.selectionEnd
            text: qsTr("Clear Formatting")
            icon.name: "edit-clear-all"
            shortcutText: "Ctrl+Space"
            onTriggered: md.clearFormatting()
        }
    }

    Component.onCompleted: {
        document.textEdit = edit;
        restoreViewState();
    }
    Component.onDestruction: saveViewState()
}
