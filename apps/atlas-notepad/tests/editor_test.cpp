// MarkdownEditor's edits on a real TextEdit: keys go through its event filter
// and toolbar calls through its invokables, and the test reads back the text.
#include "markdown.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
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

    QTextBlock blockAt(int blockNumber) const
    {
        return m_edit->property("textDocument").value<QQuickTextDocument *>()->textDocument()->findBlockByNumber(blockNumber);
    }
    // Whether every character of a line is drawn invisibly (a fence line the
    // Formatted view hides). False when the line has no formats.
    bool lineHidden(int blockNumber) const
    {
        const auto formats = blockAt(blockNumber).layout()->formats();
        if (formats.isEmpty()) {
            return false;
        }
        for (const auto &r : formats) {
            if (r.format.foreground().color().alpha() != 0) {
                return false;
            }
        }
        return true;
    }
    // Whether the line has formats and all of them are the dim text colour.
    bool lineShown(int blockNumber) const
    {
        const auto formats = blockAt(blockNumber).layout()->formats();
        if (formats.isEmpty()) {
            return false;
        }
        for (const auto &r : formats) {
            if (r.format.foreground().color() != m_editor->dimColor()) {
                return false;
            }
        }
        return true;
    }
    void type(const QString &chars)
    {
        for (const QChar c : chars) {
            QKeyEvent event(QEvent::KeyPress, c == u'`' ? int(Qt::Key_QuoteLeft) : int(c.toLatin1()), Qt::NoModifier, QString(c));
            QCoreApplication::sendEvent(m_edit.get(), &event);
        }
    }
    int caret() const
    {
        return m_edit->property("cursorPosition").toInt();
    }
    QStringList labels(int height = 600) const
    {
        MarkdownDecorations deco;
        deco.setWidth(400);
        deco.setHeight(height);
        deco.setEditor(m_editor.get());
        return deco.labelsForTest();
    }

