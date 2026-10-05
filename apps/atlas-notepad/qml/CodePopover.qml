// The code file's language and indentation, in a small card that opens beside
// the capsule button or above a status cell. It changes the document's own
// settings, which the editor follows.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Atlas.Ui
import net.eterneon.atlas.notepad

QQC2.Popup {
    id: popover

    // The tab's document and its CodeEditor (for the language list).
    property Document document: null
    property CodeEditor editor: null
    readonly property var languages: editor ? editor.languages() : []

    // Opens above (a status cell) or to the left of `item`.
    function openAt(item, side) {
        parent = item;
        const gap = Kirigami.Units.smallSpacing;
        if (side === "left") {
            x = Qt.binding(() => -popover.width - gap);
            y = Qt.binding(() => ((popover.parent ? popover.parent.height : 0) - popover.height) / 2);
        } else {
            // From the cell's left edge, pulled back to stay in the window.
            x = Qt.binding(() => {
                const overlay = QQC2.Overlay.overlay;
                const left = overlay && popover.parent ? popover.parent.mapToItem(overlay, 0, 0).x : 0;
                return overlay ? Math.min(0, overlay.width - popover.margins - left - popover.width) : 0;
            });
            y = Qt.binding(() => -popover.height - gap);
        }
        open();
    }

    modal: false
    // Kept inside the window, wherever it opens.
    margins: Kirigami.Units.smallSpacing
    focus: true
    closePolicy: QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutside
    padding: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
    width: Kirigami.Units.gridUnit * 19

    background: Rectangle {
        radius: 12
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.16)
    }

    contentItem: ColumnLayout {
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Language and indentation")
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            text: qsTr("Language")
            font.bold: true
        }
        AtlasComboBox {
            id: language
            Layout.fillWidth: true
            Accessible.name: qsTr("Language")
            model: popover.languages
            currentIndex: popover.document ? popover.languages.indexOf(popover.document.language) : -1
            displayText: currentIndex < 0 && popover.document ? popover.document.language : currentText
            // The list takes a typed letter, which jumps to the next language starting with it.
            onActivated: index => {
                if (popover.document) {
                    popover.document.language = popover.languages[index];
                }
            }
        }

        QQC2.Label {
            text: qsTr("Indentation")
            font.bold: true
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing
            SegmentedPill {
                items: [
                    {
                        text: qsTr("Spaces"),
                        toolTip: qsTr("Indent with spaces")
                    },
                    {
                        text: qsTr("Tabs"),
                        toolTip: qsTr("Indent with tabs")
                    }
                ]
                currentIndex: popover.document && !popover.document.insertSpaces ? 1 : 0
                onChosen: index => {
                    if (popover.document) {
                        popover.document.insertSpaces = index === 0;
                    }
                }
            }
            Item {
                Layout.fillWidth: true
            }
            QQC2.Label {
                text: qsTr("Width")
            }
            AtlasSpinBox {
                Accessible.name: qsTr("Indent width")
                from: 1
                to: 8
                value: popover.document ? popover.document.indentWidth : 4
                editable: true
                onValueModified: {
                    if (popover.document) {
                        popover.document.indentWidth = value;
                    }
                }
            }
        }
    }
}
