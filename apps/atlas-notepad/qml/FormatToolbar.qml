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

    // The same height class as the top bar.
    implicitHeight: Math.round(Kirigami.Units.gridUnit * 1.8)

    component Separator: Rectangle {
        Layout.leftMargin: Kirigami.Units.smallSpacing
        Layout.rightMargin: Kirigami.Units.smallSpacing
        implicitWidth: 1
        implicitHeight: Kirigami.Units.iconSizes.small
        color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
    }
    component Button: SymbolButton {
        property string iconName
        // Not `action`: AbstractButton's own would trigger it a second time.
        property QQC2.Action command
        text: command.text.replace("&", "")
        shortcutText: App.shortcutText(command.keys ?? command.shortcut)
        enabled: command.enabled
        symbol: iconName
        onClicked: command.trigger()
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Kirigami.Units.smallSpacing
        anchors.rightMargin: Kirigami.Units.smallSpacing
        spacing: 2

        Button {
            iconName: "format_h1"
            command: bar.actions.heading1
            checked: bar.heading === 1
        }
        Button {
            iconName: "format_h2"
            command: bar.actions.heading2
            checked: bar.heading === 2
        }
        Button {
            iconName: "format_paragraph"
            command: bar.actions.bodyText
        }
        Separator {}
        Button {
            iconName: "format_bold"
            command: bar.actions.bold
        }
        Button {
            iconName: "format_italic"
            command: bar.actions.italic
        }
        Button {
            iconName: "format_strikethrough"
            command: bar.actions.strikethrough
        }
        Button {
            iconName: "code"
            command: bar.actions.code
        }
        Button {
            iconName: "add_link"
            command: bar.actions.link
        }
        Separator {}
        Button {
            iconName: "format_list_bulleted"
            command: bar.actions.bulletList
        }
        Button {
            iconName: "format_list_numbered"
            command: bar.actions.numberedList
        }
        Button {
            iconName: "checklist"
            command: bar.actions.checklist
        }
        Button {
            iconName: "format_quote"
            command: bar.actions.quote
        }
        Item {
            Layout.fillWidth: true
        }
        Button {
            iconName: "text_format"
            command: bar.actions.toggleFormatted
            checked: !bar.formatted
            text: bar.formatted ? qsTr("Show Markdown Syntax") : qsTr("Show Formatting")
        }
    }
}