private Q_SLOTS:
    // Fence markers are hidden in the Formatted view, except on the line the
    // caret is on, and the text and the caret don't change by it.
    void fenceMarkersHiddenAwayFromCaret()
    {
        const QString md = QStringLiteral("```rust\nlet x;\n```\nafter");
        open(md);
        place(9);
        QVERIFY(lineHidden(0));
        QVERIFY(lineHidden(2));
        QCOMPARE(m_edit->property("cursorPosition").toInt(), 9);
        place(3);
        QVERIFY(lineShown(0));
        QVERIFY(lineHidden(2));
        QCOMPARE(m_edit->property("cursorPosition").toInt(), 3);
        place(md.indexOf(u"after"));
        QVERIFY(lineHidden(0));
        QVERIFY(lineHidden(2));
        place(md.indexOf(u"```\nafter") + 1);
        QVERIFY(lineHidden(0));
        QVERIFY(lineShown(2));
        QCOMPARE(text(), md);
        QCOMPARE(m_edit->property("lineCount").toInt(), 4);
    }

    void fenceMarkersShownInSyntaxView()
    {
        open(QStringLiteral("```rust\nlet x;\n```"));
        m_editor->setFormatted(false);
        QCoreApplication::processEvents();
        m_editor->rehighlightNow();
        place(9);
        QVERIFY(lineShown(0));
        QVERIFY(lineShown(2));
    }

    void typingAFenceShowsIt()
    {
        open(QStringLiteral("a\n"));
        place(2);
        for (const QChar c : QStringLiteral("```")) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_QuoteLeft, Qt::NoModifier, QString(c));
            QCoreApplication::sendEvent(m_edit.get(), &event);
        }
        QCOMPARE(text(), QStringLiteral("a\n```"));
        QVERIFY(lineShown(1));
    }

    void copyKeepsFences()
    {
        const QString md = QStringLiteral("```rust\nlet x;\n```");
        open(md);
        select(0, int(md.size()));
        QMetaObject::invokeMethod(m_edit.get(), "copy");
        QCOMPARE(QGuiApplication::clipboard()->text(), md);
    }

    void fenceLabelText_data()
    {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("label");
        QTest::newRow("language") << QStringLiteral("```rust") << QStringLiteral("rust");
        QTest::newRow("spaced") << QStringLiteral("  ~~~~  python title=x") << QStringLiteral("python");
        QTest::newRow("none") << QStringLiteral("```") << QString();
        QTest::newRow("blank") << QStringLiteral("```   ") << QString();
        QTest::newRow("symbols") << QStringLiteral("```c++") << QStringLiteral("c++");
        QTest::newRow("allowed punctuation") << QStringLiteral("```f#.x_y-z/w") << QStringLiteral("f#.x_y-z/w");
        QTest::newRow("control") << QStringLiteral("```ru\x01st") << QStringLiteral("ru");
        QTest::newRow("escape") << QStringLiteral("```\x1b[31m") << QString();
        QTest::newRow("bidi") << QStringLiteral("```ru\u202Est") << QStringLiteral("ru");
        QTest::newRow("only invisible") << QStringLiteral("```\u202E\u2066") << QString();
        QTest::newRow("lone surrogate") << QStringLiteral("```a\xD800z") << QStringLiteral("a");
        QTest::newRow("emoji ends it") << QStringLiteral("```rs\U0001F980x") << QStringLiteral("rs");
        QTest::newRow("zalgo") << QStringLiteral("```a\u0300\u0301\u0302\u0303b") << QStringLiteral("a");
        QTest::newRow("braille blank") << QStringLiteral("```\u2800") << QString();
        QTest::newRow("hangul filler") << QStringLiteral("```\u3164\u115F\u1160\uFFA0") << QString();
        QTest::newRow("blank inside") << QStringLiteral("```Saved\u2800successfully") << QStringLiteral("Saved");
        QTest::newRow("rmarkdown") << QStringLiteral("```{r}") << QStringLiteral("r");
        QTest::newRow("rmarkdown options") << QStringLiteral("```{python, echo=FALSE}") << QStringLiteral("python");
        QTest::newRow("exactly 64") << (QStringLiteral("```") + QString(64, u'x')) << QString(64, u'x');
        QTest::newRow("long") << (QStringLiteral("```") + QString(70, u'x')) << (QString(64, u'x') + QChar(0x2026));
        QTest::newRow("very long") << (QStringLiteral("```") + QString(5000, u'x')) << (QString(64, u'x') + QChar(0x2026));
    }
    void fenceLabelText()
    {
        QFETCH(QString, line);
        QFETCH(QString, label);
        QCOMPARE(MarkdownDecorations::fenceLabel(line), label);
    }

    void fenceRehidesAfterTypingAndLeaving()
    {
        open(QStringLiteral("a\n"));
        place(2);
        type(QStringLiteral("```"));
        QVERIFY(lineShown(1));
        place(0);
        QVERIFY(lineHidden(1));
    }

    void undoRedoOfTheCaretsFenceLine()
    {
        open(QStringLiteral("a\n"));
        place(2);
        type(QStringLiteral("```"));
        for (int i = 0; i < 6 && text() != QStringLiteral("a\n"); ++i) {
            QMetaObject::invokeMethod(m_edit.get(), "undo");
        }
        QCOMPARE(text(), QStringLiteral("a\n"));
        QCOMPARE(m_edit->property("lineCount").toInt(), 2);
        for (int i = 0; i < 6 && text() != QStringLiteral("a\n```"); ++i) {
            QMetaObject::invokeMethod(m_edit.get(), "redo");
        }
        QCOMPARE(text(), QStringLiteral("a\n```"));
        QCOMPARE(caret(), 5);
        QVERIFY(lineShown(1));
        place(0);
        QVERIFY(lineHidden(1));
    }

    void selectionSpanningFences()
    {
        const QString md = QStringLiteral("```rust\nlet x;\n```\nafter");
        open(md);
        select(0, int(md.size()));
        QVERIFY(lineHidden(0));
        QVERIFY(lineHidden(2));
        QMetaObject::invokeMethod(m_edit.get(), "copy");
        QCOMPARE(QGuiApplication::clipboard()->text(), md);
        // The caret end (the closing fence) shows its markers.
        select(2, 17);
        QVERIFY(lineShown(2));
        QMetaObject::invokeMethod(m_edit.get(), "copy");
        QCOMPARE(QGuiApplication::clipboard()->text(), md.mid(2, 15));
        QCOMPARE(text(), md);
    }

    void tildeAndUnclosedFences()
    {
        open(QStringLiteral("~~~py\ncode\n~~~"));
        place(8);
        QVERIFY(lineHidden(0));
        QVERIFY(lineHidden(2));
        QCOMPARE(labels(), QStringList{QStringLiteral("py")});
        open(QStringLiteral("```rs\ncode"));
        place(8);
        QVERIFY(lineHidden(0));
        QCOMPARE(labels(), QStringList{QStringLiteral("rs")});
        place(2);
        QVERIFY(lineShown(0));
        QVERIFY(labels().isEmpty());
    }

    void fenceThroughSyntaxViewAndBack()
    {
        const QString md = QStringLiteral("```rust\nlet x;\n```");
        open(md);
        place(3);
        QVERIFY(lineShown(0));
        m_editor->setFormatted(false);
        QCoreApplication::processEvents();
        m_editor->rehighlightNow();
        QVERIFY(lineShown(0));
        QVERIFY(lineShown(2));
        m_editor->setFormatted(true);
        QCoreApplication::processEvents();
        m_editor->rehighlightNow();
        QCOMPARE(caret(), 3);
        QVERIFY(lineShown(0));
        QVERIFY(lineHidden(2));
    }

    void labelShapeFollowsTheCaret()
    {
        open(QStringLiteral("```rust\nlet x;\n```\n\n```\nplain\n```"));
        place(10);
        QCOMPARE(labels(), QStringList{QStringLiteral("rust")});
        place(3);
        QVERIFY(labels().isEmpty());
        place(10);
        QCOMPARE(labels(), QStringList{QStringLiteral("rust")});
        m_editor->setFormatted(false);
        QCoreApplication::processEvents();
        QVERIFY(labels().isEmpty());
    }

    // Replacing the whole text moves the caret to 0 without a signal: the
    // first fence is the one that shows.
    void fenceAfterWholeTextReplace()
    {
        open(QStringLiteral("x"));
        place(0);
        m_edit->setProperty("text", QStringLiteral("```a\nx\n```"));
        QCoreApplication::processEvents();
        m_editor->rehighlightNow();
        QCOMPARE(caret(), 0);
        QVERIFY(lineShown(0));
        QVERIFY(lineHidden(2));
        place(0);
        QVERIFY(lineShown(0));
        QVERIFY(lineHidden(2));
        m_edit->setProperty("text", QStringLiteral("```b\ny\n```"));
        QCoreApplication::processEvents();
        QCOMPARE(caret(), 0);
        QVERIFY(lineShown(0));
        QVERIFY(lineHidden(2));
    }

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

    // Copy takes the Markdown as written, hidden markers included.
    void copyGivesMarkdown()
    {
        const QString md = QStringLiteral("# Head\n**bold** and [link](https://x.org)\n- [ ] item");
        open(md);
        select(0, int(md.size()));
        QMetaObject::invokeMethod(m_edit.get(), "copy");
        QCOMPARE(QGuiApplication::clipboard()->text(), md);
        select(7, 15);
        QMetaObject::invokeMethod(m_edit.get(), "copy");
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("**bold**"));
    }

    // An input method's preedit text isn't part of the document until it's
    // committed, and a commit inside a span keeps the span.
    void inputMethodCommitsIntoSpans()
    {
        open(QStringLiteral("**bold**\n- item"));
        place(4);
        QInputMethodEvent preedit(QStringLiteral("ni"), {});
        QCoreApplication::sendEvent(m_edit.get(), &preedit);
        QCOMPARE(text(), QStringLiteral("**bold**\n- item"));
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("你"));
        QCoreApplication::sendEvent(m_edit.get(), &commit);
        QCOMPARE(text(), QStringLiteral("**bo你ld**\n- item"));
        place(int(text().size()));
        QInputMethodEvent accent;
        accent.setCommitString(QStringLiteral("é"));
        QCoreApplication::sendEvent(m_edit.get(), &accent);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("**bo你ld**\n- itemé\n- "));
    }

    void enterRenumbersTheRest()
    {
        open(QStringLiteral("1. a\n2. b\n   - c\n3. d"));
        place(4);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("1. a\n2. \n3. b\n   - c\n4. d"));
    }

    void formatsAtReportsCaretAndSelectionFormats()
    {
        open(QStringLiteral("plain **bold** *it* `code` ~~gone~~\n- item\n1. one\n- [ ] task\n> quote\n# Head **strong**"));
        const auto at = [&](int s, int e = -1) { return m_editor->formatsAt(s, e < 0 ? s : e); };
        QCOMPARE(at(2), 0);
        QVERIFY(at(9) & MarkdownEditor::FmtBold);
        QVERIFY(!(at(9) & MarkdownEditor::FmtItalic));
        QVERIFY(at(17) & MarkdownEditor::FmtItalic);
        QVERIFY(at(22) & MarkdownEditor::FmtCode);
        QVERIFY(at(30) & MarkdownEditor::FmtStrike);
        // A selection of the bold word.
        QVERIFY(at(8, 12) & MarkdownEditor::FmtBold);
        const QString body = text();
        QVERIFY(at(body.indexOf(u"item")) & MarkdownEditor::FmtBullet);
        QVERIFY(at(body.indexOf(u"1. one") + 4) & MarkdownEditor::FmtNumbered);
        QVERIFY(at(body.indexOf(u"task")) & MarkdownEditor::FmtTask);
        QVERIFY(at(body.indexOf(u"quote")) & MarkdownEditor::FmtQuote);
        // A heading is not bold by itself; its strong word is.
        QVERIFY(!(at(body.indexOf(u"Head")) & MarkdownEditor::FmtBold));
        QVERIFY(at(body.indexOf(u"strong") + 2) & MarkdownEditor::FmtBold);
        QCOMPARE(at(-1), 0);
        QCOMPARE(at(9999), 0);
    }

    void formatsAtHeadingStrongOnlyInFormattedView()
    {
        open(QStringLiteral("# Head **strong** end"));
        const int word = text().indexOf(u"strong") + 2;
        QVERIFY(m_editor->formatsAt(word, word) & MarkdownEditor::FmtBold);
        QVERIFY(!(m_editor->formatsAt(2, 2) & MarkdownEditor::FmtBold));
        // The Syntax view draws the whole heading bold: not told apart.
        m_editor->setFormatted(false);
        m_editor->rehighlightNow();
        QVERIFY(!(m_editor->formatsAt(word, word) & MarkdownEditor::FmtBold));
        QVERIFY(!(m_editor->formatsAt(2, 2) & MarkdownEditor::FmtBold));
    }

    void formatsAtOnALongLine()
    {
        QString line;
        for (int i = 0; i < 5000; ++i) {
            line += QStringLiteral("a *b* ");
        }
        open(line);
        QVERIFY(m_editor->formatsAt(line.size() - 2, line.size() - 2) & MarkdownEditor::FmtItalic);
        QVERIFY(!(m_editor->formatsAt(0, 0) & MarkdownEditor::FmtItalic));
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

    void enterKeepsUnsequentialNumbers()
    {
        open(QStringLiteral("1. a\n1. b\n1. c"));
        place(4);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("1. a\n2. \n1. b\n1. c"));
    }

    void clearFormattingWholeLinesLeavesNextLine()
    {
        open(QStringLiteral("**a**\n# **b**\n**c**"));
        select(0, 6); // line one with its newline: ends at the start of line two
        m_editor->clearFormatting();
        QCOMPARE(text(), QStringLiteral("a\n# **b**\n**c**"));
    }

    void clearFormattingHeadingWithBold()
    {
        open(QStringLiteral("# **Title**"));
        select(4, 9); // "Title"
        m_editor->clearFormatting();
        QCOMPARE(text(), QStringLiteral("Title"));
    }

    void clearFormattingAcrossMergedSpans()
    {
        open(QStringLiteral("[a](u)**b** c"));
        select(1, 2); // "a"
        m_editor->clearFormatting();
        QCOMPARE(text(), QStringLiteral("a**b** c"));
        open(QStringLiteral("[a](u)**b** c"));
        select(1, 9); // "a" through "b"
        m_editor->clearFormatting();
        QCOMPARE(text(), QStringLiteral("ab c"));
    }

    void clearFormattingWithoutSelectionTakesTheLine()
    {
        open(QStringLiteral("x\n## **a** `b`\ny"));
        place(6);
        m_editor->clearFormatting();
        QCOMPARE(text(), QStringLiteral("x\na b\ny"));
    }

    void insertLinkOnAngleLinkTwice()
    {
        open(QStringLiteral("[t](<a b>) z"));
        place(2);
        m_editor->insertLink(QString(), QStringLiteral("c d"));
        QCOMPARE(text(), QStringLiteral("[t](<c d>) z"));
        place(2);
        m_editor->insertLink(QString(), QStringLiteral("e f"));
        QCOMPARE(text(), QStringLiteral("[t](<e f>) z"));
        place(2);
        m_editor->insertLink(QString(), QStringLiteral("plain"));
        QCOMPARE(text(), QStringLiteral("[t](plain) z"));
        QCOMPARE(m_editor->linkAt(2), QStringLiteral("plain"));
    }

    void insertLinkOnAutolink()
    {
        open(QStringLiteral("<https://x.org>"));
        place(3);
        QCOMPARE(m_editor->linkAt(3), QStringLiteral("https://x.org"));
        m_editor->insertLink(QString(), QStringLiteral("https://y.org"));
        QCOMPARE(text(), QStringLiteral("<https://y.org>"));
    }

    void insertLinkKeepsAutolinkALink()
    {
        open(QStringLiteral("<https://x.org>"));
        place(3);
        m_editor->insertLink(QString(), QStringLiteral("example.com"));
        QCOMPARE(text(), QStringLiteral("[example.com](example.com)"));
        open(QStringLiteral("<https://x.org>"));
        place(3);
        m_editor->insertLink(QString(), QStringLiteral("mailto:a@b.c"));
        QCOMPARE(text(), QStringLiteral("<mailto:a@b.c>"));
        QCOMPARE(m_edit->property("cursorPosition").toInt(), 14);
    }

    void spellWordsSkipCodeLikeTokens()
    {
        auto texts = [](const QList<SpellChecker::Word> &words) {
            QStringList out;
            for (const auto &w : words) {
                out.append(w.text);
            }
            return out;
        };
        const QString line = QStringLiteral("Don't stop: NASA x2 foo_bar iPhone https://x.org/abc a@b.org www.foo.com it’s a");
        QCOMPARE(texts(SpellChecker::words(line)), (QStringList{QStringLiteral("Don't"), QStringLiteral("stop"), QStringLiteral("it's")}));
        // A hidden marker inside a word doesn't split it; a Break ends it.
        const QString md = QStringLiteral("**bo**ld `code`");
        QList<SpellChecker::CharClass> classes(md.size(), SpellChecker::Normal);
        for (int i : {0, 1, 4, 5}) {
            classes[i] = SpellChecker::Skip;
        }
        for (int i = 9; i < md.size(); ++i) {
            classes[i] = SpellChecker::Break;
        }
        const auto words = SpellChecker::words(md, classes);
        QCOMPARE(texts(words), QStringList{QStringLiteral("bold")});
        QCOMPARE(words.first().start, 2);
        QCOMPARE(words.first().end, 8);
        // An emoji is not a letter: it ends the word and isn't one.
        QCOMPARE(texts(SpellChecker::words(QStringLiteral("hello\U0001F600world \U0001F600\U0001F600"))),
                 (QStringList{QStringLiteral("hello"), QStringLiteral("world")}));
    }

    void spellCheckSkipsOtherScripts()
    {
        SpellChecker::setLanguage(QStringLiteral("en_US"));
        if (SpellChecker::language().isEmpty()) {
            QSKIP("no en_US dictionary");
        }
        SpellChecker spell;
        QVERIFY(spell.misspelled(QStringLiteral("привет мир καλημέρα 你好世界")).isEmpty());
        QCOMPARE(spell.misspelled(QStringLiteral("привет wrold")).size(), 1);
    }

    void spellCheckUnderlinesProseOnly()
    {
        SpellChecker::setLanguage(QStringLiteral("en_US"));
        if (SpellChecker::language().isEmpty()) {
            QSKIP("no en_US dictionary");
        }
        SpellChecker spell;
        open(QStringLiteral("Helo **wrold** `helo` [helo](https://helo.org) fine"));
        spell.setTextEdit(m_edit.get());
        spell.setActive(true);
        m_editor->setSpellChecker(&spell);
        QCoreApplication::processEvents();
        QCOMPARE(underlined(), (QList<QPair<int, int>>{{0, 4}, {7, 12}}));
        // The context menu's word, without its markers, and a fix.
        const QVariantMap at = spell.wordAt(9);
        QCOMPARE(at.value(QStringLiteral("word")).toString(), QStringLiteral("wrold"));
        QVERIFY(at.value(QStringLiteral("suggestions")).toStringList().contains(QStringLiteral("world")));
        spell.replace(at.value(QStringLiteral("start")).toInt(), at.value(QStringLiteral("end")).toInt(), at.value(QStringLiteral("word")).toString(), QStringLiteral("world"));
        QCOMPARE(text(), QStringLiteral("Helo **world** `helo` [helo](https://helo.org) fine"));
        QCoreApplication::processEvents();
        QCOMPARE(underlined(), (QList<QPair<int, int>>{{0, 4}}));
        spell.ignore(QStringLiteral("Helo"));
        QCoreApplication::processEvents();
        QVERIFY(underlined().isEmpty());
        // Off: no underlines, no word.
        open(QStringLiteral("Thsi"));
        m_editor->setSpellChecker(&spell);
        spell.setTextEdit(m_edit.get());
        QCoreApplication::processEvents();
        QCOMPARE(underlined().size(), 1);
        spell.setActive(false);
        QCoreApplication::processEvents();
        QVERIFY(underlined().isEmpty());
        QVERIFY(spell.wordAt(1).isEmpty());
        m_editor->setSpellChecker(nullptr);
    }

    void spellCheckPlainText()
    {
        SpellChecker::setLanguage(QStringLiteral("en_US"));
        if (SpellChecker::language().isEmpty()) {
            QSKIP("no en_US dictionary");
        }
        open(QStringLiteral("plain **wrold** text"));
        m_editor->setTextEdit(nullptr); // no Markdown: the checker's own highlighter
        SpellChecker spell;
        spell.setTextEdit(m_edit.get());
        spell.setPlainText(true);
        spell.setActive(true);
        QCoreApplication::processEvents();
        QCOMPARE(underlined(), (QList<QPair<int, int>>{{8, 13}}));
        // Markdown takes over the document: its highlighter underlines.
        spell.setPlainText(false);
        m_editor->setSpellChecker(&spell);
        m_editor->setTextEdit(m_edit.get());
        QCoreApplication::processEvents();
        QCOMPARE(underlined(), (QList<QPair<int, int>>{{8, 13}}));
        m_editor->setSpellChecker(nullptr);
    }

    // Past 64K characters a style change highlights in slices from the event
    // loop; the result is what highlighting at once gives, fences and all.
    void bigDocumentHighlightsInSlices()
    {
        open(bigMarkdown());
        const auto atOnce = formats();
        m_editor->setFormatted(false);
        QCoreApplication::sendPostedEvents();
        m_editor->rehighlightNow(); // every line plain, before going back
        QVERIFY(formats() != atOnce);
        m_editor->setFormatted(true);
        QCoreApplication::sendPostedEvents(); // the queued style: the first slice
        QVERIFY(formats() != atOnce); // not all of it in one go
        QTRY_VERIFY_WITH_TIMEOUT(formats() == atOnce, 10000);
    }

    // Made on a document with text, the highlighter still highlights an edit
    // as it happens (QSyntaxHighlighter skips edits while its own queued
    // rehighlight is due; the constructor runs that one at once).
    void editRightAfterAttachIsHighlighted()
    {
        open(QStringLiteral("plain\nmore"));
        auto *doc = qobject_cast<QQuickTextDocument *>(m_edit->property("textDocument").value<QObject *>())->textDocument();
        QVERIFY(doc->begin().layout()->formats().isEmpty());
        QTextCursor(doc).insertText(QStringLiteral("# "));
        QVERIFY(!doc->begin().layout()->formats().isEmpty());
    }

    // Attached to a big document, an edit ahead of the first pass is one
    // line's work: lines the slices haven't reached keep their state, so Qt
    // doesn't read on through the rest of the document in that keystroke
    // (300 ms for 1 MB).
    void editAheadOfSlicesReadsOneLine()
    {
        m_editor.reset();
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }", QUrl());
        m_edit.reset(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY(m_edit);
        m_edit->setProperty("text", bigMarkdown());
        m_editor = std::make_unique<MarkdownEditor>();
        m_editor->setTextEdit(m_edit.get()); // the first slice
        auto *doc = qobject_cast<QQuickTextDocument *>(m_edit->property("textDocument").value<QObject *>())->textDocument();
        QTextCursor middle(doc->findBlockByNumber(doc->blockCount() / 2));
        QCOMPARE(doc->lastBlock().userState(), -1);
        middle.insertText(QStringLiteral("x"));
        QCOMPARE(doc->lastBlock().userState(), -1); // not read yet
        QTest::qWait(3000);
        const auto sliced = formats();
        m_editor->rehighlightNow();
        QCOMPARE(formats(), sliced);
    }

    // Edits while the slices run, before and after where they are: each line
    // still ends up as highlighting at once would have it.
    void editsDuringSlicesEndRight()
    {
        open(bigMarkdown());
        m_editor->setFormatted(false);
        QCoreApplication::sendPostedEvents();
        auto *doc = qobject_cast<QQuickTextDocument *>(m_edit->property("textDocument").value<QObject *>())->textDocument();
        QTextCursor top(doc);
        top.insertText(QStringLiteral("```\nnow a fence\n```\n"));
        QTextCursor end(doc);
        end.movePosition(QTextCursor::End);
        end.insertText(QStringLiteral("\n# A heading at the end\n"));
        QTest::qWait(2000);
        const auto sliced = formats();
        m_editor->rehighlightNow();
        QCOMPARE(formats(), sliced);
    }

