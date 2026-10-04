// A Notepad window: tabs, the formatting toolbar for Markdown, the editors,
// find, banners and the status bar. Menus go to Plasma's global menu when
// there is one, else to a menu button by the tabs. The App (C++) makes each
// window with its own DocumentList and shows it.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Atlas.Ui
import net.eterneon.atlas.notepad

QQC2.ApplicationWindow {
    id: root

    required property DocumentList documents
    readonly property Document document: documents.current
    // The current tab's editor (EditorView sets it when shown).
    property EditorView view: null
    readonly property Settings settings: App.settings
    readonly property bool markdown: document !== null && document.markdown
    readonly property bool editable: view !== null && !view.edit.readOnly
    readonly property bool formatEnabled: markdown && editable && !settingsOpen
    property bool settingsOpen: false

    // Every action, by name, for the toolbar and the menus.
    readonly property var actions: ({
            newTab: newTabAction,
            newWindow: newWindowAction,
            open: openAction,
            save: saveAction,
            saveAs: saveAsAction,
            saveAll: saveAllAction,
            print: printAction,
            closeTab: closeTabAction,
            reopenTab: reopenTabAction,
            closeWindow: closeWindowAction,
            quit: quitAction,
            undo: undoAction,
            redo: redoAction,
            cut: cutAction,
            copy: copyAction,
            paste: pasteAction,
            delete: deleteAction,
            selectAll: selectAllAction,
            find: findAction,
            findNext: findNextAction,
            findPrevious: findPreviousAction,
            replace: replaceAction,
            goTo: goToAction,
            timeDate: timeDateAction,
            zoomIn: zoomInAction,
            zoomOut: zoomOutAction,
            zoomReset: zoomResetAction,
            wordWrap: wordWrapAction,
            lineNumbers: lineNumbersAction,
            statusBar: statusBarAction,
            toolbar: toolbarAction,
            toggleFormatted: toggleFormattedAction,
            settings: settingsAction,
            heading1: heading1Action,
            heading2: heading2Action,
            heading3: heading3Action,
            bodyText: bodyTextAction,
            bold: boldAction,
            italic: italicAction,
            strikethrough: strikethroughAction,
            code: codeAction,
            link: linkAction,
            clearFormatting: clearFormattingAction,
            bulletList: bulletListAction,
            numberedList: numberedListAction,
            checklist: checklistAction,
            quote: quoteAction
        })

    width: Kirigami.Units.gridUnit * 50
    height: Kirigami.Units.gridUnit * 36
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 12
    title: document ? qsTr("%1 — Notepad").arg(document.title) : qsTr("Notepad")
    color: Kirigami.Theme.backgroundColor

    // --- Actions. The menus, the toolbar and the shortcuts all use these.

    QQC2.Action {
        id: newTabAction
        text: qsTr("New Tab")
        shortcut: StandardKey.New
        onTriggered: root.documents.newTab()
    }
    QQC2.Action {
        id: newWindowAction
        text: qsTr("New Window")
        shortcut: "Ctrl+Shift+N"
        onTriggered: App.newWindow()
    }
    QQC2.Action {
        id: openAction
        text: qsTr("Open…")
        shortcut: StandardKey.Open
        onTriggered: openDialog.open()
    }
    QQC2.Action {
        id: saveAction
        text: qsTr("Save")
        shortcut: StandardKey.Save
        enabled: root.document !== null && !root.document.loading && !root.document.readOnly
        onTriggered: root.document.save()
    }
    QQC2.Action {
        id: saveAsAction
        text: qsTr("Save As…")
        shortcut: "Ctrl+Shift+S"
        enabled: root.document !== null && !root.document.loading && root.document.banner !== Document.TooLarge
        onTriggered: root.saveAs(root.document)
    }
    QQC2.Action {
        id: saveAllAction
        text: qsTr("Save All")
        shortcut: "Ctrl+Alt+S"
        enabled: root.documents.anyModified
        onTriggered: {
            for (const doc of root.documents.documents()) {
                if (doc.modified && doc.path.length > 0 && !doc.readOnly) {
                    doc.save();
                }
            }
        }
    }
    QQC2.Action {
        id: printAction
        text: qsTr("Print…")
        shortcut: StandardKey.Print
        enabled: root.document !== null && !root.document.loading
        onTriggered: App.print(root.document, root)
    }
    QQC2.Action {
        id: closeTabAction
        text: qsTr("Close Tab")
        shortcut: StandardKey.Close
        enabled: root.documents.count > 0
        onTriggered: root.closeTab(root.documents.currentIndex)
    }
    QQC2.Action {
        id: reopenTabAction
        text: qsTr("Reopen Closed Tab")
        shortcut: "Ctrl+Shift+T"
        enabled: root.documents.canReopenClosed
        onTriggered: root.documents.reopenClosed()
    }
    QQC2.Action {
        id: closeWindowAction
        text: qsTr("Close Window")
        shortcut: "Ctrl+Shift+W"
        onTriggered: root.close()
    }
    QQC2.Action {
        id: quitAction
        text: qsTr("Quit")
        shortcut: StandardKey.Quit
        onTriggered: App.quit()
    }

    QQC2.Action {
        id: undoAction
        text: qsTr("Undo")
        shortcut: StandardKey.Undo
        enabled: root.view !== null && root.view.edit.canUndo
        onTriggered: root.view.edit.undo()
    }
    QQC2.Action {
        id: redoAction
        text: qsTr("Redo")
        shortcut: "Ctrl+Y"
        enabled: root.view !== null && root.view.edit.canRedo
        onTriggered: root.view.edit.redo()
    }
    QQC2.Action {
        id: cutAction
        text: qsTr("Cut")
        shortcut: StandardKey.Cut
        enabled: root.editable && root.view.selectedText.length > 0
        onTriggered: root.view.edit.cut()
    }
    QQC2.Action {
        id: copyAction
        text: qsTr("Copy")
        shortcut: StandardKey.Copy
        enabled: root.view !== null && root.view.selectedText.length > 0
        onTriggered: root.view.edit.copy()
    }
    QQC2.Action {
        id: pasteAction
        text: qsTr("Paste")
        shortcut: StandardKey.Paste
        enabled: root.editable && root.view.edit.canPaste
        onTriggered: root.view.edit.paste()
    }
    QQC2.Action {
        id: deleteAction
        text: qsTr("Delete")
        enabled: root.editable && root.view.selectedText.length > 0
        onTriggered: root.view.edit.remove(root.view.edit.selectionStart, root.view.edit.selectionEnd)
    }
    QQC2.Action {
        id: selectAllAction
        text: qsTr("Select All")
        shortcut: StandardKey.SelectAll
        enabled: root.view !== null
        onTriggered: root.view.edit.selectAll()
    }
    QQC2.Action {
        id: findAction
        text: qsTr("Find…")
        shortcut: StandardKey.Find
        enabled: root.view !== null && !root.settingsOpen
        onTriggered: root.openFind(false)
    }
    QQC2.Action {
        id: findNextAction
        text: qsTr("Find Next")
        shortcut: StandardKey.FindNext
        enabled: root.view !== null && findBar.findText.length > 0
        onTriggered: root.find(false)
    }
    QQC2.Action {
        id: findPreviousAction
        text: qsTr("Find Previous")
        shortcut: StandardKey.FindPrevious
        enabled: root.view !== null && findBar.findText.length > 0
        onTriggered: root.find(true)
    }
    QQC2.Action {
        id: replaceAction
        text: qsTr("Replace…")
        shortcut: "Ctrl+H"
        enabled: root.editable && !root.settingsOpen
        onTriggered: root.openFind(true)
    }
    QQC2.Action {
        id: goToAction
        text: qsTr("Go To Line…")
        shortcut: "Ctrl+G"
        enabled: root.view !== null && !root.settingsOpen
        onTriggered: goToDialog.open()
    }
    QQC2.Action {
        id: timeDateAction
        text: qsTr("Time/Date")
        shortcut: "F5"
        enabled: root.editable
        onTriggered: root.view.insertText(App.timeDate())
    }

    QQC2.Action {
        id: zoomInAction
        text: qsTr("Zoom In")
        shortcut: StandardKey.ZoomIn
        enabled: root.settings.zoom < 400
        onTriggered: root.settings.zoom = root.settings.zoom + 10
    }
    QQC2.Action {
        id: zoomOutAction
        text: qsTr("Zoom Out")
        shortcut: StandardKey.ZoomOut
        enabled: root.settings.zoom > 50
        onTriggered: root.settings.zoom = root.settings.zoom - 10
    }
    QQC2.Action {
        id: zoomResetAction
        text: qsTr("Restore Default Zoom")
        shortcut: "Ctrl+0"
        enabled: root.settings.zoom !== 100
        onTriggered: root.settings.zoom = 100
    }
    QQC2.Action {
        id: wordWrapAction
        text: qsTr("Word Wrap")
        shortcut: "Alt+Z"
        checkable: true
        checked: root.settings.wordWrap
        onTriggered: root.settings.wordWrap = !root.settings.wordWrap
    }
    QQC2.Action {
        id: lineNumbersAction
        text: qsTr("Line Numbers")
        checkable: true
        checked: root.settings.lineNumbers
        onTriggered: root.settings.lineNumbers = !root.settings.lineNumbers
    }
    QQC2.Action {
        id: statusBarAction
        text: qsTr("Status Bar")
        checkable: true
        checked: root.settings.statusBar
        onTriggered: root.settings.statusBar = !root.settings.statusBar
    }
    QQC2.Action {
        id: toolbarAction
        text: qsTr("Formatting Toolbar")
        checkable: true
        checked: root.settings.formattingToolbar
        enabled: root.settings.formatting
        onTriggered: root.settings.formattingToolbar = !root.settings.formattingToolbar
    }
    QQC2.Action {
        id: toggleFormattedAction
        text: qsTr("Show Markdown Syntax")
        shortcut: "Ctrl+Shift+M"
        checkable: true
        checked: root.document !== null && !root.document.formatted
        enabled: root.markdown && !root.settingsOpen
        onTriggered: root.document.formatted = !root.document.formatted
    }
    QQC2.Action {
        id: settingsAction
        text: qsTr("Settings")
        shortcut: "Ctrl+,"
        onTriggered: root.settingsOpen = !root.settingsOpen
    }

    QQC2.Action {
        id: heading1Action
        text: qsTr("Heading 1")
        shortcut: "Ctrl+1"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 1 ? 0 : 1)
    }
    QQC2.Action {
        id: heading2Action
        text: qsTr("Heading 2")
        shortcut: "Ctrl+2"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 2 ? 0 : 2)
    }
    QQC2.Action {
        id: heading3Action
        text: qsTr("Heading 3")
        shortcut: "Ctrl+3"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 3 ? 0 : 3)
    }
    QQC2.Action {
        id: bodyTextAction
        text: qsTr("Body Text")
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(0)
    }
    QQC2.Action {
        id: boldAction
        text: qsTr("Bold")
        shortcut: StandardKey.Bold
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleInline("**")
    }
    QQC2.Action {
        id: italicAction
        text: qsTr("Italic")
        shortcut: StandardKey.Italic
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleInline("*")
    }
    QQC2.Action {
        id: strikethroughAction
        text: qsTr("Strikethrough")
        shortcut: "Ctrl+Shift+X"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleInline("~~")
    }
    QQC2.Action {
        id: codeAction
        text: qsTr("Code")
        shortcut: "Ctrl+E"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleInline("`")
    }
    QQC2.Action {
        id: linkAction
        text: qsTr("Link…")
        shortcut: "Ctrl+K"
        enabled: root.formatEnabled
        onTriggered: root.openLinkDialog()
    }
    QQC2.Action {
        id: clearFormattingAction
        text: qsTr("Clear Formatting")
        shortcut: "Ctrl+Space"
        enabled: root.formatEnabled
        onTriggered: root.view.md.clearFormatting()
    }
    QQC2.Action {
        id: bulletListAction
        text: qsTr("Bulleted List")
        shortcut: "Ctrl+Shift+8"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleBlock("bullet")
    }
    QQC2.Action {
        id: numberedListAction
        text: qsTr("Numbered List")
        shortcut: "Ctrl+Shift+7"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleBlock("numbered")
    }
    QQC2.Action {
        id: checklistAction
        text: qsTr("Checklist")
        shortcut: "Ctrl+Shift+9"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleBlock("task")
    }
    QQC2.Action {
        id: quoteAction
        text: qsTr("Quote")
        shortcut: "Ctrl+Shift+."
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleBlock("quote")
    }

    // Second keys for some actions.
    Shortcut {
        sequences: ["Ctrl+=", "Ctrl+Shift+="]
        enabled: zoomInAction.enabled
        onActivated: zoomInAction.trigger()
    }
    Shortcut {
        sequences: [StandardKey.Redo]
        enabled: redoAction.enabled
        onActivated: redoAction.trigger()
    }
    Shortcut {
        sequences: ["Ctrl+Tab", "Ctrl+PgDown"]
        onActivated: root.documents.next()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+Tab", "Ctrl+PgUp"]
        onActivated: root.documents.previous()
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.settingsOpen
        onActivated: root.settingsOpen = false
    }

    // --- Menus.

    GlobalMenu {
        window: root
        actions: root.actions
        onOpenRecent: path => root.documents.open([path])
    }

    // --- Layout.

    header: ColumnLayout {
        spacing: 0

        TabBar {
            id: tabBar
            Layout.fillWidth: true
            model: root.documents
            currentIndex: root.documents.currentIndex
            onActivated: index => {
                root.documents.currentIndex = index;
                root.settingsOpen = false;
            }
            onCloseRequested: index => root.closeTab(index)
            onNewRequested: root.documents.newTab()
            onMoved: (from, to) => root.documents.move(from, to)

            ToolbarButton {
                visible: !App.hasGlobalMenu
                text: qsTr("Menu")
                icon.source: Qt.resolvedUrl("../icons/menu.svg")
                onClicked: fallbackMenu.popup(this, 0, height)

                FallbackMenu {
                    id: fallbackMenu
                    actions: root.actions
                    onOpenRecent: path => root.documents.open([path])
                }
            }
            ToolbarButton {
                text: settingsAction.text
                shortcutText: "Ctrl+,"
                icon.source: Qt.resolvedUrl("../icons/settings.svg")
                checked: root.settingsOpen
                onClicked: settingsAction.trigger()
            }
        }
        FormatToolbar {
            Layout.fillWidth: true
            visible: root.markdown && root.settings.formattingToolbar && !root.settingsOpen
            actions: root.actions
            heading: root.view ? root.view.heading : 0
            formatted: root.document !== null && root.document.formatted
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Qt.alpha(Kirigami.Theme.textColor, 0.12)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: !root.settingsOpen

        InfoBanner {
            id: banner
            Layout.fillWidth: true
            Layout.margins: shown ? Kirigami.Units.smallSpacing : 0
            readonly property int kind: root.document ? root.document.banner : Document.NoBanner
            shown: kind !== Document.NoBanner
            type: root.bannerType(kind)
            text: root.bannerText(kind)
            actions: root.bannerActions(kind)
            closable: root.bannerClosable(kind)
            onClosed: {
                if (root.document) {
                    root.document.dismissBanner();
                }
            }
        }

        Item {
            id: editors
            Layout.fillWidth: true
            Layout.fillHeight: true

            Repeater {
                id: tabs
                model: root.documents
                delegate: Loader {
                    id: tab

                    required property Document document
                    required property int index
                    readonly property bool current: index === root.documents.currentIndex
                    // Made when first shown, kept after.
                    property bool wanted: current

                    anchors.fill: parent
                    visible: current
                    active: wanted
                    onCurrentChanged: {
                        if (current) {
                            wanted = true;
                            root.showView(item);
                        }
                    }
                    onLoaded: {
                        if (current) {
                            root.showView(item);
                        }
                    }
                    sourceComponent: EditorView {
                        document: tab.document
                        current: tab.current
                        onLinkRequested: root.openLinkDialog()
                    }
                }
            }

            FindBar {
                id: findBar
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: Kirigami.Units.smallSpacing
                anchors.rightMargin: Kirigami.Units.gridUnit
                width: Math.min(implicitWidth, parent.width - Kirigami.Units.gridUnit * 2)
                z: 10
                onFindTextChanged: root.find(false, true)
                onMatchCaseChanged: root.find(false, true)
                onWholeWordsChanged: root.find(false, true)
                onRegularExpressionChanged: root.find(false, true)
                onFindNext: root.find(false)
                onFindPrevious: root.find(true)
                onReplaceOne: root.replaceOne()
                onReplaceAll: root.replaceAll()
                onClosed: {
                    if (root.view) {
                        root.view.focusEditor();
                    }
                }
            }
        }
    }

    // Settings, over the editors.
    Loader {
        anchors.fill: parent
        active: root.settingsOpen
        sourceComponent: Item {
            Kirigami.Theme.colorSet: Kirigami.Theme.View
            Kirigami.Theme.inherit: false
            Rectangle {
                anchors.fill: parent
                color: Kirigami.Theme.backgroundColor
            }
            SettingsPage {
                anchors.fill: parent
            }
            ToolbarButton {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: Kirigami.Units.largeSpacing
                text: qsTr("Back")
                shortcutText: "Esc"
                icon.name: "go-previous"
                onClicked: root.settingsOpen = false
            }
        }
    }

    DropArea {
        anchors.fill: parent
        onEntered: drag => drag.accepted = drag.hasUrls
        onDropped: drop => {
            if (drop.hasUrls) {
                root.documents.open(drop.urls);
                drop.accept();
            }
        }
    }

    Toast {
        id: toast
    }

    footer: StatusBar {
        visible: root.settings.statusBar && !root.settingsOpen && root.document !== null

        StatusBarItem {
            readonly property point lineColumn: {
                const edit = root.view ? root.view.edit : null;
                return edit ? root.document.lineColumn(edit.cursorPosition) : Qt.point(1, 1);
            }
            text: qsTr("Ln %1, Col %2").arg(lineColumn.x).arg(lineColumn.y)
            clickable: true
            toolTip: goToAction.text
            onClicked: goToAction.trigger()
        }
        StatusBarItem {
            readonly property int selected: root.view ? root.view.selectedText.length : 0
            readonly property int total: root.document ? root.document.characterCount : 0
            text: selected > 0 ? qsTr("%1 of %2 characters").arg(Qt.locale().toString(selected)).arg(Qt.locale().toString(total)) : qsTr("%n character(s)", "", total)
            toolTip: root.document ? qsTr("%n word(s), %1 line(s)", "", root.document.wordCount).arg(Qt.locale().toString(root.document.lineCount)) : ""
        }
        Item {
            Layout.fillWidth: true
        }
        StatusBarItem {
            text: qsTr("%1%").arg(root.settings.zoom)
            clickable: true
            toolTip: qsTr("Zoom")
            menu: ContextMenu {
                ContextMenuItem {
                    action: zoomInAction
                    shortcutText: "Ctrl++"
                }
                ContextMenuItem {
                    action: zoomOutAction
                    shortcutText: "Ctrl+-"
                }
                ContextMenuItem {
                    action: zoomResetAction
                    shortcutText: "Ctrl+0"
                }
            }
        }
        StatusBarItem {
            text: root.document ? root.document.lineEndingName : ""
            clickable: root.editable
            toolTip: qsTr("Line endings")
            menu: ContextMenu {
                Repeater {
                    model: [
                        [Document.Lf, qsTr("Unix (LF)")],
                        [Document.CrLf, qsTr("Windows (CRLF)")],
                        [Document.Cr, qsTr("Macintosh (CR)")]
                    ]
                    delegate: ContextMenuItem {
                        required property var modelData
                        text: modelData[1]
                        checkable: true
                        checked: root.document !== null && root.document.lineEnding === modelData[0]
                        onTriggered: root.document.lineEnding = modelData[0]
                    }
                }
            }
        }
        StatusBarItem {
            text: root.document ? root.document.encodingName : ""
            clickable: true
            toolTip: qsTr("Encoding")
            menu: ContextMenu {
                id: encodingMenu
                readonly property var encodings: [
                    [Document.Utf8, "UTF-8"],
                    [Document.Utf8Bom, qsTr("UTF-8 with BOM")],
                    [Document.Utf16Le, "UTF-16 LE"],
                    [Document.Utf16Be, "UTF-16 BE"],
                    [Document.Windows1252, "Windows-1252"]
                ]
                ContextMenuItem {
                    text: qsTr("Save With")
                    enabled: false
                }
                Repeater {
                    model: encodingMenu.encodings
                    delegate: ContextMenuItem {
                        required property var modelData
                        text: modelData[1]
                        checkable: true
                        enabled: root.editable
                        checked: root.document !== null && root.document.encoding === modelData[0]
                        onTriggered: root.document.encoding = modelData[0]
                    }
                }
                ContextMenuSeparator {}
                ContextMenuItem {
                    text: qsTr("Reopen With")
                    enabled: false
                }
                Repeater {
                    model: encodingMenu.encodings
                    delegate: ContextMenuItem {
                        required property var modelData
                        text: modelData[1]
                        enabled: root.document !== null && root.document.path.length > 0 && !root.document.modified
                        onTriggered: root.document.reopenWithEncoding(modelData[0])
                    }
                }
            }
        }
    }

    // --- Dialogs.

    FileDialog {
        id: openDialog
        title: qsTr("Open")
        fileMode: FileDialog.OpenFiles
        currentFolder: root.document && root.document.path.length > 0 ? root.document.folder : ""
        nameFilters: [qsTr("Text documents (*.txt *.md *.markdown *.log)"), qsTr("All files (*)")]
        onAccepted: root.documents.open(selectedFiles)
    }

    FileDialog {
        id: saveDialog

        property Document target: null
        // Run after the target saved, or dropped if it didn't.
        property var then: null

        title: qsTr("Save As")
        fileMode: FileDialog.SaveFile
        nameFilters: target && target.markdown ? [qsTr("Markdown (*.md)"), qsTr("Text documents (*.txt)"), qsTr("All files (*)")] : [qsTr("Text documents (*.txt)"), qsTr("Markdown (*.md)"), qsTr("All files (*)")]
        onAccepted: target.saveAs(selectedFile)
        onRejected: {
            then = null;
            root.closing = false;
        }
    }

    UnsavedDialog {
        id: unsavedDialog

        property Document target: null
        property var then: null

        onSave: root.saveThen(target, then)
        onDiscard: then()
        onCancel: root.closing = false
    }

    ConfirmDialog {
        id: goToDialog
        title: qsTr("Go To Line")
        acceptText: qsTr("Go To")
        onAboutToShow: {
            const lc = root.document.lineColumn(root.view.edit.cursorPosition);
            lineField.text = String(lc.x);
            lineField.selectAll();
        }
        onOpened: lineField.forceActiveFocus()
        onAccepted: root.view.goToLine(parseInt(lineField.text) || 1)

        QQC2.TextField {
            id: lineField
            Layout.fillWidth: true
            inputMethodHints: Qt.ImhDigitsOnly
            validator: IntValidator {
                bottom: 1
            }
            placeholderText: root.document ? qsTr("1 to %1").arg(root.document.lineCount) : ""
            Accessible.name: qsTr("Line number")
            onAccepted: {
                goToDialog.accepted();
                goToDialog.close();
            }
        }
    }

    ConfirmDialog {
        id: linkDialog
        title: qsTr("Insert Link")
        acceptText: qsTr("Insert")
        onOpened: (linkText.text.length > 0 ? linkUrl : linkText).forceActiveFocus()
        onAccepted: root.view.md.insertLink(linkText.text, linkUrl.text.trim())
        onClosed: {
            if (root.view) {
                root.view.focusEditor();
            }
        }

        QQC2.TextField {
            id: linkText
            Layout.fillWidth: true
            placeholderText: qsTr("Text to show")
            Accessible.name: qsTr("Text")
        }
        QQC2.TextField {
            id: linkUrl
            Layout.fillWidth: true
            placeholderText: qsTr("Address, such as https://example.com")
            Accessible.name: qsTr("Address")
            onAccepted: {
                linkDialog.accepted();
                linkDialog.close();
            }
        }
    }

    // --- Behaviour.

    function showView(item) {
        if (item) {
            view = item;
            item.focusEditor();
        }
    }

    function openLinkDialog() {
        if (!formatEnabled) {
            return;
        }
        const selected = view.selectedText;
        const isUrl = /^[a-z][a-z0-9+.-]*:\S+$/i.test(selected);
        linkText.text = isUrl ? "" : selected;
        linkUrl.text = isUrl ? selected : (view.md.linkAt(view.edit.cursorPosition) || "");
        linkDialog.open();
    }

    function saveAs(doc, then) {
        saveDialog.target = doc;
        saveDialog.then = then ?? null;
        saveDialog.currentFolder = doc.path.length > 0 ? doc.folder : "";
        saveDialog.selectedFile = (doc.path.length > 0 ? doc.folder + "/" : "") + doc.suggestedFileName();
        saveDialog.open();
    }

    // Saves `doc`, then runs `then` once it saved.
    property Document pendingSave: null
    property var pendingThen: null
    function saveThen(doc, then) {
        pendingSave = doc;
        pendingThen = then;
        if (doc.path.length === 0) {
            saveAs(doc, then);
        } else {
            doc.save();
        }
    }
    Connections {
        target: root.pendingSave
        function onSaved() {
            const then = root.pendingThen;
            root.pendingSave = null;
            root.pendingThen = null;
            if (then) {
                then();
            }
        }
        function onSaveFailed() {
            root.pendingSave = null;
            root.pendingThen = null;
            root.closing = false;
        }
    }
    // Any tab: Save on an untitled tab asks where.
    Instantiator {
        model: root.documents
        delegate: Connections {
            required property Document document
            target: document
            function onSaveAsRequested() {
                if (root.pendingSave !== document) {
                    root.saveAs(document);
                }
            }
            function onSaved() {
                if (saveDialog.target === document && saveDialog.then) {
                    const then = saveDialog.then;
                    saveDialog.then = null;
                    if (root.pendingSave !== document) {
                        then();
                    }
                }
            }
        }
    }

    function askToSave(doc, then) {
        documents.currentIndex = documents.indexOf(doc);
        unsavedDialog.target = doc;
        unsavedDialog.then = then;
        unsavedDialog.fileName = doc.title;
        unsavedDialog.open();
    }

    function closeTab(index) {
        documents.requestClose(index);
    }
    Connections {
        target: root.documents
        function onCloseConfirmationNeeded(document) {
            root.askToSave(document, () => root.documents.closeDocument(document));
        }
        function onEmpty() {
            root.close();
        }
        function onOpenFailed(message) {
            toast.show(message);
        }
    }

    // Closing the window: the App decides whether to ask (see App.closeWindow);
    // if so, each unsaved tab is asked about in turn.
    property bool closing: false
    property bool closeNow: false
    onClosing: close => {
        if (closeNow || App.closeWindow(documents)) {
            return;
        }
        close.accepted = false;
        closing = true;
        askNext();
    }
    function askNext() {
        if (!closing) {
            return;
        }
        for (const doc of documents.documents()) {
            if (doc.modified) {
                askToSave(doc, () => {
                    documents.closeDocument(doc);
                    askNext();
                });
                return;
            }
        }
        closeNow = App.closeWindow(documents, true);
        root.close();
    }

    // Find, on the current tab.
    function findFlags() {
        return (findBar.matchCase ? Document.MatchCase : 0) | (findBar.wholeWords ? Document.WholeWords : 0) | (findBar.regularExpression ? Document.RegularExpression : 0);
    }
    function showMatch(result) {
        findBar.error = result.error ?? "";
        findBar.matchCount = result.count ?? 0;
        findBar.currentMatch = result.index ?? 0;
        if (result.start !== undefined) {
            view.select(result.start, result.end);
        }
    }
    // `typing`: the search text changed, so the match at the caret may grow.
    function find(backward, typing) {
        if (!view || !findBar.opened && findBar.findText.length === 0) {
            return;
        }
        const edit = view.edit;
        const from = typing ? edit.selectionStart : backward ? edit.selectionStart : edit.selectionEnd;
        showMatch(document.find(findBar.findText, findFlags(), from, backward));
    }
    function replaceOne() {
        if (!editable) {
            return;
        }
        const edit = view.edit;
        showMatch(document.replaceOne(findBar.findText, findBar.replaceText, findFlags(), edit.selectionStart, edit.selectionEnd));
    }
    function replaceAll() {
        if (!editable) {
            return;
        }
        const count = document.replaceAll(findBar.findText, findBar.replaceText, findFlags());
        toast.show(qsTr("Replaced %n match(es)", "", count));
        find(false, true);
    }
    function openFind(withReplace) {
        const selected = view.selectedText;
        if (selected.length > 0 && selected.indexOf(" ") < 0 && selected.indexOf("\n") < 0) {
            findBar.findText = selected;
        }
        findBar.replaceVisible = withReplace;
        findBar.open(withReplace);
    }
    onDocumentChanged: {
        if (findBar.opened) {
            Qt.callLater(() => find(false, true));
        }
    }

    // Banners.
    function bannerType(kind) {
        switch (kind) {
        case Document.SaveFailed:
        case Document.TooLarge:
            return "error";
        case Document.Unencodable:
        case Document.ChangedOnDisk:
        case Document.Deleted:
        case Document.Lossy:
            return "warning";
        default:
            return "info";
        }
    }
    function bannerText(kind) {
        const name = document ? document.title : "";
        switch (kind) {
        case Document.SaveFailed:
            return qsTr("Couldn't save %1: %2").arg(name).arg(document.bannerText);
        case Document.Unencodable:
            return qsTr("Some characters can't be saved as %1.").arg(document.encodingName);
        case Document.ChangedOnDisk:
            return qsTr("%1 was changed by another app.").arg(name);
        case Document.Deleted:
            return qsTr("%1 was deleted or moved.").arg(name);
        case Document.TooLarge:
            return qsTr("%1 is too large to open. Notepad opens files up to 10 MB.").arg(name);
        case Document.Binary:
            return qsTr("This file doesn't look like text, so it's open read-only.");
        case Document.LongLines:
            return qsTr("This file has very long lines, so it's open read-only.");
        case Document.Lossy:
            return qsTr("Some characters couldn't be read and were replaced. Saving keeps the replacements.");
        case Document.MixedLineEndings:
            return qsTr("This file mixes line endings. Saving makes them all %1.").arg(document.lineEndingName);
        case Document.FormattingOff:
            return qsTr("Large file: formatting is off.");
        case Document.ReadOnlyFile:
            return qsTr("You can't save changes to this file. Use Save As to keep them.");
        default:
            return "";
        }
    }
    function bannerClosable(kind) {
        return kind >= Document.Binary || kind === Document.SaveFailed;
    }
    function bannerActions(kind) {
        switch (kind) {
        case Document.SaveFailed:
            return [bannerSaveAs, bannerRetry];
        case Document.Unencodable:
            return [bannerSaveUtf8];
        case Document.ChangedOnDisk:
            return [bannerReload, bannerKeepMine];
        case Document.Deleted:
            return [bannerSave, bannerCloseTab];
        case Document.TooLarge:
            return [bannerCloseTab];
        case Document.ReadOnlyFile:
            return [bannerSaveAs];
        default:
            return [];
        }
    }
    QQC2.Action {
        id: bannerSaveAs
        text: qsTr("Save As…")
        onTriggered: root.saveAs(root.document)
    }
    QQC2.Action {
        id: bannerRetry
        text: qsTr("Try Again")
        onTriggered: root.document.save()
    }
    QQC2.Action {
        id: bannerSave
        text: qsTr("Save")
        onTriggered: root.document.save()
    }
    QQC2.Action {
        id: bannerSaveUtf8
        text: qsTr("Save as UTF-8")
        onTriggered: {
            root.document.encoding = Document.Utf8;
            root.document.save();
        }
    }
    QQC2.Action {
        id: bannerReload
        text: qsTr("Reload")
        onTriggered: root.document.reload()
    }
    QQC2.Action {
        id: bannerKeepMine
        text: qsTr("Keep Mine")
        onTriggered: root.document.keepMine()
    }
    QQC2.Action {
        id: bannerCloseTab
        text: qsTr("Close Tab")
        onTriggered: root.documents.close(root.documents.currentIndex)
    }
}
