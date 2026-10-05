// The window's menus for Plasma's global menu (Qt.labs.platform: native
// only, nothing shows without a global menu). Each item mirrors one of the
// window's actions. With `withShortcuts` the items hold the actions' keys
// (KeyedAction in Main.qml then leaves its own shortcut empty: only one of
// the two may hold it, or neither fires).
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import Qt.labs.platform as Platform
import net.eterneon.atlas.notepad

Platform.MenuBar {
    id: bar

    required property var actions
    property bool withShortcuts: false

    signal openRecent(string path)

    component Item: Platform.MenuItem {
        required property QQC2.Action action
        text: action.text
        enabled: action.enabled
        checkable: action.checkable
        checked: action.checked
        shortcut: bar.withShortcuts ? action.keys : undefined
        onTriggered: action.trigger()
    }
    component Separator: Platform.MenuSeparator {}

    Platform.Menu {
        title: qsTr("&File")
        Item { action: bar.actions.newTab }
        Item { action: bar.actions.newWindow }
        Item { action: bar.actions.open }
        Platform.Menu {
            id: recent
            title: qsTr("Open Recent")
            enabled: App.recentFiles.length > 0

            Instantiator {
                model: App.recentFiles
                delegate: Platform.MenuItem {
                    required property string modelData
                    text: App.displayPath(modelData)
                    onTriggered: bar.openRecent(modelData)
                }
                onObjectAdded: (index, object) => recent.insertItem(index, object)
                onObjectRemoved: (index, object) => recent.removeItem(object)
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: qsTr("Clear List")
                onTriggered: App.clearRecentFiles()
            }
        }
        Separator {}
        Item { action: bar.actions.save }
        Item { action: bar.actions.saveAs }
        Item { action: bar.actions.saveAll }
        Separator {}
        Item { action: bar.actions.openWith }
        Item { action: bar.actions.showInFolder }
        Item { action: bar.actions.copyLocation }
        Item { action: bar.actions.properties }
        Separator {}
        Item { action: bar.actions.print }
        Separator {}
        Item { action: bar.actions.closeTab }
        Item { action: bar.actions.reopenTab }
        Item { action: bar.actions.closeWindow }
        Item { action: bar.actions.quit }
    }

    Platform.Menu {
        title: qsTr("&Edit")
        Item { action: bar.actions.undo }
        Item { action: bar.actions.redo }
        Separator {}
        Item { action: bar.actions.cut }
        Item { action: bar.actions.copy }
        Item { action: bar.actions.paste }
        Item { action: bar.actions.delete }
        Separator {}
        Item { action: bar.actions.find }
        Item { action: bar.actions.findNext }
        Item { action: bar.actions.findPrevious }
        Item { action: bar.actions.replace }
        Item { action: bar.actions.goTo }
        Separator {}
        Item { action: bar.actions.selectAll }
        Item { action: bar.actions.timeDate }
        Item { action: bar.actions.font }
    }

    Platform.Menu {
        title: qsTr("&Code")
        Item { action: bar.actions.toggleComment }
        Item { action: bar.actions.indent }
        Item { action: bar.actions.outdent }
        Separator {}
        Item { action: bar.actions.sortLines }
        Item { action: bar.actions.upperCase }
        Item { action: bar.actions.lowerCase }
        Item { action: bar.actions.titleCase }
        Item { action: bar.actions.trimSpaces }
        Separator {}
        Item { action: bar.actions.codeLineNumbers }
    }

    Platform.Menu {
        title: qsTr("F&ormat")
        Item { action: bar.actions.heading1 }
        Item { action: bar.actions.heading2 }
        Item { action: bar.actions.heading3 }
        Item { action: bar.actions.heading4 }
        Item { action: bar.actions.heading5 }
        Item { action: bar.actions.heading6 }
        Item { action: bar.actions.bodyText }
        Separator {}
        Item { action: bar.actions.bold }
        Item { action: bar.actions.italic }
        Item { action: bar.actions.strikethrough }
        Item { action: bar.actions.code }
        Item { action: bar.actions.link }
        Separator {}
        Item { action: bar.actions.bulletList }
        Item { action: bar.actions.numberedList }
        Item { action: bar.actions.checklist }
        Item { action: bar.actions.quote }
        Separator {}
        Item { action: bar.actions.clearFormatting }
    }

    Platform.Menu {
        title: qsTr("&View")
        Platform.Menu {
            title: qsTr("Zoom")
            Item { action: bar.actions.zoomIn }
            Item { action: bar.actions.zoomOut }
            Item { action: bar.actions.zoomReset }
        }
        Separator {}
        Item { action: bar.actions.toggleFormatted }
        Item { action: bar.actions.toolbar }
        Item { action: bar.actions.statusBar }
        Item { action: bar.actions.lineNumbers }
        Item { action: bar.actions.wordWrap }
        Separator {}
        Item { action: bar.actions.settings }
    }

    Platform.Menu {
        title: qsTr("&Help")
        Item { action: bar.actions.keyboardShortcuts }
        Item { action: bar.actions.about }
    }
}
