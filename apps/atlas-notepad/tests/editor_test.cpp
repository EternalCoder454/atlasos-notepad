// MarkdownEditor's edits on a real TextEdit: keys go through its event filter
// and toolbar calls through its invokables, and the test reads back the text.
#include "markdown.h"

#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <QTest>

#include <memory>

class EditorTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    std::unique_ptr<QQuickItem> m_edit;
    std::unique_ptr<MarkdownEditor> m_editor;

    // A fresh TextEdit holding `text`, read and formatted.
    void open(const QString &text)
    {
        m_editor.reset();
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }", QUrl());
        m_edit.reset(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(m_edit, qPrintable(component.errorString()));
        m_edit->setProperty("text", text);
        m_editor = std::make_unique<MarkdownEditor>();
        m_editor->setTextEdit(m_edit.get());
        m_editor->setFormatted(true);
        QCoreApplication::processEvents();
        m_editor->rehighlightNow();
    }
    QString text() const
    {
        return m_edit->property("text").toString();
    }
    void place(int position)
    {
        m_edit->setProperty("cursorPosition", position);
    }
    void select(int start, int end)
    {
        QMetaObject::invokeMethod(m_edit.get(), "select", Q_ARG(int, start), Q_ARG(int, end));
    }
    void press(int key)
    {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier, key == Qt::Key_Return ? QStringLiteral("\r") : QString());
        QCoreApplication::sendEvent(m_edit.get(), &event);
    }

