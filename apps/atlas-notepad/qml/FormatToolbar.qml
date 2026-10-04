// The Markdown formatting toolbar, under the tabs. It acts on the current
// tab's MarkdownEditor through the window's actions, so the toolbar, the
// menus and the shortcuts do the same thing.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Atlas.Ui

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
        property QQC2.Action action
        text: action.text.replace("&", "")
        shortcutText: action.shortcut ? action.shortcut.toString() : ""
        enabled: action.enabled
        icon.source: Qt.resolvedUrl("../icons/" + iconName + ".svg")
        onClicked: action.trigger()
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Kirigami.Units.largeSpacing
        anchors.rightMargin: Kirigami.Units.largeSpacing
        spacing: 2

        Button {
            iconName: "heading1"
            action: bar.actions.heading1
            checked: bar.heading === 1
        }
        Button {
            iconName: "heading2"
            action: bar.actions.heading2
            checked: bar.heading === 2
        }
        Button {
            iconName: "paragraph"
            action: bar.actions.bodyText
        }
        Separator {}
        Button {
            iconName: "bold"
            action: bar.actions.bold
        }
        Button {
            iconName: "italic"
            action: bar.actions.italic
        }
        Button {
            iconName: "strikethrough"
            action: bar.actions.strikethrough
        }
        Button {
            iconName: "code"
            action: bar.actions.code
        }
        Button {
            iconName: "link-add"
            action: bar.actions.link
        }
        Separator {}
        Button {
            iconName: "bullet-list"
            action: bar.actions.bulletList
        }
        Button {
            iconName: "numbered-list"
            action: bar.actions.numberedList
        }
        Button {
            iconName: "task"
            action: bar.actions.checklist
        }
        Button {
            iconName: "quote"
            action: bar.actions.quote
        }
        Item {
            Layout.fillWidth: true
        }
        Button {
            iconName: "text"
            action: bar.actions.toggleFormatted
            checked: !bar.formatted
            text: bar.formatted ? qsTr("Show Markdown Syntax") : qsTr("Show Formatting")
        }
    }
}
