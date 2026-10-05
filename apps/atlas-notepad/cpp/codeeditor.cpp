// Light coding: highlighting from KSyntaxHighlighting and the editing keys a
// code file expects. See codeeditor.h.
#include "codeeditor.h"
#include "codehighlighter.h"
#include "linetools.h"

#include <algorithm>

#include <QCollator>
#include <QKeyEvent>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>

#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>

namespace
{
constexpr int kBracketScanLimit = 20'000;

bool isBlank(const QString &s)
{
    return std::all_of(s.begin(), s.end(), [](QChar c) { return c == u' ' || c == u'\t'; });
}

int leadingLength(const QString &s)
{
    int n = 0;
    while (n < s.size() && (s[n] == u' ' || s[n] == u'\t')) {
        ++n;
    }
    return n;
}

// The column a position in text is at, tabs counted to the next multiple of width.
int columnOf(const QString &text, int pos, int width)
{
    int col = 0;
    for (int i = 0; i < pos && i < text.size(); ++i) {
        col = text[i] == u'\t' ? col + width - col % width : col + 1;
    }
    return col;
}
} // namespace

KSyntaxHighlighting::Repository &codeRepository()
{
    static KSyntaxHighlighting::Repository repository;
    return repository;
}

CodeEditor::CodeEditor(QObject *parent)
    : QObject(parent)
{
}

CodeEditor::~CodeEditor()
{
    if (m_highlighter) {
        delete m_highlighter.data();
    }
}

void CodeEditor::setTextEdit(QQuickItem *edit)
{
    if (edit == m_edit) {
        return;
    }
    if (m_edit) {
        m_edit->removeEventFilter(this);
    }
    if (m_highlighter) {
        delete m_highlighter.data();
    }
    m_edit = edit;
    m_doc = nullptr;
    if (edit) {
        auto *textDocument = qobject_cast<QQuickTextDocument *>(edit->property("textDocument").value<QObject *>());
        m_doc = textDocument ? textDocument->textDocument() : nullptr;
        edit->installEventFilter(this);
        applyHighlighting();
    }
    Q_EMIT textEditChanged();
}

void CodeEditor::setLanguage(const QString &name)
{
    if (name == m_language) {
        return;
    }
    m_language = name;
    m_def = name.isEmpty() ? KSyntaxHighlighting::Definition() : codeRepository().definitionForName(name);
    applyHighlighting();
    Q_EMIT languageChanged();
}

void CodeEditor::setDark(bool dark)
{
    if (dark == m_dark) {
        return;
    }
    m_dark = dark;
    if (m_highlighter) {
        m_highlighter->setTheme(codeRepository().defaultTheme(dark ? KSyntaxHighlighting::Repository::DarkTheme
                                                                    : KSyntaxHighlighting::Repository::LightTheme)); // highlights again
    }
    Q_EMIT darkChanged();
}

void CodeEditor::setInsertSpaces(bool on)
{
    if (on != m_spaces) {
        m_spaces = on;
        Q_EMIT insertSpacesChanged();
    }
}

void CodeEditor::setIndentWidth(int width)
{
    width = qBound(1, width, 16);
    if (width != m_width) {
        m_width = width;
        Q_EMIT indentWidthChanged();
    }
}

void CodeEditor::rehighlightNow()
{
    if (m_highlighter) {
        m_highlighter->rehighlightAll(true);
    }
}

void CodeEditor::applyHighlighting()
{
    if (!m_doc) {
        return;
    }
    if (!m_def.isValid()) {
        if (m_highlighter) {
            delete m_highlighter.data(); // clears its formats
        }
        return;
    }
    if (!m_highlighter) {
        m_highlighter = new CodeHighlighter(m_doc);
        m_highlighter->setTheme(codeRepository().defaultTheme(m_dark ? KSyntaxHighlighting::Repository::DarkTheme
                                                                      : KSyntaxHighlighting::Repository::LightTheme));
    }
    m_highlighter->setDefinition(m_def); // rehighlights
}

bool CodeEditor::editable() const
{
    return m_doc && m_edit && !m_edit->property("readOnly").toBool();
}

