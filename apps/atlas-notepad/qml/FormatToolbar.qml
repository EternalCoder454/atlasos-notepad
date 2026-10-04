// The Markdown formatting toolbar, under the tabs. It acts on the current
// tab's MarkdownEditor through the window's actions, so the toolbar, the
// menus and the shortcuts do the same thing.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Atlas.Ui
import net.eterneon.atlas.notepad

Item {
    id: bar

    // The window's actions, by name (Main.qml `formatActions`).
    required property var actions
    required property int heading
    required property bool formatted

    implicitHeight: row.implicitHeight + Kirigami.Units.smallSpacing * 2

    component Separator: Rectangle {
        Layout.leftMargin: Kirigami.Units.smallSpacing
        Layout.rightMargin: Kirigami.Units.smallSpacing
        implicitWidth: 1
        implicitHeight: Kirigami.Units.iconSizes.small
        color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
    }
    component Button: ToolbarButton {
        property string iconName
        // Not `action`: AbstractButton's own would trigger it a second time.
        property QQC2.Action command
        text: command.text.replace("&", "")
        shortcutText: App.shortcutText(command.keys ?? command.shortcut)
        enabled: command.enabled
        icon.source: Qt.resolvedUrl("../icons/" + iconName + ".svg")
        onClicked: command.trigger()
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Kirigami.Units.largeSpacing
        anchors.rightMargin: Kirigami.Units.largeSpacing
        spacing: 2

        Button {
            iconName: "heading1"
            command: bar.actions.heading1
            checked: bar.heading === 1
        }
        Button {
            iconName: "heading2"
            command: bar.actions.heading2
            checked: bar.heading === 2
        }
        Button {
            iconName: "paragraph"
            command: bar.actions.bodyText
        }
        Separator {}
        Button {
            iconName: "bold"
            command: bar.actions.bold
        }
        Button {
            iconName: "italic"
            command: bar.actions.italic
        }
        Button {
            iconName: "strikethrough"
            command: bar.actions.strikethrough
        }
        Button {
            iconName: "code"
            command: bar.actions.code
        }
        Button {
            iconName: "link-add"
            command: bar.actions.link
        }
        Separator {}
        Button {
            iconName: "bullet-list"
            command: bar.actions.bulletList
        }
        Button {
            iconName: "numbered-list"
            command: bar.actions.numberedList
        }
        Button {
            iconName: "task"
            command: bar.actions.checklist
        }
        Button {
            iconName: "quote"
            command: bar.actions.quote
        }
        Item {
            Layout.fillWidth: true
        }
        Button {
            iconName: "text"
            command: bar.actions.toggleFormatted
            checked: !bar.formatted
            text: bar.formatted ? qsTr("Show Markdown Syntax") : qsTr("Show Formatting")
        }
    }
}
