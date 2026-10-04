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
            heading4: heading4Action,
            heading5: heading5Action,
            heading6: heading6Action,
            font: fontAction,
            keyboardShortcuts: keyboardShortcutsAction,
            about: aboutAction,
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
    title: document ? (document.isRemote && document.host.length > 0 ? qsTr("%1 — %2 — Notepad").arg(document.title).arg(document.host) : qsTr("%1 — Notepad").arg(document.title)) : qsTr("Notepad")
    color: Kirigami.Theme.backgroundColor

    // --- Actions. The menus, the toolbar and the shortcuts all use these.

    // A key sequence may live on one object only, or neither fires: on the
    // global menu's item when there is one (so the menu shows it), else here.
    component KeyedAction: QQC2.Action {
        property var keys
        shortcut: App.hasGlobalMenu ? undefined : keys
    }

    KeyedAction {
        id: newTabAction
        text: qsTr("New Tab")
        keys: StandardKey.New
        onTriggered: root.documents.newTab()
    }
    KeyedAction {
        id: newWindowAction
        text: qsTr("New Window")
        keys: "Ctrl+Shift+N"
        onTriggered: App.newWindow()
    }
    KeyedAction {
        id: openAction
        text: qsTr("Open…")
        keys: StandardKey.Open
        onTriggered: openDialog.open()
    }
    KeyedAction {
        id: saveAction
        text: qsTr("Save")
        keys: StandardKey.Save
        enabled: root.document !== null && !root.document.loading && !root.document.readOnly
        onTriggered: root.document.save()
    }
    KeyedAction {
        id: saveAsAction
        text: qsTr("Save As…")
        keys: "Ctrl+Shift+S"
        enabled: root.document !== null && !root.document.loading && root.document.banner !== Document.TooLarge
        onTriggered: root.saveAs(root.document)
    }
    KeyedAction {
        id: saveAllAction
        text: qsTr("Save All")
        keys: "Ctrl+Alt+S"
        enabled: root.documents.anyModified
        onTriggered: root.saveAll()
    }
    KeyedAction {
        id: printAction
        text: qsTr("Print…")
        keys: StandardKey.Print
        enabled: root.document !== null && !root.document.loading
        onTriggered: App.print(root.document, root)
    }
    KeyedAction {
        id: closeTabAction
        text: qsTr("Close Tab")
        keys: StandardKey.Close
        enabled: root.documents.count > 0
        onTriggered: root.closeTab(root.documents.currentIndex)
    }
    KeyedAction {
        id: reopenTabAction
        text: qsTr("Reopen Closed Tab")
        keys: "Ctrl+Shift+T"
        enabled: root.documents.canReopenClosed
        onTriggered: root.documents.reopenClosed()
    }
    KeyedAction {
        id: closeWindowAction
        text: qsTr("Close Window")
        keys: "Ctrl+Shift+W"
        onTriggered: root.close()
    }
    KeyedAction {
        id: quitAction
        text: qsTr("Quit")
        keys: StandardKey.Quit
        onTriggered: App.quit()
    }

    KeyedAction {
        id: undoAction
        text: qsTr("Undo")
        keys: StandardKey.Undo
        enabled: (root.view !== null && root.view.edit.canUndo) && !root.settingsOpen
        onTriggered: root.view.edit.undo()
    }
    KeyedAction {
        id: redoAction
        text: qsTr("Redo")
        keys: "Ctrl+Y"
        enabled: (root.view !== null && root.view.edit.canRedo) && !root.settingsOpen
        onTriggered: root.view.edit.redo()
    }
    KeyedAction {
        id: cutAction
        text: qsTr("Cut")
        keys: StandardKey.Cut
        enabled: (root.editable && root.view.edit.selectionStart !== root.view.edit.selectionEnd) && !root.settingsOpen
        onTriggered: root.view.edit.cut()
    }
    KeyedAction {
        id: copyAction
        text: qsTr("Copy")
        keys: StandardKey.Copy
        enabled: (root.view !== null && root.view.edit.selectionStart !== root.view.edit.selectionEnd) && !root.settingsOpen
        onTriggered: root.view.edit.copy()
    }
    KeyedAction {
        id: pasteAction
        text: qsTr("Paste")
        keys: StandardKey.Paste
        enabled: (root.editable && root.view.edit.canPaste) && !root.settingsOpen
        onTriggered: root.view.edit.paste()
    }
    KeyedAction {
        id: deleteAction
        text: qsTr("Delete")
        enabled: (root.editable && root.view.edit.selectionStart !== root.view.edit.selectionEnd) && !root.settingsOpen
        onTriggered: root.view.edit.remove(root.view.edit.selectionStart, root.view.edit.selectionEnd)
    }
    KeyedAction {
        id: selectAllAction
        text: qsTr("Select All")
        keys: StandardKey.SelectAll
        enabled: (root.view !== null) && !root.settingsOpen
        onTriggered: root.view.edit.selectAll()
    }
    KeyedAction {
        id: findAction
        text: qsTr("Find…")
        keys: StandardKey.Find
        enabled: root.view !== null && !root.settingsOpen
        onTriggered: root.openFind(false)
    }
    KeyedAction {
        id: findNextAction
        text: qsTr("Find Next")
        keys: StandardKey.FindNext
        enabled: (root.view !== null && findBar.findText.length > 0) && !root.settingsOpen
        onTriggered: root.find(false)
    }
    KeyedAction {
        id: findPreviousAction
        text: qsTr("Find Previous")
        keys: StandardKey.FindPrevious
        enabled: (root.view !== null && findBar.findText.length > 0) && !root.settingsOpen
        onTriggered: root.find(true)
    }
    KeyedAction {
        id: replaceAction
        text: qsTr("Replace…")
        keys: "Ctrl+H"
        enabled: root.editable && !root.settingsOpen
        onTriggered: root.openFind(true)
    }
    KeyedAction {
        id: goToAction
        text: qsTr("Go To Line…")
        keys: "Ctrl+G"
        enabled: root.view !== null && !root.settingsOpen
        onTriggered: goToDialog.open()
    }
    KeyedAction {
        id: timeDateAction
        text: qsTr("Time/Date")
        keys: "F5"
        enabled: (root.editable) && !root.settingsOpen
        onTriggered: root.view.insertText(App.timeDate())
    }

    KeyedAction {
        id: zoomInAction
        text: qsTr("Zoom In")
        keys: StandardKey.ZoomIn
        enabled: root.settings.zoom < 400
        onTriggered: root.settings.zoom = root.settings.zoom + 10
    }
    KeyedAction {
        id: zoomOutAction
        text: qsTr("Zoom Out")
        keys: StandardKey.ZoomOut
        enabled: root.settings.zoom > 50
        onTriggered: root.settings.zoom = root.settings.zoom - 10
    }
    KeyedAction {
        id: zoomResetAction
        text: qsTr("Restore Default Zoom")
        keys: "Ctrl+0"
        enabled: root.settings.zoom !== 100
        onTriggered: root.settings.zoom = 100
    }
    KeyedAction {
        id: wordWrapAction
        text: qsTr("Word Wrap")
        keys: "Alt+Z"
        checkable: true
        checked: root.settings.wordWrap
        onTriggered: root.settings.wordWrap = !root.settings.wordWrap
    }
    KeyedAction {
        id: lineNumbersAction
        text: qsTr("Line Numbers")
        checkable: true
        checked: root.settings.lineNumbers
        onTriggered: root.settings.lineNumbers = !root.settings.lineNumbers
    }
    KeyedAction {
        id: statusBarAction
        text: qsTr("Status Bar")
        checkable: true
        checked: root.settings.statusBar
        onTriggered: root.settings.statusBar = !root.settings.statusBar
    }
    KeyedAction {
        id: toolbarAction
        text: qsTr("Formatting Toolbar")
        checkable: true
        checked: root.settings.formattingToolbar
        enabled: root.settings.formatting
        onTriggered: root.settings.formattingToolbar = !root.settings.formattingToolbar
    }
    KeyedAction {
        id: toggleFormattedAction
        text: qsTr("Show Markdown Syntax")
        keys: "Ctrl+Shift+M"
        checkable: true
        checked: root.document !== null && !root.document.formatted
        enabled: root.markdown && !root.settingsOpen
        onTriggered: root.document.formatted = !root.document.formatted
    }
    KeyedAction {
        id: fontAction
        text: qsTr("Font…")
        onTriggered: root.settingsOpen = true
    }
    KeyedAction {
        id: keyboardShortcutsAction
        text: qsTr("Keyboard Shortcuts")
        onTriggered: shortcutsDialog.open()
    }
    KeyedAction {
        id: aboutAction
        text: qsTr("About Notepad")
        onTriggered: aboutDialog.open()
    }
    KeyedAction {
        id: settingsAction
        text: qsTr("Settings")
        keys: "Ctrl+,"
        onTriggered: root.settingsOpen = !root.settingsOpen
    }

    KeyedAction {
        id: heading1Action
        text: qsTr("Heading 1")
        keys: "Ctrl+1"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 1 ? 0 : 1)
    }
    KeyedAction {
        id: heading2Action
        text: qsTr("Heading 2")
        keys: "Ctrl+2"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 2 ? 0 : 2)
    }
    KeyedAction {
        id: heading3Action
        text: qsTr("Heading 3")
        keys: "Ctrl+3"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 3 ? 0 : 3)
    }
    KeyedAction {
        id: heading4Action
        text: qsTr("Heading 4")
        keys: "Ctrl+4"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 4 ? 0 : 4)
    }
    KeyedAction {
        id: heading5Action
        text: qsTr("Heading 5")
        keys: "Ctrl+5"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 5 ? 0 : 5)
    }
    KeyedAction {
        id: heading6Action
        text: qsTr("Heading 6")
        keys: "Ctrl+6"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(root.view.heading === 6 ? 0 : 6)
    }
    KeyedAction {
        id: bodyTextAction
        text: qsTr("Body Text")
        keys: "Ctrl+Shift+0"
        enabled: root.formatEnabled
        onTriggered: root.view.md.setHeading(0)
    }
    KeyedAction {
        id: boldAction
        text: qsTr("Bold")
        keys: StandardKey.Bold
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleInline("**")
    }
    KeyedAction {
        id: italicAction
        text: qsTr("Italic")
        keys: StandardKey.Italic
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleInline("*")
    }
    KeyedAction {
        id: strikethroughAction
        text: qsTr("Strikethrough")
        keys: "Ctrl+Shift+X"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleInline("~~")
    }
    KeyedAction {
        id: codeAction
        text: qsTr("Code")
        keys: "Ctrl+E"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleInline("`")
    }
    KeyedAction {
        id: linkAction
        text: qsTr("Link…")
        keys: "Ctrl+K"
        enabled: root.formatEnabled
        onTriggered: root.openLinkDialog()
    }
    KeyedAction {
        id: clearFormattingAction
        text: qsTr("Clear Formatting")
        keys: "Ctrl+Space"
        enabled: root.formatEnabled
        onTriggered: root.view.md.clearFormatting()
    }
    KeyedAction {
        id: bulletListAction
        text: qsTr("Bulleted List")
        keys: "Ctrl+Shift+8"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleBlock("bullet")
    }
    KeyedAction {
        id: numberedListAction
        text: qsTr("Numbered List")
        keys: "Ctrl+Shift+7"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleBlock("numbered")
    }
    KeyedAction {
        id: checklistAction
        text: qsTr("Checklist")
        keys: "Ctrl+Shift+9"
        enabled: root.formatEnabled
        onTriggered: root.view.md.toggleBlock("task")
    }
    KeyedAction {
        id: quoteAction
        text: qsTr("Quote")
        keys: "Ctrl+Shift+."
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
        sequence: "Ctrl+T"
        onActivated: newTabAction.trigger()
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
        withShortcuts: App.hasGlobalMenu
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
                id: menuButton
                visible: !App.hasGlobalMenu
                text: qsTr("Menu")
                shortcutText: "F10"
                icon.source: Qt.resolvedUrl("../icons/menu.svg")
                onClicked: fallbackMenu.popup(menuButton, 0, menuButton.height)

                // The keyboard way in, as F10 opens a menu bar elsewhere.
                Shortcut {
                    sequence: "F10"
                    enabled: menuButton.visible
                    onActivated: menuButton.clicked()
                }

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

        // The App's messages (App.message): the session can't be written,
        // was set aside, belongs to another Notepad. Stays until closed.
        InfoBanner {
            id: appBanner
            Layout.fillWidth: true
            Layout.margins: shown ? Kirigami.Units.smallSpacing : 0
            shown: root.appMessage.length > 0
            type: "warning"
            text: root.appMessage
            closable: true
            onClosed: {
                root.appMessage = "";
                shown = Qt.binding(() => root.appMessage.length > 0);
            }
        }

        // A remote file on its way.
        InfoBanner {
            id: loadBanner
            Layout.fillWidth: true
            Layout.margins: shown ? Kirigami.Units.smallSpacing : 0
            shown: root.document !== null && root.document.fetching
            type: "info"
            text: !root.document ? "" : root.document.loadPercent >= 0 ? qsTr("Loading %1… %2%").arg(root.document.title).arg(root.document.loadPercent) : qsTr("Loading %1…").arg(root.document.title)
            actions: [bannerCancelLoad]
        }

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
                // The close button set `shown` itself, which ended the
                // binding; without it no banner would show again.
                if (root.document) {
                    root.document.dismissBanner();
                }
                shown = Qt.binding(() => kind !== Document.NoBanner);
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
                const edit = root.view && root.document ? root.view.edit : null;
                return edit ? root.document.lineColumn(edit.cursorPosition) : Qt.point(1, 1);
            }
            text: qsTr("Ln %1, Col %2").arg(lineColumn.x).arg(lineColumn.y)
            clickable: true
            toolTip: goToAction.text
            onClicked: goToAction.trigger()
        }
        StatusBarItem {
            readonly property int selected: root.view ? root.view.edit.selectionEnd - root.view.edit.selectionStart : 0
            readonly property int total: root.document ? root.document.characterCount : 0
            text: selected > 0 ? qsTr("%1 of %2 characters").arg(selected.toLocaleString(Qt.locale(), "f", 0)).arg(total.toLocaleString(Qt.locale(), "f", 0))
                : total === 1 ? qsTr("1 character") : qsTr("%1 characters").arg(total.toLocaleString(Qt.locale(), "f", 0))
            toolTip: root.document ? qsTr("%n word(s), %1 line(s)", "", root.document.wordCount).arg(Qt.locale().toString(root.document.lineCount)) : ""
        }
        Item {
            Layout.fillWidth: true
        }
        StatusBarItem {
            visible: root.document !== null && root.document.saving
            text: qsTr("Saving…")
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

        title: qsTr("Save As")
        fileMode: FileDialog.SaveFile
        nameFilters: target && target.markdown ? [qsTr("Markdown (*.md)"), qsTr("Text documents (*.txt)"), qsTr("All files (*)")] : [qsTr("Text documents (*.txt)"), qsTr("Markdown (*.md)"), qsTr("All files (*)")]
        onAccepted: target.saveAs(selectedFile)
        onRejected: {
            if (root.pendingSave === target) {
                root.resetSave(); // a close or quit waiting on this save stops
            }
        }
    }

    UnsavedDialog {
        id: unsavedDialog

        property Document target: null
        property var then: null

        note: App.sessionProblem.length > 0 ? qsTr("Notepad can't keep your unsaved changes for next time: %1").arg(App.sessionProblem) : ""
        onSave: root.saveThen(target, then)
        onDiscard: then()
        onCancel: root.resetSave()
    }

    ConfirmDialog {
        id: shortcutsDialog
        title: qsTr("Keyboard Shortcuts")
        acceptText: qsTr("Close")
        showReject: false

        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.preferredWidth: Kirigami.Units.gridUnit * 22
            Layout.preferredHeight: Math.min(implicitHeight, root.height * 0.6)
            contentWidth: availableWidth
            QQC2.ScrollBar.vertical: SlimScrollBar {}

            GridLayout {
                width: parent.width - Kirigami.Units.gridUnit // clear of the scrollbar
                columns: 2
                columnSpacing: Kirigami.Units.largeSpacing * 2
                rowSpacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: root.shortcutRows()
                    delegate: QQC2.Label {
                        required property var modelData
                        required property int index
                        Layout.row: Math.floor(index / 2)
                        Layout.column: index % 2
                        Layout.fillWidth: index % 2 === 0
                        text: modelData
                        opacity: index % 2 === 1 ? 0.7 : 1
                        Layout.alignment: index % 2 === 1 ? Qt.AlignRight : Qt.AlignLeft
                    }
                }
            }
        }
    }

    ConfirmDialog {
        id: aboutDialog
        title: qsTr("About Notepad")
        text: qsTr("Version %1\nA simple, fast Notepad for plain text and Markdown.\nMIT licence, made by Eterneon. Notepad collects nothing and needs no account.").arg(App.version)
        acceptText: qsTr("Close")
        showReject: false

        QQC2.Label {
            Layout.fillWidth: true
            text: "<a href=\"https://github.com/EternalCoder454/atlasos-notepad\">%1</a>".arg(qsTr("Project page"))
            textFormat: Text.StyledText
            linkColor: Kirigami.Theme.linkColor
            onLinkActivated: link => Qt.openUrlExternally(link)
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }
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
            onAccepted: linkUrl.forceActiveFocus()
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
        const selected = view.edit.selectedText;
        const isUrl = /^[a-z][a-z0-9+.-]*:\S+$/i.test(selected);
        linkText.text = isUrl ? "" : selected;
        linkUrl.text = isUrl ? selected : (view.md.linkAt(view.edit.cursorPosition) || "");
        linkDialog.open();
    }

    function saveAs(doc) {
        saveDialog.target = doc;
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
            saveAs(doc);
        } else {
            doc.save();
        }
    }
    // A save that didn't happen (cancelled, failed, refused) ends the whole
    // close or quit it was part of.
    function resetSave() {
        pendingSave = null;
        pendingThen = null;
        closing = false;
        App.cancelQuit();
    }
    Connections {
        target: root.pendingSave
        function onSaved() {
            const doc = root.pendingSave;
            const then = root.pendingThen;
            root.pendingSave = null;
            root.pendingThen = null;
            if (doc.modified) {
                // Typed while it saved: what's on disk isn't all of it.
                root.askToSave(doc, then);
            } else if (then) {
                then();
            }
        }
        function onSaveFailed() {
            root.resetSave();
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
        }
    }

    // Name, keys pairs (flattened) for the Keyboard Shortcuts dialog.
    function shortcutRows() {
        const rows = [];
        for (const action of Object.values(actions)) {
            if (action.keys !== undefined) {
                rows.push(action.text.replace("…", ""), App.shortcutText(action.keys));
            }
        }
        rows.push(qsTr("New Tab"), "Ctrl+T");
        rows.push(qsTr("Next Tab"), "Ctrl+Tab");
        rows.push(qsTr("Previous Tab"), "Ctrl+Shift+Tab");
        rows.push(qsTr("Open a Link"), qsTr("Ctrl+Click"));
        if (!App.hasGlobalMenu) {
            rows.push(qsTr("Menu"), "F10");
        }
        return rows;
    }

    // Named tabs save at once; untitled ones ask where, one after another.
    function saveAll() {
        const untitled = [];
        for (const doc of documents.documents()) {
            if (!doc.modified || doc.readOnly) {
                continue;
            }
            if (doc.path.length > 0) {
                doc.save();
            } else {
                untitled.push(doc);
            }
        }
        saveUntitled(untitled);
    }
    function saveUntitled(list) {
        if (list.length === 0) {
            return;
        }
        const doc = list.shift();
        documents.currentIndex = documents.indexOf(doc);
        saveThen(doc, () => saveUntitled(list));
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
        // A tab shown or opened brings the editor back from Settings.
        function onCurrentIndexChanged() {
            root.settingsOpen = false;
        }
        function onCountChanged() {
            root.settingsOpen = false;
        }
        function onOpenFailed(message) {
            toast.show(message);
        }
    }

    property string appMessage
    property bool appMessageIsSessionProblem: false
    Connections {
        target: App
        function onMessage(text) {
            if (App.activeWindow() === root) {
                root.appMessage = text;
                root.appMessageIsSessionProblem = App.sessionProblem.length > 0;
            }
        }
        // Writing works again: the warning is over.
        function onSessionProblemChanged() {
            if (App.sessionProblem.length === 0 && root.appMessageIsSessionProblem) {
                root.appMessage = "";
                root.appMessageIsSessionProblem = false;
            }
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
        if (findBar.error.length === 0) {
            toast.show(count === 1 ? qsTr("Replaced 1 match") : qsTr("Replaced %1 matches").arg(count));
        }
        find(false, true);
    }
    function openFind(withReplace) {
        const selected = view.edit.selectedText;
        if (selected.length > 0 && selected.indexOf(" ") < 0 && selected.indexOf("\n") < 0) {
            findBar.findText = selected;
        }
        if (withReplace) {
            findBar.replaceVisible = true;
        }
        findBar.open(withReplace);
    }
    // Another tab: recount, but leave its caret and selection alone.
    onDocumentChanged: {
        if (findBar.opened) {
            Qt.callLater(() => {
                if (!view || findBar.findText.length === 0) {
                    return;
                }
                const edit = view.edit;
                const result = document.find(findBar.findText, findFlags(), edit.selectionStart, false);
                findBar.error = result.error ?? "";
                findBar.matchCount = result.count ?? 0;
                // A match already selected there is the current one.
                const onMatch = result.start === edit.selectionStart && result.end === edit.selectionEnd;
                findBar.currentMatch = onMatch ? result.index : 0;
            });
        }
    }

    // Banners.
    function bannerType(kind) {
        switch (kind) {
        case Document.SaveFailed:
        case Document.ReadFailed:
        case Document.TooLarge:
            return "error";
        case Document.Unencodable:
        case Document.ChangedOnDisk:
        case Document.Deleted:
        case Document.Lossy:
        case Document.Unrecovered:
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
        case Document.ReadFailed:
            return qsTr("Couldn't read %1: %2").arg(name).arg(document.bannerText);
        case Document.Unrecovered:
            return document.path.length > 0 ? qsTr("Your unsaved changes to %1 couldn't be recovered; this is the file as saved.").arg(name) : qsTr("The text of %1 couldn't be recovered.").arg(name);
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
        return kind >= Document.Unrecovered || kind === Document.SaveFailed || kind === Document.ReadFailed;
    }
    function bannerActions(kind) {
        switch (kind) {
        case Document.SaveFailed:
            return [bannerSaveAs, bannerRetry];
        case Document.ReadFailed:
            // Reading again drops what was typed since.
            return document.modified ? [] : [bannerReadAgain];
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
    KeyedAction {
        id: bannerCancelLoad
        text: qsTr("Cancel")
        onTriggered: root.document.cancelLoad()
    }
    KeyedAction {
        id: bannerSaveAs
        text: qsTr("Save As…")
        onTriggered: root.saveAs(root.document)
    }
    KeyedAction {
        id: bannerRetry
        text: qsTr("Try Again")
        onTriggered: root.document.save()
    }
    KeyedAction {
        id: bannerReadAgain
        text: qsTr("Try Again")
        onTriggered: root.document.reload()
    }
    KeyedAction {
        id: bannerSave
        text: qsTr("Save")
        onTriggered: root.document.save()
    }
    KeyedAction {
        id: bannerSaveUtf8
        text: qsTr("Save as UTF-8")
        onTriggered: {
            root.document.encoding = Document.Utf8;
            root.document.save();
        }
    }
    KeyedAction {
        id: bannerReload
        text: qsTr("Reload")
        onTriggered: root.document.reload()
    }
    KeyedAction {
        id: bannerKeepMine
        text: qsTr("Keep Mine")
        onTriggered: root.document.keepMine()
    }
    KeyedAction {
        id: bannerCloseTab
        text: qsTr("Close Tab")
        onTriggered: root.closeTab(root.documents.currentIndex) // asks first: the text may be unsaved
    }
}