QStringList CodeEditor::languages()
{
    QStringList names;
    const auto defs = codeRepository().definitions();
    for (const auto &def : defs) {
        if (!def.isHidden()) {
            names << def.name();
        }
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), collator);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

QString CodeEditor::commentMarker() const
{
    if (!m_def.isValid()) {
        return {};
    }
    const QString single = m_def.singleLineCommentMarker();
    if (!single.isEmpty()) {
        return single;
    }
    return m_def.multiLineCommentMarker().first;
}

QString CodeEditor::indentUnit(int column) const
{
    if (!m_spaces) {
        return QStringLiteral("\t");
    }
    return QString(m_width - column % m_width, u' ');
}

// The first and last block touched by [from, to).
void CodeEditor::firstLast(int from, int to, QTextBlock *first, QTextBlock *last) const
{
    const int end = qMax(0, m_doc->characterCount() - 1);
    from = qBound(0, from, end);
    to = qBound(0, to, end);
    if (to < from) {
        std::swap(from, to);
    }
    *first = m_doc->findBlock(from);
    *last = to > from ? m_doc->findBlock(to - 1) : *first;
}

// The text of the blocks first..last.
static QStringList blockTexts(const QTextBlock &first, const QTextBlock &last)
{
    QStringList lines;
    for (QTextBlock b = first; b.isValid() && b.blockNumber() <= last.blockNumber(); b = b.next()) {
        lines << b.text();
    }
    return lines;
}

// Swaps the lines in one edit; the caret and the selection follow the text.
void CodeEditor::replaceLines(const QTextBlock &first, const QStringList &old, const QStringList &lines)
{
    const int caret = m_edit->property("cursorPosition").toInt();
    const int selStart = m_edit->property("selectionStart").toInt();
    const int selEnd = m_edit->property("selectionEnd").toInt();
    const int anchor = caret == selStart ? selEnd : selStart;
    const int newCaret = LineTools::mapPosition(first, old, lines, caret);
    const int newAnchor = LineTools::mapPosition(first, old, lines, anchor);
    if (!LineTools::applyLines(m_doc, first, old, lines)) {
        return;
    }
    if (selStart != selEnd) {
        QMetaObject::invokeMethod(m_edit, "select", Q_ARG(int, newAnchor), Q_ARG(int, newCaret));
    } else {
        m_edit->setProperty("cursorPosition", newCaret);
    }
}

void CodeEditor::indentLines(int from, int to)
{
    if (!editable()) {
        return;
    }
    QTextBlock first, last;
    firstLast(from, to, &first, &last);
    const QString unit = m_spaces ? QString(m_width, u' ') : QStringLiteral("\t");
    const QStringList old = blockTexts(first, last);
    QStringList lines = old;
    for (QString &l : lines) {
        if (!l.isEmpty()) {
            l.prepend(unit);
        }
    }
    replaceLines(first, old, lines);
}

void CodeEditor::outdentLines(int from, int to)
{
    if (!editable()) {
        return;
    }
    QTextBlock first, last;
    firstLast(from, to, &first, &last);
    const QStringList old = blockTexts(first, last);
    QStringList lines = old;
    for (QString &text : lines) {
        int n = 0;
        if (!text.isEmpty() && text[0] == u'\t') {
            n = 1;
        } else {
            while (n < m_width && n < text.size() && text[n] == u' ') {
                ++n;
            }
        }
        text.remove(0, n);
    }
    replaceLines(first, old, lines);
}

