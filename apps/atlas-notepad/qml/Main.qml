// Spike S1: one Markdown document in the Formatted view, with the formatting
// toolbar. Tabs, the status bar, the global menu and AtlasWindow come in S4.
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import net.eterneon.atlas.notepad

QQC2.ApplicationWindow {
    id: window

    // The caret line's heading level, for the toolbar.
    readonly property int heading: {
        edit.length; // re-read on edits too
        return md.headingAt(edit.cursorPosition);
    }

    width: 960
    height: 720
    visible: true
    title: qsTr("Notepad")
    color: Kirigami.Theme.backgroundColor

    header: Item {
        implicitHeight: bar.implicitHeight + Kirigami.Units.smallSpacing * 2

        RowLayout {
            id: bar
            anchors.fill: parent
            anchors.leftMargin: Kirigami.Units.largeSpacing
            anchors.rightMargin: Kirigami.Units.largeSpacing
            spacing: 2

            ToolIcon {
                iconName: "heading1"
                text: qsTr("Heading 1")
                checked: window.heading === 1
                onClicked: md.setHeading(window.heading === 1 ? 0 : 1)
            }
            ToolIcon {
                iconName: "heading2"
                text: qsTr("Heading 2")
                checked: window.heading === 2
                onClicked: md.setHeading(window.heading === 2 ? 0 : 2)
            }
            ToolIcon {
                iconName: "paragraph"
                text: qsTr("Body Text")
                onClicked: md.setHeading(0)
            }
            Rectangle {
                Layout.leftMargin: Kirigami.Units.smallSpacing
                Layout.rightMargin: Kirigami.Units.smallSpacing
                implicitWidth: 1
                implicitHeight: Kirigami.Units.iconSizes.small
                color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
            }
            ToolIcon {
                iconName: "bold"
                text: qsTr("Bold")
                onClicked: md.toggleInline("**")
            }
            ToolIcon {
                iconName: "italic"
                text: qsTr("Italic")
                onClicked: md.toggleInline("*")
            }
            ToolIcon {
                iconName: "strikethrough"
                text: qsTr("Strikethrough")
                onClicked: md.toggleInline("~~")
            }
            ToolIcon {
                iconName: "code"
                text: qsTr("Code")
                onClicked: md.toggleInline("`")
            }
            Rectangle {
                Layout.leftMargin: Kirigami.Units.smallSpacing
                Layout.rightMargin: Kirigami.Units.smallSpacing
                implicitWidth: 1
                implicitHeight: Kirigami.Units.iconSizes.small
                color: Qt.alpha(Kirigami.Theme.textColor, 0.15)
            }
            ToolIcon {
                iconName: "bullet-list"
                text: qsTr("Bulleted List")
                onClicked: md.toggleBlock("bullet")
            }
            ToolIcon {
                iconName: "task"
                text: qsTr("Checklist")
                onClicked: md.toggleBlock("task")
            }
            ToolIcon {
                iconName: "quote"
                text: qsTr("Quote")
                onClicked: md.toggleBlock("quote")
            }
            Item {
                Layout.fillWidth: true
            }
            ToolIcon {
                iconName: "text"
                text: md.formatted ? qsTr("Show Markdown Syntax") : qsTr("Show Formatting")
                checked: !md.formatted
                onClicked: md.formatted = !md.formatted
            }
        }
    }

    Flickable {
        id: view
        objectName: "view"
        anchors.fill: parent
        contentWidth: width
        contentHeight: edit.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick

        function ensureVisible(r) {
            if (contentY >= r.y)
                contentY = r.y;
            else if (contentY + height <= r.y + r.height)
                contentY = r.y + r.height - height;
        }

        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

        TextEdit {
            id: edit
            objectName: "editor"
            width: view.width
            height: Math.max(implicitHeight, view.height)
            focus: true
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            persistentSelection: true
            font: Kirigami.Theme.defaultFont
            color: Kirigami.Theme.textColor
            selectionColor: Kirigami.Theme.highlightColor
            selectedTextColor: Kirigami.Theme.highlightedTextColor
            leftPadding: Kirigami.Units.gridUnit
            rightPadding: Kirigami.Units.gridUnit
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: Kirigami.Units.largeSpacing
            onCursorRectangleChanged: view.ensureVisible(cursorRectangle)

            // Behind the text, over the visible part only.
            MarkdownDecorations {
                z: -1
                editor: md
                y: view.contentY
                width: edit.width
                height: view.height
            }
        }
    }

    MarkdownEditor {
        id: md
        textEdit: edit
        font: edit.font
        textColor: Kirigami.Theme.textColor
        dimColor: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, 0.55)
        linkColor: Kirigami.Theme.linkColor
        codeColor: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, 0.07)
        accentColor: Kirigami.Theme.highlightColor
    }

    Shortcut {
        sequence: "Ctrl+B"
        onActivated: md.toggleInline("**")
    }
    Shortcut {
        sequence: "Ctrl+I"
        onActivated: md.toggleInline("*")
    }
    Shortcut {
        sequence: "Ctrl+Shift+M"
        onActivated: md.formatted = !md.formatted
    }
    Repeater {
        model: 6
        delegate: Item {
            required property int index
            Shortcut {
                sequence: "Ctrl+" + (index + 1)
                onActivated: md.setHeading(index + 1)
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+Shift+0"
        onActivated: md.setHeading(0)
    }
}
