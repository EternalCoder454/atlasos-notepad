// MarkdownEditor's edits on a real TextEdit: keys go through its event filter
// and toolbar calls through its invokables, and the test reads back the text.
#include "codeeditor.h"
#include "codehighlighter.h"
#include "linetools.h"
#include "markdown.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
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

#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <algorithm>
#include <functional>
#include <memory>

namespace
{
// A document's formats, block by block (start, length, look of each range).
bool sameFormats(QTextDocument &a, QTextDocument &b, QString *why = nullptr)
{
    if (a.blockCount() != b.blockCount()) {
        return false;
    }
    QTextBlock x = a.begin(), y = b.begin();
    for (int n = 0; x.isValid() && y.isValid(); x = x.next(), y = y.next(), ++n) {
        const auto fa = x.layout()->formats();
        const auto fb = y.layout()->formats();
        bool same = fa.size() == fb.size();
        for (qsizetype i = 0; same && i < fa.size(); ++i) {
            same = fa[i].start == fb[i].start && fa[i].length == fb[i].length && fa[i].format == fb[i].format;
        }
        if (!same) {
            if (why) {
                auto dump = [](const QList<QTextLayout::FormatRange> &f) {
                    QString d;
                    for (const auto &r : f) {
                        d += QStringLiteral(" [%1+%2").arg(r.start).arg(r.length);
                        const auto props = r.format.properties();
                        for (auto it = props.cbegin(); it != props.cend(); ++it) {
                            d += QStringLiteral(" %1=%2").arg(it.key()).arg(it.value().toString());
                        }
                        d += u']';
                    }
                    return d;
                };
                *why = QStringLiteral("line %1 (%2 and %3 ranges): %4 | mine%5 | stock%6").arg(n).arg(fa.size()).arg(fb.size()).arg(x.text(), dump(fa), dump(fb));
            }
            return false;
        }
    }
    return true;
}

KSyntaxHighlighting::Theme themeFor(bool dark)
{
    return codeRepository().defaultTheme(dark ? KSyntaxHighlighting::Repository::DarkTheme : KSyntaxHighlighting::Repository::LightTheme);
}

// What KSyntaxHighlighting's own highlighter makes of `text`.
struct Stock {
    QTextDocument doc;
    std::unique_ptr<KSyntaxHighlighting::SyntaxHighlighter> highlighter;
    Stock(const QString &text, const QString &language, bool dark)
    {
        (void)doc.documentLayout();
        doc.setPlainText(text);
        highlighter = std::make_unique<KSyntaxHighlighting::SyntaxHighlighter>(&doc);
        highlighter->setTheme(themeFor(dark));
        highlighter->setDefinition(codeRepository().definitionForName(language));
        highlighter->rehighlight();
    }
};

QString cppSample()
{
    return QStringLiteral(
        "#include <vector>\n#define MAX(a, b) ((a) > (b) ? (a) : (b))\n\n"
        "/* a block\n   comment */\nnamespace n {\n"
        "template <class T> struct Box { T v = T(); };\n"
        "int main(int argc, char **argv) // entry\n{\n"
        "    const char *s = \"text \\\" more\"; char c = 'x';\n"
        "    for (int i = 0; i < 10; ++i) { if (i % 2) continue; }\n"
        "    auto r = R\"(raw\nstring)\"; return 0x1F + 3.5e2;\n}\n}\n");
}

QString pythonSample()
{
    return QStringLiteral(
        "import os\n\n@decorator\ndef f(a, b=2, *args):\n"
        "    \"\"\"doc\n    string\"\"\"\n    x = [i for i in range(10) if i % 2]  # c\n"
        "    return f'{a} {b}' + \"s\"\n\nclass A(B):\n    pass\n");
}

QString jsonSample()
{
    return QStringLiteral("{\n  \"name\": \"atlas\",\n  \"n\": [1, 2.5, -3e4, true, null],\n  \"o\": {\"k\": \"v\\n\"}\n}\n");
}

QString shellSample()
{
    return QStringLiteral(
        "#!/bin/bash\nset -euo pipefail\nfor f in *.txt; do\n  echo \"$f: ${#f}\" | grep -v '^#' > out.$$\ndone\n"
        "cat <<EOF\nhere $HOME\nEOF\nif [ -f x ]; then echo $(date); fi\n");
}

// Many lines of C++ with comments, up to about `chars` characters.
QString generatedCpp(int chars, bool blockComments = true)
{
    QString out;
    for (int i = 0; out.size() < chars; ++i) {
        out += QStringLiteral("int f%1(int a) { return a * %1; } // line %1 \"s\"\n").arg(i);
        if (blockComments && i % 50 == 7) {
            out += QStringLiteral("/* a comment\n   over two lines */ static const char *k%1 = \"x\";\n").arg(i);
        }
    }
    return out;
}
} // namespace

class EditorTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    std::unique_ptr<QQuickItem> m_edit;
    std::unique_ptr<MarkdownEditor> m_editor;
    std::unique_ptr<CodeEditor> m_code;
    std::unique_ptr<LineTools> m_lines;

    // A fresh TextEdit holding `text` with a CodeEditor on it.
    void openCode(const QString &text, const QString &language = QStringLiteral("C++"), bool spaces = true, int width = 4)
    {
        m_code.reset();
        m_lines.reset();
        m_editor.reset();
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }", QUrl());
        m_edit.reset(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(m_edit, qPrintable(component.errorString()));
        m_edit->setProperty("text", text);
        m_code = std::make_unique<CodeEditor>();
        m_code->setLanguage(language);
        m_code->setInsertSpaces(spaces);
        m_code->setIndentWidth(width);
        m_code->setTextEdit(m_edit.get());
        QCoreApplication::processEvents();
        attachLines(width);
    }
    // The line tools on the current TextEdit.
    void attachLines(int width = 4)
    {
        m_lines = std::make_unique<LineTools>();
        m_lines->setIndentWidth(width);
        m_lines->setTextEdit(m_edit.get());
    }
    // A plain tab: a TextEdit with the line tools only, no CodeEditor.
    void openPlain(const QString &text)
    {
        m_code.reset();
        m_lines.reset();
        m_editor.reset();
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }", QUrl());
        m_edit.reset(qobject_cast<QQuickItem *>(component.create()));
        QVERIFY2(m_edit, qPrintable(component.errorString()));
        m_edit->setProperty("text", text);
        QCoreApplication::processEvents();
        attachLines();
    }
    int selStart() const { return m_edit->property("selectionStart").toInt(); }
    int selEnd() const { return m_edit->property("selectionEnd").toInt(); }
    // One undo must bring `before` back.
    void undoesTo(const QString &before)
    {
        QMetaObject::invokeMethod(m_edit.get(), "undo");
        QCOMPARE(text(), before);
    }

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