void CodeEditor::toggleComment(int from, int to)
{
    if (!editable() || !m_def.isValid()) {
        return;
    }
    QString open = m_def.singleLineCommentMarker();
    QString close;
    if (open.isEmpty()) {
        const auto multi = m_def.multiLineCommentMarker();
        open = multi.first;
        close = multi.second;
    }
    if (open.isEmpty()) {
        return;
    }
    QTextBlock first, last;
    firstLast(from, to, &first, &last);

    // The leading whitespace every non-blank line shares, and whether all are commented.
    const QStringList old = blockTexts(first, last);
    QString shared;
    bool haveShared = false;
    bool allCommented = true;
    bool any = false;
    for (const QString &text : old) {
        if (isBlank(text)) {
            continue;
        }
        any = true;
        const QString lead = text.left(leadingLength(text));
        if (!haveShared) {
            shared = lead;
            haveShared = true;
        } else {
            int k = 0;
            while (k < shared.size() && k < lead.size() && shared[k] == lead[k]) {
                ++k;
            }
            shared.truncate(k);
        }
        const QString body = text.mid(lead.size()).trimmed();
        if (!body.startsWith(open) || (!close.isEmpty() && !body.endsWith(close))) {
            allCommented = false;
        }
    }
    if (!any) {
        return;
    }

    QStringList lines = old;
    for (QString &line : lines) {
        const QString text = line;
        if (isBlank(text)) {
            continue;
        }
        if (!allCommented) {
            line.insert(shared.size(), open + u' ');
            if (!close.isEmpty()) {
                line += u' ' + close;
            }
            continue;
        }
        if (!close.isEmpty()) {
            // Strip the closing marker first so the opening one's offsets stay valid.
            int end = int(text.size());
            while (end > 0 && text[end - 1].isSpace()) {
                --end;
            }
            int len = int(close.size());
            if (end - len > 0 && text[end - len - 1] == u' ') {
                ++len;
            }
            line.remove(end - len, len);
        }
        const int start = leadingLength(text);
        int len = int(open.size());
        if (start + len < text.size() && text[start + len] == u' ') {
            ++len;
        }
        line.remove(start, len);
    }
    replaceLines(first, old, lines);
}

QPoint CodeEditor::bracketPair(int cursor)
{
    if (!m_doc) {
        return {-1, -1};
    }
    static const QString opens = QStringLiteral("{([");
    static const QString closes = QStringLiteral("})]");
    const int end = m_doc->characterCount() - 1;
    for (int pos : {cursor - 1, cursor}) {
        if (pos < 0 || pos >= end) {
            continue;
        }
        const QChar ch = m_doc->characterAt(pos);
        const qsizetype o = opens.indexOf(ch);
        const qsizetype cl = closes.indexOf(ch);
        if (o < 0 && cl < 0) {
            continue;
        }
        const QChar self = ch;
        const QChar other = o >= 0 ? closes[o] : opens[cl];
        const int step = o >= 0 ? 1 : -1;
        int depth = 0;
        int scanned = 0;
        for (int i = pos + step; i >= 0 && i < end && scanned < kBracketScanLimit; i += step, ++scanned) {
            const QChar x = m_doc->characterAt(i);
            if (x == self) {
                ++depth;
            } else if (x == other) {
                if (depth == 0) {
                    return {pos, i};
                }
                --depth;
            }
        }
    }
    return {-1, -1};
}

bool CodeEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_edit || !m_doc || event->type() != QEvent::KeyPress || !editable()) {
        return false;
    }
    auto *key = static_cast<QKeyEvent *>(event);
    const auto mods = key->modifiers() & ~Qt::KeypadModifier;
    bool handled = false;
    switch (key->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        handled = mods == Qt::NoModifier && newline();
        break;
    case Qt::Key_Tab:
        handled = mods == Qt::NoModifier && tab();
        break;
    case Qt::Key_Backtab:
        if (mods == Qt::ShiftModifier || mods == Qt::NoModifier) {
            outdentLines(m_edit->property("selectionStart").toInt(), m_edit->property("selectionEnd").toInt());
            handled = true;
        }
        break;
    default:
        if ((mods == Qt::NoModifier || mods == Qt::ShiftModifier) && key->text().size() == 1) {
            handled = closingBracket(key->text());
        }
        break;
    }
    if (handled) {
        event->accept();
    }
    return handled;
}