private:
    // About 400 KB: headings, emphasis, and fences that slices cut through.
    static QString bigMarkdown()
    {
        QString t;
        for (int i = 0; t.size() < 400 * 1024; ++i) {
            t += QStringLiteral("# Part %1\n\nSome **bold** and _italic_ text, `code` and a [link](https://example.org).\n\n").arg(i);
            if (i % 7 == 0) {
                t += QStringLiteral("```\nfenced line\n# not a heading\n```\n\n");
            }
        }
        return t;
    }
    // Each line's state and formats.
    QList<QPair<int, QList<QTextLayout::FormatRange>>> formats() const
    {
        auto *doc = qobject_cast<QQuickTextDocument *>(m_edit->property("textDocument").value<QObject *>())->textDocument();
        QList<QPair<int, QList<QTextLayout::FormatRange>>> all;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            all.append({b.userState(), b.layout()->formats()});
        }
        return all;
    }
    // The misspelled words of the first line, as the underlines get them.
    QList<QPair<int, int>> underlined() const
    {
        auto *doc = qobject_cast<QQuickTextDocument *>(m_edit->property("textDocument").value<QObject *>())->textDocument();
        const BlockInfo *info = BlockInfo::of(doc->begin());
        return info ? info->misspelled : QList<QPair<int, int>>{};
    }
};

QTEST_MAIN(EditorTest)
#include "editor_test.moc"
