// The code file's language and indentation, in a small card that opens beside
// the capsule button or above a status cell. It changes the document's own
// settings, which the editor follows.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Telamon.Ui
import net.eterneon.atlas.notepad

TelamonPopover {
    id: popover

    // The tab's document and its CodeEditor (for the language list).
    property Document document: null
    property CodeEditor editor: null
    readonly property var languages: editor ? editor.languages() : []

    // "left": beside the target, centred on it, instead of TelamonPopover's own
    // below-or-above placement (the capsule is a strip at the window's edge).
    property bool _left: false

    // Opens above (a status cell) or to the left of `item`.
    function openAt(item, side) {
        target = item;
        _left = side === "left";
        showArrow = !_left;
        open();
    }
    function _placeLeft(): void {
        const t = popover.target;
        const p = popover.parent;
        if (!popover._left || !t || !p) {
            return;
        }
        const origin = t.mapToItem(p, 0, 0);
        const gap = Kirigami.Units.smallSpacing;
        popover.x = Math.max(0, Math.min(origin.x - popover.width - gap, p.width - popover.width));
        popover.y = Math.max(0, Math.min(origin.y + (t.height - popover.height) / 2, p.height - popover.height));
    }
    onAboutToShow: _placeLeft()
    onImplicitWidthChanged: if (visible) _placeLeft()
    onImplicitHeightChanged: if (visible) _placeLeft()

    ColumnLayout {
        Layout.preferredWidth: Kirigami.Units.gridUnit * 19
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Language and indentation")
        spacing: Kirigami.Units.largeSpacing

        TelamonLabel {
            text: qsTr("Language")
            textStyle: TelamonLabel.Heading
        }
        TelamonComboBox {
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

        TelamonLabel {
            text: qsTr("Indentation")
            textStyle: TelamonLabel.Heading
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing
            TelamonSegmentedControl {
                id: indentSwitch
                Accessible.name: qsTr("Indent with")
                model: [
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
                onActivated: index => {
                    if (popover.document) {
                        popover.document.insertSpaces = index === 0;
                    }
                    // The control assigned currentIndex itself; follow the document again.
                    indentSwitch.currentIndex = Qt.binding(() => popover.document && !popover.document.insertSpaces ? 1 : 0);
                }
            }
            Item {
                Layout.fillWidth: true
            }
            TelamonLabel {
                text: qsTr("Width")
            }
            TelamonSpinBox {
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