bool CodeEditor::newline()
{
    const int selStart = m_edit->property("selectionStart").toInt();
    const int selEnd = m_edit->property("selectionEnd").toInt();
    QTextCursor c(m_doc);
    c.setPosition(qMin(selStart, selEnd));
    c.setPosition(qMax(selStart, selEnd), QTextCursor::KeepAnchor);
    c.beginEditBlock();
    c.removeSelectedText();
    const QTextBlock block = c.block();
    const QString text = block.text();
    const int col = c.positionInBlock();
    const QString before = text.left(col);
    const QString after = text.mid(col);
    const QString indent = text.left(qMin(col, leadingLength(text)));
    QString trimmed = before;
    while (!trimmed.isEmpty() && (trimmed.back() == u' ' || trimmed.back() == u'\t')) {
        trimmed.chop(1);
    }
    // Text after a line-comment marker doesn't open a block.
    const QString marker = m_def.isValid() ? m_def.singleLineCommentMarker() : QString();
    if (!marker.isEmpty()) {
        const qsizetype at = trimmed.indexOf(marker);
        if (at >= 0) {
            trimmed.truncate(at);
            while (!trimmed.isEmpty() && (trimmed.back() == u' ' || trimmed.back() == u'\t')) {
                trimmed.chop(1);
            }
        }
    }
    const QChar lastCh = trimmed.isEmpty() ? QChar() : trimmed.back();
    static const QStringList colonBlocks = {QStringLiteral("Python"), QStringLiteral("YAML"), QStringLiteral("Nim"),
                                            QStringLiteral("CoffeeScript"), QStringLiteral("Makefile")};
    const bool deeper = lastCh == u'{' || lastCh == u'(' || lastCh == u'[' || (lastCh == u':' && m_def.isValid() && colonBlocks.contains(m_def.name()));
    const QString unit = m_spaces ? QString(m_width, u' ') : QStringLiteral("\t");
    const QString afterTrim = after.trimmed();
    const qsizetype openAt = QStringLiteral("{([").indexOf(lastCh);
    const bool split = openAt >= 0 && !afterTrim.isEmpty() && afterTrim[0] == QStringLiteral("})]")[openAt];
    int caret;
    if (split) {
        c.insertText(u'\n' + indent + unit);
        caret = c.position();
        c.insertText(u'\n' + indent);
    } else {
        c.insertText(u'\n' + indent + (deeper ? unit : QString()));
        caret = c.position();
    }
    c.endEditBlock();
    m_edit->setProperty("cursorPosition", caret);
    return true;
}

bool CodeEditor::tab()
{
    const int selStart = qMin(m_edit->property("selectionStart").toInt(), m_edit->property("selectionEnd").toInt());
    const int selEnd = qMax(m_edit->property("selectionStart").toInt(), m_edit->property("selectionEnd").toInt());
    if (selStart != selEnd && m_doc->findBlock(selStart) != m_doc->findBlock(selEnd)) {
        indentLines(selStart, selEnd);
        return true;
    }
    QTextCursor c(m_doc);
    c.setPosition(selStart);
    c.setPosition(selEnd, QTextCursor::KeepAnchor);
    c.beginEditBlock();
    c.removeSelectedText();
    c.insertText(indentUnit(columnOf(c.block().text(), c.positionInBlock(), m_width)));
    c.endEditBlock();
    m_edit->setProperty("cursorPosition", c.position());
    return true;
}

// A closing bracket typed as the first thing on a line takes the line one level out.
bool CodeEditor::closingBracket(const QString &text)
{
    if (text != u"}" && text != u")" && text != u"]") {
        return false;
    }
    const int pos = m_edit->property("cursorPosition").toInt();
    if (m_edit->property("selectionStart").toInt() != m_edit->property("selectionEnd").toInt()) {
        return false;
    }
    QTextCursor c(m_doc);
    c.setPosition(pos);
    const QTextBlock block = c.block();
    const QString line = block.text();
    const int col = c.positionInBlock();
    if (col == 0 || !isBlank(line.left(col))) {
        return false;
    }
    int n = 0;
    if (line[0] == u'\t') {
        n = 1;
    } else {
        while (n < m_width && n < col && line[n] == u' ') {
            ++n;
        }
    }
    if (n == 0) {
        return false;
    }
    c.beginEditBlock();
    QTextCursor d(m_doc);
    d.setPosition(block.position());
    d.setPosition(block.position() + n, QTextCursor::KeepAnchor);
    d.removeSelectedText();
    c.insertText(text);
    c.endEditBlock();
    m_edit->setProperty("cursorPosition", c.position());
    return true;
}
