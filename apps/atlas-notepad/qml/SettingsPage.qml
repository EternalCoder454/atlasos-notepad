// Settings, over the editor (as in Windows 11 Notepad), with About at the end.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Atlas.Ui
import net.eterneon.atlas.notepad

AtlasPage {
    id: page

    readonly property Settings settings: App.settings

    title: qsTr("Settings")

    Section {
        title: qsTr("Font")
        footer: qsTr("Used for plain text and the Markdown syntax view. The formatted view uses your system font at this size.")

        SectionRow {
            title: qsTr("Family")
            QQC2.ComboBox {
                id: family
                Accessible.name: qsTr("Font family")
                Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                model: Qt.fontFamilies()
                currentIndex: model.indexOf(page.settings.font.family)
                // A family that isn't installed still shows its name.
                displayText: currentIndex < 0 ? page.settings.font.family : currentText
                onActivated: index => page.settings.font = Qt.font({
                    family: family.textAt(index),
                    pointSize: page.settings.font.pointSize
                })
            }
        }
        SectionRow {
            title: qsTr("Size")
            QQC2.SpinBox {
                Accessible.name: qsTr("Font size")
                from: 6
                to: 72
                value: Math.round(page.settings.font.pointSize)
                editable: true
                onValueModified: page.settings.font = Qt.font({
                    family: page.settings.font.family,
                    styleName: page.settings.font.styleName,
                    pointSize: value
                })
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            padding: Kirigami.Units.largeSpacing
            leftPadding: Kirigami.Units.gridUnit
            text: qsTr("The quick brown fox jumps over the lazy dog")
            font: page.settings.font
            elide: Text.ElideRight
            Accessible.name: qsTr("Preview: %1").arg(text)
        }
    }

    Section {
        title: qsTr("Text")

        SectionRow {
            title: qsTr("Word wrap")
            subtitle: qsTr("Long lines continue on the next line instead of scrolling sideways")
            showSwitch: true
            switchChecked: page.settings.wordWrap
            onSwitchToggled: checked => page.settings.wordWrap = checked
        }
        SectionRow {
            title: qsTr("Line numbers")
            showSwitch: true
            switchChecked: page.settings.lineNumbers
            onSwitchToggled: checked => page.settings.lineNumbers = checked
        }
        SectionRow {
            title: qsTr("Status bar")
            showSwitch: true
            switchChecked: page.settings.statusBar
            onSwitchToggled: checked => page.settings.statusBar = checked
        }
    }

    Section {
        title: qsTr("Markdown")
        footer: qsTr("Markdown files (.md) can show their formatting, such as headings, bold text and lists, instead of the symbols that make it. The file itself stays plain text.")

        SectionRow {
            title: qsTr("Formatting")
            subtitle: qsTr("Show Markdown files and new tabs with formatting")
            showSwitch: true
            switchChecked: page.settings.formatting
            onSwitchToggled: checked => page.settings.formatting = checked
        }
        SectionRow {
            title: qsTr("Open files formatted")
            subtitle: qsTr("Off: files open showing the Markdown syntax")
            enabled: page.settings.formatting
            showSwitch: true
            switchChecked: page.settings.openMarkdownFormatted
            onSwitchToggled: checked => page.settings.openMarkdownFormatted = checked
        }
        SectionRow {
            title: qsTr("Formatting toolbar")
            enabled: page.settings.formatting
            showSwitch: true
            switchChecked: page.settings.formattingToolbar
            onSwitchToggled: checked => page.settings.formattingToolbar = checked
        }
    }

    Section {
        title: qsTr("When Notepad Starts")

        SectionRow {
            title: qsTr("Continue previous session")
            subtitle: qsTr("Tabs come back as you left them, unsaved changes too")
            clickable: true
            radio: true
            checkmark: page.settings.continueSession
            Accessible.role: Accessible.RadioButton
            Accessible.checked: checkmark
            onClicked: page.settings.continueSession = true
        }
        SectionRow {
            title: qsTr("Start a new session")
            subtitle: qsTr("Closing a window asks about unsaved changes")
            clickable: true
            radio: true
            checkmark: !page.settings.continueSession
            Accessible.role: Accessible.RadioButton
            Accessible.checked: checkmark
            onClicked: page.settings.continueSession = false
        }
    }

    Section {
        title: qsTr("Opening Files")

        SectionRow {
            title: qsTr("Open in a new tab")
            clickable: true
            radio: true
            checkmark: !page.settings.openInNewWindow
            Accessible.role: Accessible.RadioButton
            Accessible.checked: checkmark
            onClicked: page.settings.openInNewWindow = false
        }
        SectionRow {
            title: qsTr("Open in a new window")
            clickable: true
            radio: true
            checkmark: page.settings.openInNewWindow
            Accessible.role: Accessible.RadioButton
            Accessible.checked: checkmark
            onClicked: page.settings.openInNewWindow = true
        }
    }

    Section {
        title: qsTr("Drawing")
        footer: qsTr("Notepad draws its window with the processor, which starts faster and uses less memory. Takes effect the next time Notepad opens.")

        SectionRow {
            title: qsTr("Use the graphics card to draw the window")
            showSwitch: true
            switchChecked: page.settings.gpuRendering
            onSwitchToggled: checked => page.settings.gpuRendering = checked
        }
    }

    Section {
        title: qsTr("About")
        footer: qsTr("Notepad collects nothing and needs no account.")

        SectionRow {
            title: qsTr("Notepad")
            value: qsTr("Version %1").arg(App.version)
            iconName: "accessories-text-editor"
        }
        SectionRow {
            title: qsTr("License")
            value: qsTr("MIT")
        }
        SectionRow {
            title: qsTr("Made by")
            value: qsTr("Eterneon")
        }
        SectionRow {
            title: qsTr("Project Page")
            chevron: true
            onClicked: Qt.openUrlExternally("https://github.com/EternalCoder454/atlasos-notepad")
        }
    }
}
