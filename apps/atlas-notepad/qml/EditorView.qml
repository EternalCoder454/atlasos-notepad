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
    readonly property string selectedText: edit.selectedText

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
        if (edit.readOnly) {
            return;
        }
        if (edit.selectedText.length > 0) {
            edit.remove(edit.selectionStart, edit.selectionEnd);
        }
        edit.insert(edit.cursorPosition, text);
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

    Flickable {
        id: flick
        objectName: view.current ? "view" : ""
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: gutter.right
        anchors.right: parent.right
        contentWidth: edit.width
        contentHeight: edit.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: view.settings.wordWrap ? Flickable.VerticalFlick : Flickable.HorizontalAndVerticalFlick
        // Wheel scrolling only: drags select text.
        interactive: false

        function ensureVisible(r) {
            if (contentY >= r.y) {
                contentY = r.y;
            } else if (contentY + height <= r.y + r.height) {
                contentY = r.y + r.height - height;
            }
            if (!view.settings.wordWrap) {
                if (contentX >= r.x) {
                    contentX = Math.max(0, r.x - Kirigami.Units.gridUnit);
                } else if (contentX + width <= r.x + r.width) {
                    contentX = r.x + r.width - width + Kirigami.Units.gridUnit;
                }
            }
        }

        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
        QQC2.ScrollBar.horizontal: QQC2.ScrollBar {
            visible: !view.settings.wordWrap
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
            height: Math.max(implicitHeight, flick.height)
            focus: true
            textFormat: TextEdit.PlainText
            wrapMode: view.settings.wordWrap ? TextEdit.Wrap : TextEdit.NoWrap
            readOnly: view.document.readOnly || view.document.loading
            selectByMouse: true
            persistentSelection: true
            // Four spaces, as Windows Notepad.
            tabStopDistance: fontInfo.advanceWidth(" ") * 4
            font: {
                const base = view.formatted ? Kirigami.Theme.defaultFont : view.settings.font;
                return Qt.font({
                    family: base.family,
                    styleName: view.formatted ? "" : view.settings.font.styleName,
                    pointSize: view.settings.font.pointSize * view.settings.zoom / 100
                });
            }
            color: Kirigami.Theme.textColor
            selectionColor: Kirigami.Theme.highlightColor
            selectedTextColor: Kirigami.Theme.highlightedTextColor
            leftPadding: view.settings.lineNumbers ? Kirigami.Units.largeSpacing : Kirigami.Units.gridUnit
            rightPadding: Kirigami.Units.gridUnit
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing
            onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
            onCursorPositionChanged: view.saveViewState()

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
                    if (url.length > 0) {
                        Qt.openUrlExternally(url);
                    }
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
                    contextMenu.popup();
                }
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

    MarkdownEditor {
        id: md
        textEdit: view.document.markdown ? edit : null
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

        ContextMenuItem {
            visible: contextMenu.link.length > 0
            text: qsTr("Open Link")
            icon.name: "internet-services"
            onTriggered: Qt.openUrlExternally(contextMenu.link)
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
            enabled: !edit.readOnly && edit.selectedText.length > 0
            onTriggered: edit.cut()
        }
        ContextMenuItem {
            text: qsTr("Copy")
            icon.name: "edit-copy"
            shortcutText: "Ctrl+C"
            enabled: edit.selectedText.length > 0
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
            enabled: !edit.readOnly && edit.selectedText.length > 0
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
            icon.source: Qt.resolvedUrl("../icons/link-add.svg")
            shortcutText: "Ctrl+K"
            onTriggered: view.linkRequested()
        }
        ContextMenuItem {
            visible: view.document.markdown
            enabled: !edit.readOnly && edit.selectedText.length > 0
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