private Q_SLOTS:
    // ---- CodeEditor

    void codeLanguages()
    {
        const QStringList names = CodeEditor::languages();
        QVERIFY(names.contains(QStringLiteral("C++")));
        QVERIFY(names.contains(QStringLiteral("Python")));
        QCOMPARE(names.size(), QSet<QString>(names.begin(), names.end()).size());
    }
    void codeHighlightsAndDetaches()
    {
        openCode(QStringLiteral("int x = 1; // hi\n"));
        QVERIFY(!blockAt(0).layout()->formats().isEmpty());
        m_code->setLanguage(QString());
        QVERIFY(blockAt(0).layout()->formats().isEmpty());
        m_code->setLanguage(QStringLiteral("C++"));
        QVERIFY(!blockAt(0).layout()->formats().isEmpty());
        m_code->setDark(true);
        QVERIFY(!blockAt(0).layout()->formats().isEmpty());
        m_code->setTextEdit(nullptr);
        QVERIFY(blockAt(0).layout()->formats().isEmpty());
    }
    // ---- CodeHighlighter: the same look as KSyntaxHighlighting's, in linear time

    void codeHighlighterMatchesStock_data()
    {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("text");
        QTest::newRow("C++") << QStringLiteral("C++") << cppSample();
        QTest::newRow("Python") << QStringLiteral("Python") << pythonSample();
        QTest::newRow("JSON") << QStringLiteral("JSON") << jsonSample();
        QTest::newRow("Bash") << QStringLiteral("Bash") << shellSample();
    }
    void codeHighlighterMatchesStock()
    {
        QFETCH(QString, language);
        QFETCH(QString, text);
        for (const bool dark : {false, true}) {
            Stock stock(text, language, dark);
            QTextDocument doc;
            (void)doc.documentLayout(); // contentsChange is only sent with a layout
            doc.setPlainText(text);
            CodeHighlighter highlighter(&doc);
            highlighter.setTheme(themeFor(dark));
            highlighter.setDefinition(codeRepository().definitionForName(language));
            QVERIFY(!doc.begin().layout()->formats().isEmpty());
            QString why;
            QVERIFY2(sameFormats(doc, stock.doc, &why), qPrintable(why));
            // Again, now.
            highlighter.rehighlightAll(true);
            QVERIFY2(sameFormats(doc, stock.doc, &why), qPrintable(why));
        }
    }

    // Typing `/*` at the top: the lines after it become comment (the lines
    // by the edit at once, the rest from the event loop), and deleting it
    // gives them back. Past 64K characters the first pass is in slices too.
    void codeHighlighterCarriesStateChanges()
    {
        // No `*/` in it: the comment runs to the end.
        const QString text = generatedCpp(100 * 1024, false);
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        doc.setPlainText(text);
        CodeHighlighter highlighter(&doc);
        highlighter.setTheme(themeFor(false));
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        QString why;
        {
            Stock stock(text, QStringLiteral("C++"), false);
            QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
        }
        QTextCursor cursor(&doc);
        cursor.insertText(QStringLiteral("/*"));
        {
            Stock stock(doc.toPlainText(), QStringLiteral("C++"), false);
            // A screenful after the edit is done before the event loop runs.
            QTextBlock a = doc.findBlockByNumber(3), b = stock.doc.findBlockByNumber(3);
            QCOMPARE(a.layout()->formats().size(), b.layout()->formats().size());
            QVERIFY(!sameFormats(doc, stock.doc)); // the end isn't there yet
            QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
        }
        cursor.setPosition(0);
        cursor.setPosition(2, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        QCOMPARE(doc.toPlainText(), text);
        Stock stock(text, QStringLiteral("C++"), false);
        QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
    }

    // Text set on a big document (what a file filled in pieces does) is
    // highlighted as the stock one would, without a pass over it per piece.
    void codeHighlighterFollowsAppends()
    {
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        CodeHighlighter highlighter(&doc);
        highlighter.setTheme(themeFor(false));
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        const QString text = generatedCpp(150 * 1024);
        QTextCursor cursor(&doc);
        for (qsizetype pos = 0; pos < text.size();) {
            const qsizetype end = text.indexOf(u'\n', qMin<qsizetype>(pos + 20000, text.size() - 1)) + 1;
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(text.mid(pos, end - pos));
            pos = end;
        }
        QCOMPARE(doc.toPlainText(), text);
        Stock stock(text, QStringLiteral("C++"), false);
        QString why;
        QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
    }

    // A theme or language switch restyles every line, a big document too.
    void codeHighlighterSwitchesThemeAndLanguage()
    {
        const QString text = generatedCpp(100 * 1024);
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        doc.setPlainText(text);
        CodeHighlighter highlighter(&doc);
        highlighter.setTheme(themeFor(false));
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        QString why;
        highlighter.setTheme(themeFor(true));
        {
            Stock stock(text, QStringLiteral("C++"), true);
            QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
        }
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("Python")));
        Stock stock(text, QStringLiteral("Python"), true);
        QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
    }

    // The all-at-once pass is linear: 1 MB took 90 s through the stock one.
    void codeHighlighterIsLinear()
    {
        const QString text = generatedCpp(1'000'000);
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        doc.setPlainText(text);
        CodeHighlighter highlighter(&doc);
        highlighter.setTheme(themeFor(false));
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        QElapsedTimer clock;
        clock.start();
        highlighter.rehighlightAll(true);
        const qint64 ms = clock.elapsed();
        qInfo() << "1 MB rehighlightAll:" << ms << "ms";
        QVERIFY2(ms < 5000, qPrintable(QStringLiteral("took %1 ms").arg(ms)));
        QVERIFY(!doc.lastBlock().previous().layout()->formats().isEmpty());
    }

    // A line over Limits::lineLength is left plain (and the lines after it
    // still highlighted).
    void codeHighlighterSkipsHugeLines()
    {
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        doc.setPlainText(QStringLiteral("int a; // x\n") + QString(150'000, u'a') + QStringLiteral("\nint b; // y\n"));
        CodeHighlighter highlighter(&doc);
        highlighter.setTheme(themeFor(false));
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        QVERIFY(!doc.findBlockByNumber(0).layout()->formats().isEmpty());
        QVERIFY(doc.findBlockByNumber(1).layout()->formats().isEmpty());
        QVERIFY(!doc.findBlockByNumber(2).layout()->formats().isEmpty());
    }

    // A big paste after a pass that stops where the state matches again (the
    // `*/`) is still highlighted: the pass goes to the end for it.
    void codeHighlighterBigInsertDuringPass()
    {
        const QString head = generatedCpp(200 * 1024, false) + QStringLiteral("*/\n");
        const QString text = head + generatedCpp(100 * 1024, false);
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        doc.setPlainText(text);
        CodeHighlighter highlighter(&doc);
        highlighter.setTheme(themeFor(false));
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        QString why;
        {
            Stock stock(text, QStringLiteral("C++"), false);
            QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
        }
        QTextCursor cursor(&doc);
        cursor.insertText(QStringLiteral("/*"));
        // Before the pass gets to the `*/`: a paste well after it.
        cursor.setPosition(doc.findBlockByNumber(doc.blockCount() - 100).position());
        cursor.insertText(generatedCpp(8000, false));
        Stock stock(doc.toPlainText(), QStringLiteral("C++"), false);
        QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
    }

    // Typing in a big paste the pass hasn't done yet doesn't put the pass off:
    // it goes on while the typing does, not only once it stops.
    void codeHighlighterTypingDoesNotHoldBackPass()
    {
        const QString text = generatedCpp(100 * 1024, false);
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        doc.setPlainText(text);
        CodeHighlighter highlighter(&doc);
        highlighter.setTheme(themeFor(false));
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        QString why;
        {
            Stock stock(text, QStringLiteral("C++"), false);
            QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 20000);
        }
        // A comment opened at the top leaves a pass due for the rest (the
        // edit's own 2 ms reach a few dozen lines), then a big paste at the
        // end is left to that pass, its lines not done.
        QTextCursor cursor(&doc);
        cursor.insertText(QStringLiteral("/*"));
        const int paste = doc.blockCount() - 1;
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(generatedCpp(8000, false));
        // A key every 10 ms in the paste, for 300 ms.
        QTextCursor typing(doc.findBlockByNumber(paste + 3));
        for (int i = 0; i < 30; ++i) {
            typing.insertText(QStringLiteral("x"));
            QTest::qWait(10);
        }
        // Halfway down, a line is a comment by now: the pass ran meanwhile.
        const QList<QTextLayout::FormatRange> comment = doc.findBlockByNumber(1).layout()->formats();
        QCOMPARE(comment.size(), 1);
        const QTextBlock middle = doc.findBlockByNumber(paste / 2);
        const QList<QTextLayout::FormatRange> got = middle.layout()->formats();
        QVERIFY2(got.size() == 1 && got.first().start == 0 && got.first().length == middle.length() - 1
                     && got.first().format == comment.first().format,
                 "the pass made no progress while typing went on");
    }

    // The input method's text being composed keeps its underline through a
    // highlight, and the line's colours move past it, as the stock one has it.
    void codeHighlighterKeepsPreedit()
    {
        const QString text = QStringLiteral("int a = 1; // x \"s\"\n");
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        doc.setPlainText(text);
        CodeHighlighter highlighter(&doc);
        highlighter.setTheme(themeFor(false));
        highlighter.setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        Stock stock(text, QStringLiteral("C++"), false);
        QTextLayout::FormatRange preedit;
        preedit.start = 6;
        preedit.length = 2;
        preedit.format.setFontUnderline(true);
        preedit.format.setUnderlineStyle(QTextCharFormat::DashUnderline);
        for (QTextDocument *d : {&doc, &stock.doc}) {
            QTextLayout *layout = d->begin().layout();
            layout->setPreeditArea(6, QStringLiteral("xy"));
            layout->setFormats(layout->formats() << preedit);
        }
        highlighter.rehighlightAll(true);
        stock.highlighter->rehighlight();
        const auto formats = doc.begin().layout()->formats();
        const bool kept = std::any_of(formats.cbegin(), formats.cend(), [](const QTextLayout::FormatRange &r) {
            return r.start == 6 && r.length == 2 && r.format.underlineStyle() == QTextCharFormat::DashUnderline;
        });
        QVERIFY(kept);
        QString why;
        QVERIFY2(sameFormats(doc, stock.doc, &why), qPrintable(why));
    }

    // Blocks may carry another highlighter's data (the Markdown one, before a
    // language switch): it is replaced, not read as ours, and what isn't ours
    // is left when we go.
    void codeHighlighterForeignUserData()
    {
        struct Foreign : QTextBlockUserData {
            int marker = 7;
        };
        const QString text = cppSample();
        QTextDocument doc;
        (void)doc.documentLayout(); // contentsChange is only sent with a layout
        doc.setPlainText(text);
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            b.setUserData(new Foreign);
        }
        auto highlighter = std::make_unique<CodeHighlighter>(&doc);
        highlighter->setTheme(themeFor(false));
        highlighter->setDefinition(codeRepository().definitionForName(QStringLiteral("C++")));
        Stock stock(text, QStringLiteral("C++"), false);
        QString why;
        QVERIFY2(sameFormats(doc, stock.doc, &why), qPrintable(why));
        // An edit in a comment, after its first line became someone else's
        // again: its state isn't known from there.
        QCOMPARE(doc.findBlockByNumber(3).text(), QStringLiteral("/* a block"));
        doc.findBlockByNumber(3).setUserData(new Foreign);
        QTextCursor cursor(doc.findBlockByNumber(4));
        cursor.insertText(QStringLiteral(" "));
        cursor.deletePreviousChar();
        QTRY_VERIFY2_WITH_TIMEOUT(sameFormats(doc, stock.doc, &why), qPrintable(why), 5000);
        doc.findBlockByNumber(0).setUserData(new Foreign);
        highlighter.reset();
        QVERIFY(dynamic_cast<Foreign *>(doc.findBlockByNumber(0).userData()));
        QVERIFY(!doc.findBlockByNumber(2).userData());
    }

    void codeToggleComment()
    {
        openCode(QStringLiteral("  a\n    b\n\n  c"));
        QCOMPARE(m_code->commentMarker(), QStringLiteral("//"));
        m_code->toggleComment(0, text().size());
        QCOMPARE(text(), QStringLiteral("  // a\n  //   b\n\n  // c"));
        m_code->toggleComment(0, text().size());
        QCOMPARE(text(), QStringLiteral("  a\n    b\n\n  c"));
        // One uncommented line makes the toggle comment all of them.
        openCode(QStringLiteral("// a\nb"));
        m_code->toggleComment(0, 6);
        QCOMPARE(text(), QStringLiteral("// // a\n// b"));
        // One undo step.
        QMetaObject::invokeMethod(m_edit.get(), "undo");
        QCOMPARE(text(), QStringLiteral("// a\nb"));
        // [from, to) stops before a line the range ends at the start of.
        openCode(QStringLiteral("a\nb\nc"));
        m_code->toggleComment(0, 2);
        QCOMPARE(text(), QStringLiteral("// a\nb\nc"));
        // No single-line marker: the multi-line pair wraps each line.
        openCode(QStringLiteral("<a>\n<b>"), QStringLiteral("HTML"));
        m_code->toggleComment(0, 7);
        QCOMPARE(text(), QStringLiteral("<!-- <a> -->\n<!-- <b> -->"));
        m_code->toggleComment(0, text().size());
        QCOMPARE(text(), QStringLiteral("<a>\n<b>"));
        // No language: nothing happens.
        openCode(QStringLiteral("a"), QString());
        m_code->toggleComment(0, 1);
        QCOMPARE(text(), QStringLiteral("a"));
    }
    void codeIndentOutdent()
    {
        openCode(QStringLiteral("a\n\n  b\nc"));
        m_code->indentLines(0, text().size());
        QCOMPARE(text(), QStringLiteral("    a\n\n      b\n    c"));
        m_code->outdentLines(0, text().size());
        QCOMPARE(text(), QStringLiteral("a\n\n  b\nc"));
        openCode(QStringLiteral("a\n\tb"), QStringLiteral("C++"), false);
        m_code->indentLines(0, 5);
        QCOMPARE(text(), QStringLiteral("\ta\n\t\tb"));
        m_code->outdentLines(3, 4);
        QCOMPARE(text(), QStringLiteral("\ta\n\tb"));
        m_code->outdentLines(0, 0);
        QCOMPARE(text(), QStringLiteral("a\n\tb"));
    }
    void codeBracketPair()
    {
        openCode(QStringLiteral("f(a[1], {x})"));
        QCOMPARE(m_code->bracketPair(2), QPoint(1, 11)); // just after "("
        QCOMPARE(m_code->bracketPair(1), QPoint(1, 11)); // at "("
        QCOMPARE(m_code->bracketPair(12), QPoint(11, 1)); // after ")"
        QCOMPARE(m_code->bracketPair(4), QPoint(3, 5)); // [ ]
        QCOMPARE(m_code->bracketPair(0), QPoint(-1, -1)); // next to "f" only
        QCOMPARE(m_code->bracketPair(7), QPoint(-1, -1)); // between ", "
        openCode(QStringLiteral("a ( b"));
        QCOMPARE(m_code->bracketPair(3), QPoint(-1, -1)); // unbalanced
        openCode(QStringLiteral("a ) b"));
        QCOMPARE(m_code->bracketPair(3), QPoint(-1, -1));
        openCode(QStringLiteral("(()"));
        QCOMPARE(m_code->bracketPair(1), QPoint(1, 2)); // "(" at 0 has no match, the next does
        QCOMPARE(m_code->bracketPair(2), QPoint(1, 2));
        openCode(QString());
        QCOMPARE(m_code->bracketPair(0), QPoint(-1, -1));
        QCOMPARE(m_code->bracketPair(-5), QPoint(-1, -1));
        // The scan gives up after 20k characters.
        openCode(u'(' + QString(30'000, u'x') + u')');
        QCOMPARE(m_code->bracketPair(1), QPoint(-1, -1));
        openCode(u'(' + QString(1000, u'x') + u')');
        QCOMPARE(m_code->bracketPair(1), QPoint(0, 1001));
    }
    void codeEditorSurvivesTextEditDying()
    {
        openCode(QStringLiteral("int x;\n"));
        QVERIFY(m_code->textEdit());
        m_edit.reset(); // the TextEdit and its document go first
        m_code.reset(); // must not double-free the highlighter
        openCode(QStringLiteral("int x;\n"));
        m_edit.reset();
        m_code->setTextEdit(nullptr);
        m_code->setLanguage(QStringLiteral("Python"));
        m_code.reset();
    }
    // --- Line tools (LineTools): plain, code and Markdown source tabs.
    void lineSort()
    {
        openPlain(QStringLiteral("pear\nApple\napple\nbanana"));
        m_lines->sortLines(0, 0, 0);
        QCOMPARE(text(), QStringLiteral("Apple\napple\nbanana\npear")); // ordinal: capitals first
        undoesTo(QStringLiteral("pear\nApple\napple\nbanana"));
        m_lines->sortLines(0, 0, LineTools::CaseInsensitive);
        QCOMPARE(text(), QStringLiteral("Apple\napple\nbanana\npear")); // stable
        m_lines->sortLines(0, 0, LineTools::CaseInsensitive | LineTools::Descending);
        QCOMPARE(text(), QStringLiteral("pear\nbanana\nApple\napple")); // ties keep their order
        openPlain(QStringLiteral("z\nc\nb\na"));
        m_lines->sortLines(2, 5, 0); // lines 2..4 only
        QCOMPARE(text(), QStringLiteral("z\nb\nc\na"));
        openPlain(QStringLiteral("z\nc\nb\na"));
        m_lines->sortLines(2, 7, 0);
        QCOMPARE(text(), QStringLiteral("z\na\nb\nc"));
        QCOMPARE(selStart(), 2);
        QCOMPARE(selEnd(), 7);
        openPlain(QStringLiteral("b\na")); // no newline at the end
        m_lines->sortLines(0, 0, LineTools::Descending);
        QCOMPARE(text(), QStringLiteral("b\na"));
        m_lines->sortLines(0, 0, 0);
        QCOMPARE(text(), QStringLiteral("a\nb"));
        openPlain(QString());
        m_lines->sortLines(0, 0, 0);
        QCOMPARE(text(), QString());
    }
    void lineSortNumeric()
    {
        openPlain(QStringLiteral("10 c\n9 b\nnote\n-3\n 2.5\n9 a\nabc"));
        m_lines->sortLines(0, 0, LineTools::Numeric);
        QCOMPARE(text(), QStringLiteral("abc\nnote\n-3\n 2.5\n9 a\n9 b\n10 c"));
        m_lines->sortLines(0, 0, LineTools::Numeric | LineTools::Descending);
        QCOMPARE(text(), QStringLiteral("10 c\n9 b\n9 a\n 2.5\n-3\nnote\nabc"));
    }
    void lineReverseDuplicatesEmpty()
    {
        openPlain(QStringLiteral("a\nb\nc"));
        m_lines->reverseLines(0, 0);
        QCOMPARE(text(), QStringLiteral("c\nb\na"));
        undoesTo(QStringLiteral("a\nb\nc"));
        openPlain(QStringLiteral("b\na\nb\n\na\n\nc\nb"));
        m_lines->removeDuplicateLines(0, 0);
        QCOMPARE(text(), QStringLiteral("b\na\n\nc"));
        undoesTo(QStringLiteral("b\na\nb\n\na\n\nc\nb"));
        m_lines->removeEmptyLines(0, 0);
        QCOMPARE(text(), QStringLiteral("b\na\nb\na\nc\nb"));
        openPlain(QStringLiteral("a\n  \n\t\nb\n"));
        m_lines->removeEmptyLines(0, 0);
        QCOMPARE(text(), QStringLiteral("a\nb"));
        openPlain(QStringLiteral("\n\n"));
        m_lines->removeEmptyLines(0, 0); // every line goes
        QCOMPARE(text(), QString());
        undoesTo(QStringLiteral("\n\n"));
        openPlain(QStringLiteral("x\nx\nx")); // a selection limits it
        m_lines->removeDuplicateLines(0, 5);
        QCOMPARE(text(), QStringLiteral("x"));
        openPlain(QStringLiteral("a\na\nb\nb"));
        m_lines->removeDuplicateLines(4, 7);
        QCOMPARE(text(), QStringLiteral("a\na\nb"));
    }
    void lineDuplicate()
    {
        openPlain(QStringLiteral("a\nbb\nc"));
        place(4); // caret in "bb"
        m_lines->duplicateLines(4, 4);
        QCOMPARE(text(), QStringLiteral("a\nbb\nbb\nc"));
        QCOMPARE(caret(), 7); // on the copy
        undoesTo(QStringLiteral("a\nbb\nc"));
        m_lines->duplicateLines(0, 4); // selection over "a\nb"
        QCOMPARE(text(), QStringLiteral("a\nbb\na\nbb\nc"));
        QCOMPARE(selStart(), 5);
        QCOMPARE(selEnd(), 9);
        openPlain(QStringLiteral("a\nb")); // the last line has no newline
        m_lines->duplicateLines(2, 2);
        QCOMPARE(text(), QStringLiteral("a\nb\nb"));
        QCOMPARE(caret(), 4);
        openPlain(QString());
        m_lines->duplicateLines(0, 0);
        QCOMPARE(text(), QStringLiteral("\n"));
        openPlain(QStringLiteral("a\n"));
        m_lines->duplicateLines(2, 2); // the empty last line
        QCOMPARE(text(), QStringLiteral("a\n\n"));
    }
    void lineMove()
    {
        openPlain(QStringLiteral("a\nb\nc\nd"));
        m_lines->moveLines(2, 2, false);
        QCOMPARE(text(), QStringLiteral("b\na\nc\nd"));
        QCOMPARE(caret(), 0);
        undoesTo(QStringLiteral("a\nb\nc\nd"));
        m_lines->moveLines(0, 0, false); // already first
        QCOMPARE(text(), QStringLiteral("a\nb\nc\nd"));
        m_lines->moveLines(2, 5, true); // "b\nc" down
        QCOMPARE(text(), QStringLiteral("a\nd\nb\nc"));
        QCOMPARE(selStart(), 4);
        QCOMPARE(selEnd(), 7);
        m_lines->moveLines(selStart(), selEnd(), true); // already last
        QCOMPARE(text(), QStringLiteral("a\nd\nb\nc"));
        m_lines->moveLines(selStart(), selEnd(), false);
        QCOMPARE(text(), QStringLiteral("a\nb\nc\nd"));
        QCOMPARE(selStart(), 2);
        QCOMPARE(selEnd(), 5);
        openPlain(QStringLiteral("a\nb")); // last line without newline moves up
        m_lines->moveLines(2, 2, false);
        QCOMPARE(text(), QStringLiteral("b\na"));
        m_lines->moveLines(0, 0, true);
        QCOMPARE(text(), QStringLiteral("a\nb"));
        openPlain(QString());
        m_lines->moveLines(0, 0, true);
        m_lines->moveLines(0, 0, false);
        QCOMPARE(text(), QString());
    }
    void lineDelete()
    {
        openPlain(QStringLiteral("a\nb\nc"));
        m_lines->deleteLines(2, 2);
        QCOMPARE(text(), QStringLiteral("a\nc"));
        QCOMPARE(caret(), 2);
        undoesTo(QStringLiteral("a\nb\nc"));
        m_lines->deleteLines(0, 3); // "a\nb"
        QCOMPARE(text(), QStringLiteral("c"));
        undoesTo(QStringLiteral("a\nb\nc"));
        m_lines->deleteLines(4, 4); // the last line, no newline
        QCOMPARE(text(), QStringLiteral("a\nb"));
        QCOMPARE(caret(), 2);
        m_lines->deleteLines(0, 3);
        QCOMPARE(text(), QString());
        m_lines->deleteLines(0, 0);
        QCOMPARE(text(), QString());
        openPlain(QStringLiteral("a\nb\n")); // the empty last line
        m_lines->deleteLines(4, 4);
        QCOMPARE(text(), QStringLiteral("a\nb"));
    }
    void lineJoin()
    {
        openPlain(QStringLiteral("a  \n   b\n\t\n c\nd"));
        m_lines->joinLines(0, 13);
        QCOMPARE(text(), QStringLiteral("a b c\nd"));
        undoesTo(QStringLiteral("a  \n   b\n\t\n c\nd"));
        m_lines->joinLines(0, 0); // the caret's line and the next
        QCOMPARE(text(), QStringLiteral("a b\n\t\n c\nd"));
        QCOMPARE(caret(), 1);
        openPlain(QStringLiteral("a\nb"));
        m_lines->joinLines(2, 2); // the last line: nothing to join
        QCOMPARE(text(), QStringLiteral("a\nb"));
        openPlain(QStringLiteral("a\nb\nc"));
        m_lines->joinLines(0, 5);
        QCOMPARE(text(), QStringLiteral("a b c"));
        QCOMPARE(selEnd(), 5);
        openPlain(QStringLiteral("\n x"));
        m_lines->joinLines(0, 0);
        QCOMPARE(text(), QStringLiteral("x"));
        openPlain(QString());
        m_lines->joinLines(0, 0);
        QCOMPARE(text(), QString());
    }
    void lineTrim()
    {
        openCode(QStringLiteral("a  \n\t\nb\t \nc"));
        place(1);
        m_lines->trimSpaces(1, 1, LineTools::TrimTrailing);
        QCOMPARE(text(), QStringLiteral("a\n\nb\nc"));
        QCOMPARE(caret(), 1);
        undoesTo(QStringLiteral("a  \n\t\nb\t \nc"));
        openPlain(QStringLiteral("  a \n\t b\n c"));
        m_lines->trimSpaces(0, 0, LineTools::TrimLeading);
        QCOMPARE(text(), QStringLiteral("a \nb\nc"));
        openPlain(QStringLiteral("  a \n\t b\t\n   \n c"));
        m_lines->trimSpaces(0, 0, LineTools::TrimBoth);
        QCOMPARE(text(), QStringLiteral("a\nb\n\nc"));
        openPlain(QStringLiteral(" a \n b \n c "));
        m_lines->trimSpaces(4, 8, LineTools::TrimBoth); // the second line only
        QCOMPARE(text(), QStringLiteral(" a \nb\n c "));
        m_lines->trimSpaces(0, 0, 9); // a bad mode does nothing
        QCOMPARE(text(), QStringLiteral(" a \nb\n c "));
    }
    void lineTabsAndSpaces()
    {
        openPlain(QStringLiteral("\ta\n  \tb\nc\td\n"));
        m_lines->tabsToSpaces(0, 0);
        QCOMPARE(text(), QStringLiteral("    a\n    b\nc   d\n"));
        undoesTo(QStringLiteral("\ta\n  \tb\nc\td\n"));
        openPlain(QStringLiteral("        a\n      b\n  c\n x  y\n\t  z"));
        m_lines->spacesToLeadingTabs(0, 0);
        QCOMPARE(text(), QStringLiteral("\t\ta\n\t  b\n  c\n x  y\n\t  z"));
        openPlain(QStringLiteral("    a\n    b"));
        attachLines(2); // the document's width
        m_lines->spacesToLeadingTabs(0, 0);
        QCOMPARE(text(), QStringLiteral("\t\ta\n\t\tb"));
        m_lines->tabsToSpaces(0, 3); // the first line only
        QCOMPARE(text(), QStringLiteral("    a\n\t\tb"));
    }
    void lineChangeCase()
    {
        openPlain(QStringLiteral("hello wORLD foo"));
        m_lines->changeCase(0, 11, LineTools::UpperCase);
        QCOMPARE(text(), QStringLiteral("HELLO WORLD foo"));
        m_lines->changeCase(0, 11, LineTools::LowerCase);
        QCOMPARE(text(), QStringLiteral("hello world foo"));
        m_lines->changeCase(0, 15, LineTools::TitleCase);
        QCOMPARE(text(), QStringLiteral("Hello World Foo"));
        m_lines->changeCase(7, 7, LineTools::UpperCase); // the word at the caret
        QCOMPARE(text(), QStringLiteral("Hello WORLD Foo"));
        m_lines->changeCase(0, 5, 7); // a bad mode does nothing
        QCOMPARE(text(), QStringLiteral("Hello WORLD Foo"));
        m_lines->changeCase(0, 15, LineTools::InvertCase);
        QCOMPARE(text(), QStringLiteral("hELLO world fOO"));
        undoesTo(QStringLiteral("Hello WORLD Foo"));
        openPlain(QStringLiteral("hELLO there. how ARE you? fine!ok\nnext line. 3.5 apples"));
        m_lines->changeCase(0, text().size(), LineTools::SentenceCase);
        QCOMPARE(text(), QStringLiteral("Hello there. How are you? Fine!ok\nNext line. 3.5 apples"));
        QCOMPARE(LineTools::convertCase(QString(), LineTools::SentenceCase), QString());
    }
    void lineToolsReadOnlyAreNotEdited()
    {
        for (bool code : {false, true}) {
            if (code) {
                openCode(QStringLiteral("b\na  \n\tc"));
            } else {
                openPlain(QStringLiteral("b\na  \n\tc"));
            }
            m_edit->setProperty("readOnly", true);
            m_lines->duplicateLines(0, 3);
            m_lines->moveLines(0, 0, true);
            m_lines->deleteLines(0, 3);
            m_lines->joinLines(0, 3);
            m_lines->sortLines(0, 0, 0);
            m_lines->reverseLines(0, 0);
            m_lines->removeDuplicateLines(0, 0);
            m_lines->removeEmptyLines(0, 0);
            m_lines->trimSpaces(0, 0, LineTools::TrimBoth);
            m_lines->tabsToSpaces(0, 0);
            m_lines->spacesToLeadingTabs(0, 0);
            m_lines->changeCase(0, 3, LineTools::UpperCase);
            QCOMPARE(text(), QStringLiteral("b\na  \n\tc"));
        }
    }
    void lineToolsDetachedDoNothing()
    {
        LineTools tools;
        tools.sortLines(0, 0, 0);
        tools.duplicateLines(0, 0);
        tools.changeCase(0, 1, 0);
        openPlain(QStringLiteral("b\na"));
        m_lines->setTextEdit(nullptr);
        m_lines->sortLines(0, 0, 0);
        QCOMPARE(text(), QStringLiteral("b\na"));
        m_lines->setTextEdit(m_edit.get());
        m_edit.reset(); // the TextEdit goes first
        m_lines->sortLines(0, 0, 0);
        m_lines->duplicateLines(0, 0);
    }
    void lineMoveOverEqualLines()
    {
        openPlain(QStringLiteral("x\n\n\n\ny"));
        m_lines->moveLines(2, 2, true); // blank line down past a blank line
        QCOMPARE(text(), QStringLiteral("x\n\n\n\ny"));
        QCOMPARE(caret(), 3);
        m_lines->moveLines(3, 3, true);
        QCOMPARE(caret(), 4);
        m_lines->moveLines(4, 4, false);
        QCOMPARE(caret(), 3);
    }
    void lineSelectionEndsAtColumnZero()
    {
        openPlain(QStringLiteral("c\nb\na\nz"));
        m_lines->sortLines(0, 6, 0); // "c\nb\na\n" selected
        QCOMPARE(text(), QStringLiteral("a\nb\nc\nz"));
        QCOMPARE(selStart(), 0);
        QCOMPARE(selEnd(), 6);
        openCode(QStringLiteral("a\nb\nc"));
        select(0, 4); // "a\nb\n"
        m_code->indentLines(0, 4);
        QCOMPARE(text(), QStringLiteral("    a\n    b\nc"));
        QCOMPARE(selStart(), 4);
        QCOMPARE(selEnd(), 12); // still the start of "c"
    }
    void lineCaretStaysOnItsLine()
    {
        openPlain(QStringLiteral("a\na\nb\nb\nc"));
        place(8); // "c"
        m_lines->removeDuplicateLines(8, 8);
        QCOMPARE(text(), QStringLiteral("a\nb\nc"));
        QCOMPARE(caret(), 4); // the same line number, clamped to the new count
        openPlain(QStringLiteral("a\na\nb"));
        place(0);
        m_lines->removeDuplicateLines(0, 0);
        QCOMPARE(caret(), 0);
    }
    void lineCaseKeepsSelection()
    {
        openPlain(QStringLiteral("one two"));
        select(0, 3);
        m_lines->changeCase(0, 3, LineTools::UpperCase);
        QCOMPARE(text(), QStringLiteral("ONE two"));
        QCOMPARE(selStart(), 0);
        QCOMPARE(selEnd(), 3);
        openPlain(QStringLiteral("straße"));
        select(0, 6);
        m_lines->changeCase(0, 6, LineTools::UpperCase);
        QCOMPARE(text(), QStringLiteral("STRASSE"));
        QCOMPARE(selEnd(), 7);
        openPlain(QStringLiteral("one two"));
        place(5);
        m_lines->changeCase(5, 5, LineTools::UpperCase); // the word at the caret
        QCOMPARE(text(), QStringLiteral("one TWO"));
        QCOMPARE(caret(), 5);
    }
    void lineCaseUnicode()
    {
        auto n = [](const QString &s) { return s.normalized(QString::NormalizationForm_C); };
        QCOMPARE(LineTools::convertCase(QStringLiteral("cafés au lait"), LineTools::TitleCase), QStringLiteral("Cafés Au Lait"));
        QCOMPARE(n(LineTools::convertCase(QStringLiteral("İSTANBUL"), LineTools::TitleCase)), n(QStringLiteral("İstanbul")));
        QCOMPARE(LineTools::convertCase(QStringLiteral("DON’T don't"), LineTools::TitleCase), QStringLiteral("Don’t Don't"));
        QCOMPARE(LineTools::convertCase(QString::fromUcs4(U"\U00010428\U00010429 \U00010428\U00010429"), LineTools::TitleCase), QString::fromUcs4(U"\U00010400\U00010429 \U00010400\U00010429"));
        QCOMPARE(LineTools::convertCase(QString::fromUcs4(U"\U00010428\U00010429"), LineTools::UpperCase), QString::fromUcs4(U"\U00010400\U00010401"));
        QCOMPARE(LineTools::convertCase(QString::fromUcs4(U"\U00010400a"), LineTools::InvertCase), QString::fromUcs4(U"\U00010428A"));
        QCOMPARE(LineTools::convertCase(QStringLiteral("ßtraße"), LineTools::TitleCase), QStringLiteral("SStraße"));
        QCOMPARE(LineTools::convertCase(QString::fromUcs4(U"\U00010428a. \U00010428b"), LineTools::SentenceCase), QString::fromUcs4(U"\U00010400a. \U00010400b"));
    }
    void lineSortNumberForms()
    {
        openPlain(QStringLiteral("1e2\n.5\n-.5\n3\n1e999\n-1e999\n2e-999\nx"));
        m_lines->sortLines(0, 0, LineTools::Numeric);
        QCOMPARE(text(), QStringLiteral("x\n-1e999\n-.5\n2e-999\n.5\n3\n1e2\n1e999"));
    }
    void lineTabsCountCodePoints()
    {
        openPlain(QString::fromUcs4(U"\U0001F600\tx"));
        m_lines->tabsToSpaces(0, 0);
        QCOMPARE(text(), QString::fromUcs4(U"\U0001F600") + QStringLiteral("   x"));
    }
    void codeEditsAreOneStepAndKeepCaret()
    {
        openCode(QStringLiteral("a\nb\nc"));
        place(3); // before "b"... after its start
        m_code->indentLines(3, 3);
        QCOMPARE(text(), QStringLiteral("a\n    b\nc"));
        QCOMPARE(caret(), 7); // follows the text
        undoesTo(QStringLiteral("a\nb\nc"));
        m_code->toggleComment(0, 5);
        QCOMPARE(text(), QStringLiteral("// a\n// b\n// c"));
        undoesTo(QStringLiteral("a\nb\nc"));
    }
    // Time on a big file: NP_LINES_FILE=/path/to/log. Skipped without it.
    void lineToolsLargeFile()
    {
        const QByteArray path = qgetenv("NP_LINES_FILE");
        if (path.isEmpty()) {
            QSKIP("set NP_LINES_FILE to time the line tools on a big file");
        }
        QFile f(QString::fromLocal8Bit(path));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString big = QString::fromUtf8(f.readAll());
        const struct {
            const char *name;
            std::function<void()> run;
        } cases[] = {
            {"remove duplicates", [&] { m_lines->removeDuplicateLines(0, 0); }},
            {"sort", [&] { m_lines->sortLines(0, 0, 0); }},
            {"sort numeric descending", [&] { m_lines->sortLines(0, 0, LineTools::Numeric | LineTools::Descending); }},
            {"reverse", [&] { m_lines->reverseLines(0, 0); }},
            {"join", [&] { m_lines->joinLines(0, int(big.size())); }},
            {"trim both", [&] { m_lines->trimSpaces(0, 0, LineTools::TrimBoth); }},
            {"remove empty", [&] { m_lines->removeEmptyLines(0, 0); }},
            {"move down (one line)", [&] { m_lines->moveLines(0, 0, true); }},
            {"duplicate (one line)", [&] { m_lines->duplicateLines(0, 0); }},
        };
        for (const auto &c : cases) {
            openPlain(big);
            QElapsedTimer timer;
            timer.start();
            c.run();
            const qint64 ms = timer.elapsed();
            QMetaObject::invokeMethod(m_edit.get(), "undo");
            qInfo().noquote() << "LINES" << c.name << ms << "ms," << (text() == big ? "undo ok" : "undo DIFFERS");
        }
    }
    // Time on a big file: NP_CODE_FILE=/path/to/code.cpp. Skipped without it.
    void codeBigIndentAndComment()
    {
        const QByteArray path = qgetenv("NP_CODE_FILE");
        if (path.isEmpty()) {
            QSKIP("set NP_CODE_FILE to time indent and comment on a big file");
        }
        QFile f(QString::fromLocal8Bit(path));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString big = QString::fromUtf8(f.readAll());
        const struct {
            const char *name;
            std::function<void()> run;
        } cases[] = {
            {"indent", [&] { m_code->indentLines(0, int(big.size())); }},
            {"outdent", [&] { m_code->outdentLines(0, int(big.size())); }},
            {"toggle comment", [&] { m_code->toggleComment(0, int(big.size())); }},
        };
        for (const auto &c : cases) {
            openCode(big);
            QElapsedTimer timer;
            timer.start();
            c.run();
            const qint64 ms = timer.elapsed();
            QMetaObject::invokeMethod(m_edit.get(), "undo");
            qInfo().noquote() << "CODE" << c.name << ms << "ms," << (text() == big ? "undo ok" : "undo DIFFERS");
        }
    }
    void codeAutoIndentColonAndComments()
    {
        // A colon opens a block only where it means one.
        openCode(QStringLiteral("case x:"), QStringLiteral("C++"));
        place(7);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("case x:\n"));
        // A bracket after the comment marker doesn't.
        openCode(QStringLiteral("a(); // see {"), QStringLiteral("C++"));
        place(13);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("a(); // see {\n"));
        openCode(QStringLiteral("x = 1  # why:"), QStringLiteral("Python"));
        place(13);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("x = 1  # why:\n"));
    }
    void codeEnterAutoIndent()
    {
        openCode(QStringLiteral("  if (x) {"));
        place(10);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("  if (x) {\n      "));
        QCOMPARE(caret(), text().size());
        // Plain line keeps its indent.
        openCode(QStringLiteral("  foo();"));
        place(8);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("  foo();\n  "));
        // Between { and }: the } goes on its own line.
        openCode(QStringLiteral("  f() {}"));
        place(7);
        press(Qt::Key_Enter);
        QCOMPARE(text(), QStringLiteral("  f() {\n      \n  }"));
        QCOMPARE(caret(), 14);
        // Python's colon, with trailing spaces.
        openCode(QStringLiteral("def f():  "), QStringLiteral("Python"));
        place(10);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("def f():  \n    "));
        // Tabs mode.
        openCode(QStringLiteral("\tx = [\n"), QStringLiteral("C++"), false);
        place(6);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("\tx = [\n\t\t\n"));
        // A selection is replaced; one undo step restores it.
        openCode(QStringLiteral("ab cd"));
        select(1, 4);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("a\nd"));
        QMetaObject::invokeMethod(m_edit.get(), "undo");
        QCOMPARE(text(), QStringLiteral("ab cd"));
        // Read-only: passes through (and the TextEdit refuses it).
        openCode(QStringLiteral("x {"));
        m_edit->setProperty("readOnly", true);
        place(3);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("x {"));
    }
    void codeTabKey()
    {
        openCode(QStringLiteral("ab"));
        place(2);
        press(Qt::Key_Tab);
        QCOMPARE(text(), QStringLiteral("ab  ")); // to column 4
        place(0);
        press(Qt::Key_Tab);
        QCOMPARE(text(), QStringLiteral("    ab  "));
        QCOMPARE(caret(), 4);
        // Tabs mode.
        openCode(QStringLiteral("ab"), QStringLiteral("C++"), false);
        place(1);
        press(Qt::Key_Tab);
        QCOMPARE(text(), QStringLiteral("a\tb"));
        // A selection inside a line is replaced.
        openCode(QStringLiteral("abcd"), QStringLiteral("C++"), true, 2);
        select(1, 3);
        press(Qt::Key_Tab);
        QCOMPARE(text(), QStringLiteral("a d")); // column 1 to the next multiple of 2
        // Over several lines: indentLines; Backtab: outdentLines.
        openCode(QStringLiteral("a\nb\nc"));
        select(0, 3);
        press(Qt::Key_Tab);
        QCOMPARE(text(), QStringLiteral("    a\n    b\nc"));
        QKeyEvent back(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        QCoreApplication::sendEvent(m_edit.get(), &back);
        QCOMPARE(text(), QStringLiteral("a\nb\nc"));
        // Backtab with no selection outdents the caret's line.
        openCode(QStringLiteral("    a"));
        place(5);
        QCoreApplication::sendEvent(m_edit.get(), &back);
        QCOMPARE(text(), QStringLiteral("a"));
    }
    void codeClosingBracketDedents()
    {
        openCode(QStringLiteral("if (x) {\n    foo();\n    "));
        place(text().size());
        type(QStringLiteral("}"));
        QCOMPARE(text(), QStringLiteral("if (x) {\n    foo();\n}"));
        QCOMPARE(caret(), text().size());
        QMetaObject::invokeMethod(m_edit.get(), "undo");
        QCOMPARE(text(), QStringLiteral("if (x) {\n    foo();\n    "));
        // Not the first non-space character: no dedent.
        openCode(QStringLiteral("    a"));
        place(5);
        type(QStringLiteral(")"));
        QCOMPARE(text(), QStringLiteral("    a)"));
        // Nothing to dedent: passes through.
        openCode(QStringLiteral(""));
        type(QStringLiteral("]"));
        QCOMPARE(text(), QStringLiteral("]"));
        // Tabs.
        openCode(QStringLiteral("\t\t"), QStringLiteral("C++"), false);
        place(2);
        type(QStringLiteral("}"));
        QCOMPARE(text(), QStringLiteral("\t}"));
    }
    void codeDetachedPassesKeysThrough()
    {
        openCode(QStringLiteral("x {"));
        m_code->setTextEdit(nullptr);
        place(3);
        press(Qt::Key_Return);
        QCOMPARE(text(), QStringLiteral("x {\n"));
    }

};

QTEST_MAIN(EditorTest)
#include "editor_test.moc"