private Q_SLOTS:
    void enterContinuesList()
    {
        open(QStringLiteral("- a"));
        place(3);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("- a\n- "));
    }

    void enterOnEmptyItemEndsList()
    {
        open(QStringLiteral("- a\n- "));
        place(6);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("- a\n"));
    }

    // A quoted rule or heading has no content after its content start, but it
    // isn't an empty item: Enter continues the quote.
    void enterAfterQuotedRuleKeepsIt()
    {
        open(QStringLiteral("> ---"));
        place(5);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("> ---\n> "));
    }

    // Only spaces and tabs make an item empty, as in markdown.rs.
    void enterAfterNoBreakSpaceKeepsIt()
    {
        const QString item = QStringLiteral("- ") + QChar(0x00a0);
        open(item);
        place(3);
        press(Qt::Key_Return);
        // The text property reads a no-break space as a space: ask the document.
        const QTextDocument *doc = m_edit->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        QCOMPARE(doc->blockCount(), 2);
        QCOMPARE(doc->firstBlock().text(), item);
        QCOMPARE(doc->lastBlock().text(), QStringLiteral("- "));
    }

    void backspaceLastCharacterRemovesSpan()
    {
        open(QStringLiteral("x **a** y"));
        place(5);
        press(Qt::Key_Backspace);
        QCOMPARE(text(), QStringLiteral("x  y"));
    }

    // Hidden ranges of neighbouring spans run together; only this span goes.
    void backspaceLeavesNextSpan_data()
    {
        QTest::addColumn<QString>("before");
        QTest::addColumn<int>("position");
        QTest::addColumn<QString>("after");
        QTest::newRow("bold then link") << QStringLiteral("**a**[b](u)") << 3 << QStringLiteral("[b](u)");
        QTest::newRow("italic then italic") << QStringLiteral("*a*_b_") << 2 << QStringLiteral("_b_");
    }
    void backspaceLeavesNextSpan()
    {
        QFETCH(QString, before);
        QFETCH(int, position);
        QFETCH(QString, after);
        open(before);
        place(position);
        press(Qt::Key_Backspace);
        QCOMPARE(text(), after);
    }

    void backspaceKeepsLinkUrl()
    {
        open(QStringLiteral("[a](u)"));
        place(2);
        press(Qt::Key_Backspace);
        QCOMPARE(text(), QStringLiteral("[](u)"));
    }

    void deleteLeavesNextSpan()
    {
        open(QStringLiteral("**a**[b](u)"));
        place(2);
        press(Qt::Key_Delete);
        QCOMPARE(text(), QStringLiteral("[b](u)"));
    }

    void toolbarLeavesCodeAlone()
    {
        const QString code = QStringLiteral("```\ncode\n```");
        open(code);
        select(0, int(code.size()));
        m_editor->toggleBlock(QStringLiteral("bullet"));
        QCOMPARE(text(), code);
        m_editor->setHeading(1);
        QCOMPARE(text(), code);
        m_editor->toggleBlock(QStringLiteral("quote"));
        QCOMPARE(text(), code);
    }

    void bulletReplacesHeading()
    {
        open(QStringLiteral("# Title"));
        place(4);
        m_editor->toggleBlock(QStringLiteral("bullet"));
        QCOMPARE(text(), QStringLiteral("- Title"));
    }

    void headingReplacesBullet()
    {
        open(QStringLiteral("- item"));
        place(4);
        m_editor->setHeading(2);
        QCOMPARE(text(), QStringLiteral("## item"));
    }

    void normalTextKeepsLists()
    {
        open(QStringLiteral("# Title\n- item"));
        select(0, 14);
        m_editor->setHeading(0);
        QCOMPARE(text(), QStringLiteral("Title\n- item"));
    }

    // Whole lines selected with their newline end at the start of the next.
    void selectionEndingAtLineStartLeavesThatLine()
    {
        open(QStringLiteral("one\ntwo"));
        select(0, 4);
        m_editor->setHeading(1);
        QCOMPARE(text(), QStringLiteral("# one\ntwo"));
        select(0, 6);
        m_editor->toggleBlock(QStringLiteral("quote"));
        QCOMPARE(text(), QStringLiteral("> # one\ntwo"));
    }

    void readOnlyIsNotEdited()
    {
        open(QStringLiteral("- a"));
        m_edit->setProperty("readOnly", true);
        place(3);
        press(Qt::Key_Return);
        press(Qt::Key_Backspace);
        m_editor->setHeading(1);
        m_editor->toggleBlock(QStringLiteral("task"));
        m_editor->toggleInline(QStringLiteral("**"));
        QCOMPARE(text(), QStringLiteral("- a"));
    }

    // One undo takes back one toolbar edit.
    void toolbarEditIsOneUndo()
    {
        open(QStringLiteral("one\ntwo"));
        select(0, 7);
        m_editor->toggleBlock(QStringLiteral("numbered"));
        QCOMPARE(text(), QStringLiteral("1. one\n2. two"));
        QMetaObject::invokeMethod(m_edit.get(), "undo");
        QCOMPARE(text(), QStringLiteral("one\ntwo"));
    }

    void enterRenumbersTheRest()
    {
        open(QStringLiteral("1. a\n2. b\n   - c\n3. d"));
        place(4);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("1. a\n2. \n3. b\n   - c\n4. d"));
    }

    void clearFormattingKeepsText()
    {
        open(QStringLiteral("# A **b** [c](u) \\*"));
        select(0, int(text().size()));
        m_editor->clearFormatting();
        QCOMPARE(text(), QStringLiteral("A b c \\*"));
    }

    void clearFormattingTakesMarkersAroundSelection()
    {
        open(QStringLiteral("x **bold** y"));
        select(4, 8); // "bold", the visible letters
        m_editor->clearFormatting();
        QCOMPARE(text(), QStringLiteral("x bold y"));
    }

    void insertLinkWrapsSelection()
    {
        open(QStringLiteral("see here"));
        select(4, 8);
        m_editor->insertLink(QStringLiteral("here"), QStringLiteral("https://a.b/c d"));
        QCOMPARE(text(), QStringLiteral("see [here](<https://a.b/c d>)"));
    }

    void insertLinkOnLinkChangesAddress()
    {
        open(QStringLiteral("[t](old)"));
        place(2); // after the visible "t"
        m_editor->insertLink(QString(), QStringLiteral("new"));
        QCOMPARE(text(), QStringLiteral("[t](new)"));
    }
};

QTEST_MAIN(EditorTest)
#include "editor_test.moc"
