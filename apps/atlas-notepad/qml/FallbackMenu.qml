// The menu button's menu, for when there is no global menu: the same
// actions as GlobalMenu, in submenus.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import Atlas.Ui
import net.eterneon.atlas.notepad

ContextMenu {
    id: menu

    required property var actions

    // Never taller than the window: the list scrolls past that.
    height: Math.min(implicitHeight, (QQC2.Overlay.overlay ? QQC2.Overlay.overlay.height : implicitHeight) - topMargin - bottomMargin)

    signal openRecent(string path)
    Component.onCompleted: contentItem.interactive = Qt.binding(() => contentItem.contentHeight + topPadding + bottomPadding > height)

    // A submenu: inside the window, scrolling when it doesn't fit. (Atlas.Ui
    // scrolls only past the window's height, not past it less the margins.)
    component SubMenu: ContextMenu {
        height: Math.min(implicitHeight, (QQC2.Overlay.overlay ? QQC2.Overlay.overlay.height : implicitHeight) - topMargin - bottomMargin)
        Component.onCompleted: contentItem.interactive = Qt.binding(() => contentItem.contentHeight + topPadding + bottomPadding > height)
    }

    component Item: ContextMenuItem {
        // The shortcut lives on the action; this only shows it.
        shortcutText: action ? App.shortcutText(action.keys ?? action.shortcut) : ""
    }

    Item { action: menu.actions.newTab }
    Item { action: menu.actions.newWindow }
    Item { action: menu.actions.open }
    SubMenu {
        id: recent
        title: qsTr("Open Recent")
        // Where the recent files go: just after the separator that ends the
        // lead items (Reopen Closed Tab), wherever that is in the menu.
        function recentLead(): int {
            for (let i = 0; i < count; ++i) {
                if (itemAt(i) === leadEnd) {
                    return i + 1;
                }
            }
            console.warn("FallbackMenu: the Open Recent lead separator is missing");
            return count; // after everything, never above Reopen Closed Tab
        }

        Item { action: menu.actions.reopenTab }
        ContextMenuSeparator {
            id: leadEnd
        }
        Instantiator {
            model: App.recentFiles
            delegate: ContextMenuItem {
                required property string modelData
                text: App.displayPath(modelData)
                onTriggered: menu.openRecent(modelData)
            }
            onObjectAdded: (index, object) => recent.insertItem(index + recent.recentLead(), object)
            onObjectRemoved: (index, object) => recent.removeItem(object)
        }
        // Only with files to separate from the rest: else it would sit
        // directly under the first separator.
        ContextMenuSeparator {
            visible: App.recentFiles.length > 0
        }
        ContextMenuItem {
            text: qsTr("Clear List")
            enabled: App.recentFiles.length > 0
            onTriggered: App.clearRecentFiles()
        }
    }
    Item { action: menu.actions.save }
    Item { action: menu.actions.saveAs }
    Item { action: menu.actions.saveAll }
    // One submenu: the whole menu then fits a default-size window.
    SubMenu {
        title: qsTr("File Location")
        Item { action: menu.actions.openWith }
        Item { action: menu.actions.showInFolder }
        Item { action: menu.actions.copyLocation }
        Item { action: menu.actions.properties }
    }
    Item { action: menu.actions.print }
    ContextMenuSeparator {}
    SubMenu {
        title: qsTr("Edit")
        Item { action: menu.actions.undo }
        Item { action: menu.actions.redo }
        ContextMenuSeparator {}
        Item { action: menu.actions.cut }
        Item { action: menu.actions.copy }
        Item { action: menu.actions.paste }
        Item { action: menu.actions.delete }
        ContextMenuSeparator {}
        Item { action: menu.actions.find }
        Item { action: menu.actions.replace }
        Item { action: menu.actions.goTo }
        Item { action: menu.actions.selectAll }
        Item { action: menu.actions.timeDate }
        Item { action: menu.actions.font }
    }
    SubMenu {
        title: qsTr("Format")
        enabled: menu.actions.bold.enabled
        SubMenu {
            title: qsTr("Heading")
            Item { action: menu.actions.heading1 }
            Item { action: menu.actions.heading2 }
            Item { action: menu.actions.heading3 }
            Item { action: menu.actions.heading4 }
            Item { action: menu.actions.heading5 }
            Item { action: menu.actions.heading6 }
            Item { action: menu.actions.bodyText }
        }
        ContextMenuSeparator {}
        Item { action: menu.actions.bold }
        Item { action: menu.actions.italic }
        Item { action: menu.actions.strikethrough }
        Item { action: menu.actions.code }
        Item { action: menu.actions.link }
        ContextMenuSeparator {}
        Item { action: menu.actions.bulletList }
        Item { action: menu.actions.numberedList }
        Item { action: menu.actions.checklist }
        Item { action: menu.actions.quote }
        ContextMenuSeparator {}
        Item { action: menu.actions.clearFormatting }
    }
    SubMenu {
        title: qsTr("Code")
        enabled: menu.actions.toggleComment.enabled
        Item { action: menu.actions.toggleComment }
        Item { action: menu.actions.indent }
        Item { action: menu.actions.outdent }
        ContextMenuSeparator {}
        Item { action: menu.actions.sortLines }
        Item { action: menu.actions.upperCase }
        Item { action: menu.actions.lowerCase }
        Item { action: menu.actions.titleCase }
        Item { action: menu.actions.trimSpaces }
        ContextMenuSeparator {}
        Item { action: menu.actions.codeLineNumbers }
    }
    SubMenu {
        title: qsTr("View")
        Item { action: menu.actions.zoomIn }
        Item { action: menu.actions.zoomOut }
        Item { action: menu.actions.zoomReset }
        ContextMenuSeparator {}
        Item { action: menu.actions.toggleFormatted }
        Item { action: menu.actions.toolbar }
        Item { action: menu.actions.statusBar }
        Item { action: menu.actions.lineNumbers }
        Item { action: menu.actions.wordWrap }
    }
    SubMenu {
        title: qsTr("Help")
        Item { action: menu.actions.keyboardShortcuts }
        Item { action: menu.actions.about }
    }
    Item { action: menu.actions.settings }
    ContextMenuSeparator {}
    Item { action: menu.actions.closeTab }
    Item { action: menu.actions.closeWindow }
    Item { action: menu.actions.quit }
}
