// "Do you want to save changes to notes.md?" with Save, Don't Save and
// Cancel, in ConfirmDialog's look (which has two buttons only).
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import Atlas.Ui

QQC2.Popup {
    id: dialog

    property string fileName
    // Why it asks although the session would keep the changes, if it can't.
    property string note

    signal save
    signal discard
    signal cancel

    parent: QQC2.Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: QQC2.Popup.CloseOnEscape
    width: Math.min(parent ? parent.width - Kirigami.Units.gridUnit * 2 : 0, Kirigami.Units.gridUnit * 25)
    padding: Math.round(Kirigami.Units.gridUnit * 1.3)
    onOpened: saveButton.forceActiveFocus()
    // Escape and the window manager's close count as Cancel.
    property bool answered: false
    onAboutToShow: answered = false
    onClosed: {
        if (!answered) {
            cancel();
        }
    }
    function answer(which) {
        answered = true;
        close();
        which();
    }

    QQC2.Overlay.modal: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.35)
    }
    background: Rectangle {
        radius: 14
        color: Kirigami.Theme.backgroundColor
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.16)
    }

    contentItem: ColumnLayout {
        Accessible.role: Accessible.Dialog
        Accessible.name: title.text
        spacing: Kirigami.Units.largeSpacing

        QQC2.Label {
            id: title
            Layout.fillWidth: true
            text: qsTr("Do you want to save changes to %1?").arg(dialog.fileName)
            font.bold: true
            font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.15
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            Accessible.role: Accessible.Heading
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: dialog.note.length > 0
            text: dialog.note
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            opacity: 0.8
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.largeSpacing
            Item {
                Layout.fillWidth: true
            }
            SecondaryButton {
                text: qsTr("Cancel")
                onClicked: dialog.answer(dialog.cancel)
            }
            SecondaryButton {
                text: qsTr("Don't Save")
                onClicked: dialog.answer(dialog.discard)
            }
            PrimaryButton {
                id: saveButton
                text: qsTr("Save")
                onClicked: dialog.answer(dialog.save)
            }
        }
    }
}
