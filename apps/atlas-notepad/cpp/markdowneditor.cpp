// Editing in the Formatted view: the caret steps over hidden syntax as if it
// weren't there, Backspace and Delete take the character you see, Enter
// carries a list on, and the toolbar's edits go through QTextCursor, so each
// is one step on the document's own undo stack.
#include "markdown.h"

#include <algorithm>

#include <QAbstractTextDocumentLayout>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextLayout>

MarkdownEditor::MarkdownEditor(QObject *parent)
    : QObject(parent)
{
    m_style.monoFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    m_style.text = Qt::black;
    m_style.dim = Qt::gray;
    m_style.link = Qt::blue;
    m_style.code = QColor(0, 0, 0, 18);
    m_accent = Qt::blue;
}

void MarkdownEditor::setTextEdit(QQuickItem *edit)
{
    if (edit == m_edit) {
        return;
    }
    if (m_edit) {
        m_edit->removeEventFilter(this);
        disconnect(m_edit, nullptr, this, nullptr);
    }
    if (m_highlighter) {
        delete m_highlighter;
        // That cleared every line's formats: a plain-text spell highlighter
        // on the same document draws its underlines again.
        if (m_spell) {
            QMetaObject::invokeMethod(m_spell, &SpellChecker::rehighlight, Qt::QueuedConnection);
        }
    }
    m_edit = edit;
    m_doc = nullptr;
    if (edit) {
        auto *textDocument = qobject_cast<QQuickTextDocument *>(edit->property("textDocument").value<QObject *>());
        m_doc = textDocument ? textDocument->textDocument() : nullptr;
        edit->installEventFilter(this);
        connect(edit, SIGNAL(cursorPositionChanged()), this, SLOT(snapCursor()));
        // Replacing the whole text (load, reload) can move the caret without
        // its signal: the fence lines' reveal follows it again.
        connect(edit, SIGNAL(textChanged()), this, SLOT(syncCaret()));
        if (m_doc) {
            m_highlighter = new MarkdownHighlighter(m_doc);
            m_highlighter->setSpellChecker(m_spell);
            m_highlighter->setCaretSource([edit = QPointer<QQuickItem>(edit)] {
                return edit ? edit->property("cursorPosition").toInt() : 0;
            });
            m_highlighter->setCaret(cursor());
            m_highlighter->setStyle(m_style);
        }
    }
    Q_EMIT textEditChanged();
}

void MarkdownEditor::setFormatted(bool formatted)
{
    if (formatted == m_style.formatted) {
        return;
    }
    m_style.formatted = formatted;
    applyStyle();
    Q_EMIT formattedChanged();
}

void MarkdownEditor::setFont(const QFont &font)
{
    if (font == m_style.font) {
        return;
    }
    m_style.font = font;
    applyStyle();
    Q_EMIT styleChanged();
}

#define NP_COLOR_SETTER(name, member)                                                                                                                \
    void MarkdownEditor::name(const QColor &c)                                                                                                       \
    {                                                                                                                                                \
        if (c != m_style.member) {                                                                                                                   \
            m_style.member = c;                                                                                                                      \
            applyStyle();                                                                                                                            \
            Q_EMIT styleChanged();                                                                                                                   \
        }                                                                                                                                            \
    }
NP_COLOR_SETTER(setTextColor, text)
NP_COLOR_SETTER(setDimColor, dim)
NP_COLOR_SETTER(setLinkColor, link)
NP_COLOR_SETTER(setCodeColor, code)
#undef NP_COLOR_SETTER

// QML sets the style one property at a time at start: highlight once, after.
void MarkdownEditor::applyStyle()
{
    if (m_stylePending) {
        return;
    }
    m_stylePending = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_stylePending = false;
            if (m_highlighter) {
                    m_highlighter->setStyle(m_style);
            }
        },
        Qt::QueuedConnection);
}

void MarkdownEditor::setSpellChecker(SpellChecker *spell)
{
    if (spell == m_spell) {
        return;
    }
    if (m_spell) {
        disconnect(m_spell, nullptr, this, nullptr);
    }
    m_spell = spell;
    if (spell) {
        // On, off, a word added: highlight again (once).
        connect(spell, &SpellChecker::changed, this, &MarkdownEditor::applyStyle);
    }
    if (m_highlighter) {
        m_highlighter->setSpellChecker(spell);
        applyStyle();
    }
    Q_EMIT spellCheckerChanged();
}

void MarkdownEditor::rehighlightNow()
{
    if (m_highlighter) {
        m_highlighter->rehighlightAll(true);
    }
}

QPointF MarkdownEditor::textOrigin() const
{
    if (!m_edit) {
        return {};
    }
    return {m_edit->property("leftPadding").toReal(), m_edit->property("topPadding").toReal()};
}

int MarkdownEditor::cursor() const
{
    return m_edit->property("cursorPosition").toInt();
}

int MarkdownEditor::anchor() const
{
    const int start = m_edit->property("selectionStart").toInt();
    const int end = m_edit->property("selectionEnd").toInt();
    return cursor() == start ? end : start;
}

bool MarkdownEditor::hasSelection() const
{
    return m_edit->property("selectionStart").toInt() != m_edit->property("selectionEnd").toInt();
}

void MarkdownEditor::select(int anchor, int position)
{
    const bool was = m_snapping;
    m_snapping = true;
    m_edit->setProperty("cursorPosition", anchor);
    if (position != anchor) {
        QMetaObject::invokeMethod(m_edit, "moveCursorSelection", Q_ARG(int, position));
    }
    m_snapping = was;
}

bool MarkdownEditor::editable() const
{
    return m_doc && m_edit && !m_edit->property("readOnly").toBool();
}

bool MarkdownEditor::hiddenEndingAt(int pos, int *start) const
{
    const QTextBlock block = m_doc->findBlock(pos);
    const BlockInfo *info = BlockInfo::of(block);
    if (!info) {
        return false;
    }
    const int rel = pos - block.position();
    for (const auto &[s, e] : info->hidden) {
        if (e == rel) {
            *start = block.position() + s;
            return true;
        }
    }
    return false;
}

bool MarkdownEditor::hiddenStartingAt(int pos, int *end) const
{
    const QTextBlock block = m_doc->findBlock(pos);
    const BlockInfo *info = BlockInfo::of(block);
    if (!info) {
        return false;
    }
    const int rel = pos - block.position();
    for (const auto &[s, e] : info->hidden) {
        if (s == rel) {
            *end = block.position() + e;
            return true;
        }
    }
    return false;
}

bool MarkdownEditor::hiddenAround(int pos, int *start, int *end) const
{
    const QTextBlock block = m_doc->findBlock(pos);
    const BlockInfo *info = BlockInfo::of(block);
    if (!info) {
        return false;
    }
    const int rel = pos - block.position();
    for (const auto &[s, e] : info->hidden) {
        if (s < rel && rel < e) {
            *start = block.position() + s;
            *end = block.position() + e;
            return true;
        }
    }
    return false;
}

// [from, to) is one character with hidden syntax on both sides. When that is
// the markers of one span (`**a**`, `~~a~~`, `` `a` ``), sets [*start, *end)
// to the span with its markers. Hidden ranges run together across neighbouring
// spans (`**a**[b](u)`), so only the delimiters next to the character count,
// and a link's `[` and `](url)` never do: its URL isn't lost with its last letter.
bool MarkdownEditor::loneSpan(int from, int to, int *start, int *end) const
{
    int hs;
    int he;
    if (!hiddenEndingAt(from, &hs) || !hiddenStartingAt(to, &he)) {
        return false;
    }
    auto delimiter = [](QChar c) {
        return c == u'*' || c == u'_' || c == u'~' || c == u'`';
    };
    int left = 0;
    while (from - left - 1 >= hs && delimiter(m_doc->characterAt(from - left - 1))) {
        ++left;
    }
    int right = 0;
    while (to + right < he && delimiter(m_doc->characterAt(to + right))) {
        ++right;
    }
    const int k = qMin(left, right);
    if (k == 0) {
        return false;
    }
    for (int i = 0; i < k; ++i) {
        if (m_doc->characterAt(from - 1 - i) != m_doc->characterAt(to + i)) {
            return false;
        }
    }
    *start = from - k;
    *end = to + k;
    return true;
}

// Hidden syntax has no width, so both its edges are one place on screen. A
// step goes past it and one character you see. Landing at the start of a line
// whose syntax opens it (a heading's "## "), the caret goes into the text.
int MarkdownEditor::stepRight(int pos) const
{
    int p = pos;
    int s;
    int e;
    while (hiddenStartingAt(p, &e) && e > p) {
        p = e;
    }
    QTextCursor c(m_doc);
    c.setPosition(p);
    if (!c.movePosition(QTextCursor::NextCharacter)) {
        return pos;
    }
    p = c.position();
    if (hiddenAround(p, &s, &e)) {
        p = e;
    } else if (p == m_doc->findBlock(p).position() && hiddenStartingAt(p, &e)) {
        p = e;
    }
    return p;
}

int MarkdownEditor::stepLeft(int pos) const
{
    int p = pos;
    int s;
    int e;
    while (hiddenEndingAt(p, &s) && s < p) {
        p = s;
    }
    QTextCursor c(m_doc);
    c.setPosition(p);
    if (!c.movePosition(QTextCursor::PreviousCharacter)) {
        return pos;
    }
    p = c.position();
    if (hiddenAround(p, &s, &e)) {
        p = s;
    }
    return p;
}

bool MarkdownEditor::moveCaret(int direction, bool extend)
{
    if (!extend && hasSelection()) {
        return false; // the TextEdit collapses the selection
    }
    const int from = cursor();
    const int to = direction > 0 ? stepRight(from) : stepLeft(from);
    select(extend ? anchor() : to, to);
    return true;
}

void MarkdownEditor::syncCaret()
{
    if (m_highlighter && m_edit) {
        m_highlighter->setCaret(cursor());
    }
}

// After a click, Up, Down, Home or End: out of hidden syntax.
void MarkdownEditor::snapCursor()
{
    if (m_highlighter && m_edit) {
        m_highlighter->setCaret(cursor());
    }
    if (!m_style.formatted || m_snapping || !m_doc || !m_edit || hasSelection()) {
        return;
    }
    const int p = cursor();
    int s;
    int e;
    int to = p;
    if (hiddenAround(p, &s, &e)) {
        to = e;
    } else if (p == m_doc->findBlock(p).position() && hiddenStartingAt(p, &e)) {
        to = e;
    }
    if (to != p) {
        select(to, to);
    }
}

bool MarkdownEditor::backspace()
{
    const int pos = cursor();
    const QTextBlock block = m_doc->findBlock(pos);
    const BlockInfo *info = BlockInfo::of(block);
    const int rel = pos - block.position();
    QTextCursor c(m_doc);

    // At the start of a heading's, item's or quote's text: back to a plain line.
    if (info && rel > 0 && rel == int(info->line.contentStart)) {
        const NpLine &l = info->line;
        int from = -1;
        int s;
        if (l.kind == Md::HeadingLine && hiddenEndingAt(pos, &s)) {
            from = s - block.position();
        } else if (l.kind == Md::Bullet || l.kind == Md::Numbered || l.kind == Md::Task) {
            from = info->listStart();
        } else if (!info->quoteMarks.isEmpty()) {
            from = info->quoteMarks.last();
        }
        if (from >= 0) {
            c.setPosition(block.position() + from);
            c.setPosition(pos, QTextCursor::KeepAnchor);
            c.removeSelectedText();
            return true;
        }
    }

    int p = pos;
    int s;
    while (hiddenEndingAt(p, &s) && s < p) {
        p = s;
    }
    if (p == 0) {
        return true;
    }
    c.setPosition(p);
    if (p == m_doc->findBlock(p).position()) {
        c.deletePreviousChar(); // joins the lines
        return true;
    }
    c.movePosition(QTextCursor::PreviousCharacter);
    int from = c.position();
    int to = p;
    // The last character of a span: the span's syntax goes with it.
    loneSpan(from, to, &from, &to);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    return true;
}

bool MarkdownEditor::deleteForward()
{
    const int pos = cursor();
    int p = pos;
    int e;
    while (hiddenStartingAt(p, &e) && e > p) {
        p = e;
    }
    const QTextBlock block = m_doc->findBlock(p);
    QTextCursor c(m_doc);
    c.setPosition(p);
    if (p == block.position() + block.length() - 1) {
        c.deleteChar(); // joins the lines (nothing at the end)
        return true;
    }
    c.movePosition(QTextCursor::NextCharacter);
    int from = p;
    int to = c.position();
    loneSpan(from, to, &from, &to);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    return true;
}

// Enter in a list item starts the next item; in an empty one it ends the list.
// Numbers the items of a numbered list from `block` on: `number`, then up,
// past items nested deeper, until the list ends. Reads the blocks' line info,
// which still holds for blocks the current edit hasn't touched. With `dry`
// nothing is changed; it tells whether the items are numbered that way already.
static bool renumber(QTextCursor &c, QTextBlock block, int listStart, qlonglong number, bool dry = false)
{
    for (; block.isValid(); block = block.next()) {
        const BlockInfo *info = BlockInfo::of(block);
        if (!info) {
            return true;
        }
        const NpLine &l = info->line;
        const bool list = l.kind == Md::Bullet || l.kind == Md::Numbered || l.kind == Md::Task;
        if (list && info->listStart() > listStart) {
            continue;
        }
        if (l.kind != Md::Numbered || info->listStart() != listStart) {
            return true;
        }
        const int digits = int(l.markerLen) - 1;
        const QString want = QString::number(number++);
        if (QStringView(block.text()).mid(l.markerStart, digits) != want) {
            if (dry) {
                return false;
            }
            c.setPosition(block.position() + int(l.markerStart));
            c.setPosition(block.position() + int(l.markerStart) + digits, QTextCursor::KeepAnchor);
            c.insertText(want);
        }
    }
    return true;
}

bool MarkdownEditor::newline()
{
    if (hasSelection()) {
        return false;
    }
    const int pos = cursor();
    const QTextBlock block = m_doc->findBlock(pos);
    const BlockInfo *info = BlockInfo::of(block);
    if (!info) {
        return false;
    }
    const NpLine &l = info->line;
    const bool list = l.kind == Md::Bullet || l.kind == Md::Numbered || l.kind == Md::Task;
    if (!list && info->quoteMarks.isEmpty()) {
        return false;
    }
    const QString text = block.text();
    if (pos - block.position() < int(l.contentStart)) {
        return false;
    }
    // Only an item or quoted text can be empty: a quoted heading, rule or
    // code line ends where its content starts and is continued instead.
    const bool canBeEmpty = list || l.kind == Md::Blank || l.kind == Md::Paragraph;
    bool empty = true;
    for (const QChar ch : QStringView(text).mid(l.contentStart)) {
        if (ch != u' ' && ch != u'\t') { // the spaces markdown.rs skips
            empty = false;
            break;
        }
    }
    QTextCursor c(m_doc);
    if (canBeEmpty && empty) {
        c.setPosition(block.position() + (list ? info->listStart() : 0));
        c.setPosition(block.position() + text.size(), QTextCursor::KeepAnchor);
        c.removeSelectedText();
        return true;
    }
    QString prefix;
    if (!list) {
        prefix = text.left(info->quoteMarks.last() + 1) + u' ';
    } else if (l.kind == Md::Numbered) {
        const int digits = int(l.markerLen) - 1;
        const qlonglong next = QStringView(text).mid(l.markerStart, digits).toLongLong() + 1;
        prefix = text.left(l.markerStart) + QString::number(next) + text.at(l.markerStart + digits) + u' ';
    } else {
        prefix = text.left(l.contentStart);
        if (l.kind == Md::Task) {
            prefix[l.markerStart + 1] = u' ';
        }
        if (!prefix.endsWith(u' ') && !prefix.endsWith(u'\t')) {
            prefix += u' ';
        }
    }
    // The items after this one shift down only if they counted on from it:
    // a `1. 1. 1.` list stays as it is.
    const bool shift = l.kind == Md::Numbered
        && renumber(c, block.next(), info->listStart(), QStringView(text).mid(l.markerStart, int(l.markerLen) - 1).toLongLong() + 1, true);
    c.setPosition(pos);
    c.beginEditBlock();
    c.insertBlock();
    c.insertText(prefix);
    if (shift) {
        const qlonglong next = QStringView(prefix).mid(l.markerStart, prefix.size() - l.markerStart - 2).toLongLong();
        renumber(c, c.block().next(), info->listStart(), next + 1);
    }
    c.endEditBlock();
    return true;
}

// Tab and Shift+Tab in a list item nest it one level deeper or shallower.
bool MarkdownEditor::indent(bool outdent)
{
    if (hasSelection()) {
        return false;
    }
    const QTextBlock block = m_doc->findBlock(cursor());
    const BlockInfo *info = BlockInfo::of(block);
    if (!info || !(info->line.kind == Md::Bullet || info->line.kind == Md::Numbered || info->line.kind == Md::Task)) {
        return false;
    }
    const int start = info->listStart();
    const int width = int(info->line.contentStart) - start;
    QTextCursor c(m_doc);
    if (!outdent) {
        c.setPosition(block.position() + start);
        c.insertText(QString(width, u' '));
        return true;
    }
    const QString text = block.text();
    int n = 0;
    while (n < width && start - 1 - n >= 0 && text.at(start - 1 - n) == u' ') {
        ++n;
    }
    if (n > 0) {
        c.setPosition(block.position() + start - n);
        c.setPosition(block.position() + start, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
    return true;
}

QRectF MarkdownEditor::taskBox(QTextDocument *doc, const QTextBlock &block)
{
    const BlockInfo *info = BlockInfo::of(block);
    QTextLayout *layout = block.layout();
    if (!info || info->line.kind != Md::Task || !layout || layout->lineCount() == 0) {
        return {};
    }
    const int at = int(info->line.markerStart);
    const QTextLine line = layout->lineForTextPosition(at);
    if (!line.isValid()) {
        return {};
    }
    const QPointF origin = doc->documentLayout()->blockBoundingRect(block).topLeft();
    const qreal x1 = line.cursorToX(at);
    const qreal x2 = line.cursorToX(at + 3);
    const qreal size = qMax<qreal>(10, qRound(line.ascent() * 0.9));
    const qreal cx = origin.x() + (x1 + x2) / 2;
    const qreal cy = origin.y() + line.y() + line.height() / 2;
    return {qRound(cx - size / 2) + 0.5, qRound(cy - size / 2) + 0.5, size - 1, size - 1};
}

bool MarkdownEditor::pressTaskBox(const QPointF &point)
{
    const QPointF p = point - textOrigin();
    const int pos = m_doc->documentLayout()->hitTest(p, Qt::FuzzyHit);
    if (pos < 0) {
        return false;
    }
    const QTextBlock block = m_doc->findBlock(pos);
    const QRectF box = taskBox(m_doc, block);
    if (box.isEmpty() || !box.adjusted(-3, -3, 3, 3).contains(p)) {
        return false;
    }
    toggleTask(block);
    return true;
}

void MarkdownEditor::toggleTask(const QTextBlock &block)
{
    const BlockInfo *info = BlockInfo::of(block);
    if (!info || info->line.kind != Md::Task) {
        return;
    }
    QTextCursor c(m_doc);
    c.setPosition(block.position() + int(info->line.markerStart) + 1);
    c.setPosition(c.position() + 1, QTextCursor::KeepAnchor);
    c.insertText(info->line.checked ? QStringLiteral(" ") : QStringLiteral("x"));
}

bool MarkdownEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_edit || !m_doc) {
        return false;
    }
    bool handled = false;
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const auto mods = key->modifiers() & ~Qt::KeypadModifier;
        const bool plain = mods == Qt::NoModifier;
        const bool fm = m_style.formatted;
        switch (key->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
            if (fm && (plain || mods == Qt::ShiftModifier)) {
                handled = moveCaret(key->key() == Qt::Key_Right ? 1 : -1, mods == Qt::ShiftModifier);
            }
            break;
        case Qt::Key_Backspace:
            handled = editable() && fm && plain && !hasSelection() && backspace();
            break;
        case Qt::Key_Delete:
            handled = editable() && fm && plain && !hasSelection() && deleteForward();
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            handled = editable() && plain && newline();
            break;
        case Qt::Key_Tab:
            handled = editable() && plain && indent(false);
            break;
        case Qt::Key_Backtab:
            handled = editable() && indent(true);
            break;
        default:
            break;
        }
    } else if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        handled = editable() && m_style.formatted && mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier
            && pressTaskBox(mouse->position());
    }
    if (handled) {
        event->accept();
    }
    return handled;
}

// Toolbar ---------------------------------------------------------------------

static int runBefore(QTextDocument *doc, int pos, QChar ch)
{
    int n = 0;
    while (pos - n - 1 >= 0 && doc->characterAt(pos - n - 1) == ch) {
        ++n;
    }
    return n;
}

static int runAfter(QTextDocument *doc, int pos, QChar ch)
{
    int n = 0;
    while (pos + n < doc->characterCount() && doc->characterAt(pos + n) == ch) {
        ++n;
    }
    return n;
}

void MarkdownEditor::toggleInline(const QString &marker)
{
    if (!editable() || marker.isEmpty()) {
        return;
    }
    int s = m_edit->property("selectionStart").toInt();
    int e = m_edit->property("selectionEnd").toInt();
    if (s == e) {
        QTextCursor word(m_doc);
        word.setPosition(s);
        word.select(QTextCursor::WordUnderCursor);
        if (word.hasSelection()) {
            s = word.selectionStart();
            e = word.selectionEnd();
        }
    }
    while (s < e && m_doc->characterAt(s).isSpace()) {
        ++s;
    }
    while (e > s && m_doc->characterAt(e - 1).isSpace()) {
        --e;
    }
    const int m = int(marker.size());
    const QChar ch = marker.at(0);
    // Is the text already in this style? `*` and `_` runs hold both italic
    // (an odd count) and bold (two or more): `**b**` isn't italic.
    const int around = qMin(runBefore(m_doc, s, ch), runAfter(m_doc, e, ch));
    const bool single = m == 1 && (ch == u'*' || ch == u'_');
    const bool wrapped = single ? around % 2 == 1 : around >= m;
    QTextCursor c(m_doc);
    c.beginEditBlock();
    if (wrapped && s != e) {
        c.setPosition(e);
        c.setPosition(e + m, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.setPosition(s - m);
        c.setPosition(s, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.endEditBlock();
        select(s - m, e - m);
        return;
    }
    c.setPosition(e);
    c.insertText(marker);
    c.setPosition(s);
    c.insertText(marker);
    c.endEditBlock();
    select(s + m, e + m);
}

static const QRegularExpression &headingPrefix()
{
    static const QRegularExpression re(QStringLiteral("^ {0,3}#{1,6}(?:[ \\t]+|$)"));
    return re;
}

// The last line a selection takes in. One that ends at the start of a line
// (whole lines selected, newline and all) leaves that line out.
static int lastBlock(QTextDocument *doc, int s, int e)
{
    const QTextBlock b = doc->findBlock(e);
    return (e > s && e == b.position() ? b.previous() : b).blockNumber();
}

static bool isListKind(uint8_t kind)
{
    return kind == Md::Bullet || kind == Md::Numbered || kind == Md::Task;
}

// Where a line's own text starts, after any quote marks.
static int afterQuotes(const QTextBlock &block)
{
    const BlockInfo *info = BlockInfo::of(block);
    if (!info || info->quoteMarks.isEmpty()) {
        return 0;
    }
    const int at = info->quoteMarks.last() + 1;
    return at < block.text().size() && block.text().at(at) == u' ' ? at + 1 : at;
}

void MarkdownEditor::setHeading(int level)
{
    if (!editable()) {
        return;
    }
    level = qBound(0, level, 6);
    const int s = m_edit->property("selectionStart").toInt();
    const int e = m_edit->property("selectionEnd").toInt();
    const int last = lastBlock(m_doc, s, e);
    const QString prefix = level > 0 ? QString(level, u'#') + u' ' : QString();
    QTextCursor c(m_doc);
    c.beginEditBlock();
    for (QTextBlock b = m_doc->findBlock(s); b.isValid() && b.blockNumber() <= last; b = b.next()) {
        const BlockInfo *info = BlockInfo::of(b);
        const uint8_t kind = info ? info->line.kind : Md::Blank;
        if (kind == Md::FenceLine || kind == Md::CodeLine || kind == Md::RuleLine) {
            continue;
        }
        if (isListKind(kind)) {
            if (level == 0) {
                continue; // Normal text undoes headings, not lists.
            }
            // The heading takes the list marker's place.
            c.setPosition(b.position() + info->listStart());
            c.setPosition(b.position() + int(info->line.contentStart), QTextCursor::KeepAnchor);
            c.insertText(prefix);
            continue;
        }
        const int at = afterQuotes(b);
        const auto match = headingPrefix().matchView(QStringView(b.text()).mid(at));
        c.setPosition(b.position() + at);
        if (match.hasMatch()) {
            c.setPosition(b.position() + at + int(match.capturedLength()), QTextCursor::KeepAnchor);
        }
        c.insertText(prefix);
    }
    c.endEditBlock();
}

void MarkdownEditor::toggleBlock(const QString &kind)
{
    if (!editable()) {
        return;
    }
    const bool quote = kind == u"quote";
    const uint8_t want = kind == u"numbered" ? Md::Numbered : kind == u"task" ? Md::Task : Md::Bullet;
    const int s = m_edit->property("selectionStart").toInt();
    const int e = m_edit->property("selectionEnd").toInt();
    const QTextBlock first = m_doc->findBlock(s);
    const int last = lastBlock(m_doc, s, e);
    const bool many = first.blockNumber() != last;

    auto has = [&](const QTextBlock &b) {
        const BlockInfo *info = BlockInfo::of(b);
        return info && (quote ? !info->quoteMarks.isEmpty() : info->line.kind == want);
    };
    // Code is left alone (a marker would break a fence), and so are rules.
    auto skip = [&](const QTextBlock &b) {
        const BlockInfo *info = BlockInfo::of(b);
        const uint8_t k = info ? info->line.kind : Md::Blank;
        return (many && b.text().trimmed().isEmpty()) || k == Md::FenceLine || k == Md::CodeLine || (!quote && k == Md::RuleLine);
    };
    bool all = true;
    for (QTextBlock b = first; b.isValid() && b.blockNumber() <= last; b = b.next()) {
        if (!skip(b) && !has(b)) {
            all = false;
            break;
        }
    }

    QTextCursor c(m_doc);
    c.beginEditBlock();
    int number = 1;
    for (QTextBlock b = first; b.isValid() && b.blockNumber() <= last; b = b.next()) {
        if (skip(b)) {
            continue;
        }
        const BlockInfo *info = BlockInfo::of(b);
        const NpLine l = info ? info->line : NpLine{};
        const bool isList = isListKind(l.kind);
        if (quote) {
            if (all) {
                const int q = info->quoteMarks.first();
                const QString text = b.text();
                c.setPosition(b.position() + q);
                c.setPosition(b.position() + q + (q + 1 < text.size() && text.at(q + 1) == u' ' ? 2 : 1), QTextCursor::KeepAnchor);
                c.removeSelectedText();
            } else if (!has(b)) {
                c.setPosition(b.position());
                c.insertText(QStringLiteral("> "));
            }
            continue;
        }
        QString marker;
        if (!all) {
            marker = want == Md::Numbered ? QString::number(number++) + QStringLiteral(". ")
                : want == Md::Task        ? QStringLiteral("- [ ] ")
                                          : QStringLiteral("- ");
        }
        if (isList && info) {
            c.setPosition(b.position() + info->listStart());
            c.setPosition(b.position() + int(l.contentStart), QTextCursor::KeepAnchor);
        } else if (l.kind == Md::HeadingLine) {
            // The list marker takes the heading's place.
            const int at = afterQuotes(b);
            const auto match = headingPrefix().matchView(QStringView(b.text()).mid(at));
            c.setPosition(b.position() + at);
            if (match.hasMatch()) {
                c.setPosition(b.position() + at + int(match.capturedLength()), QTextCursor::KeepAnchor);
            }
        } else {
            c.setPosition(b.position() + (l.kind == Md::Paragraph ? int(l.contentStart) : afterQuotes(b)));
        }
        c.insertText(marker);
    }
    c.endEditBlock();
}

int MarkdownEditor::headingAt(int position) const
{
    if (!m_doc) {
        return 0;
    }
    const BlockInfo *info = BlockInfo::of(m_doc->findBlock(position));
    return info && info->line.kind == Md::HeadingLine ? info->line.heading : 0;
}

int MarkdownEditor::formatsAt(int start, int end) const
{
    if (!m_doc || start < 0) {
        return 0;
    }
    end = qMax(start, end);
    const QTextBlock block = m_doc->findBlock(start);
    if (!block.isValid()) {
        return 0;
    }
    const BlockInfo *info = BlockInfo::of(block);
    if (!info) {
        return 0;
    }
    int bits = 0;
    switch (info->line.kind) {
    case Md::Bullet:
        bits |= FmtBullet;
        break;
    case Md::Numbered:
        bits |= FmtNumbered;
        break;
    case Md::Task:
        bits |= FmtTask;
        break;
    case Md::FenceLine:
    case Md::CodeLine:
    case Md::RuleLine:
        return 0; // no inline formats in code, and a rule has none
    default:
        break;
    }
    if (info->line.quoteDepth > 0) {
        bits |= FmtQuote;
    }
    const bool heading = info->line.kind == Md::HeadingLine;
    // The characters looked at, in the block: the selection clipped to the
    // line, or for a caret the character before it (or after, at the start).
    const int from = start - block.position();
    const int to = qMin(end, block.position() + block.length() - 1) - block.position();
    const bool caret = start == end;
    // The ranges are sorted and don't overlap: jump to the first one that
    // reaches the characters looked at, and stop after the last (a long line
    // has thousands).
    const int lo = caret ? (from > 0 ? from - 1 : from) : from;
    const int hi = caret ? lo + 1 : to;
    const auto formats = block.layout() ? block.layout()->formats() : QList<QTextLayout::FormatRange>();
    auto it = std::partition_point(formats.cbegin(), formats.cend(), [lo](const QTextLayout::FormatRange &r) { return r.start + r.length <= lo; });
    for (; it != formats.cend() && it->start < hi; ++it) {
        const QTextCharFormat &f = it->format;
        // A heading is bold by itself: only its own bold (Strong) counts when
        // it can be told apart. The Syntax view draws the whole heading line
        // bold, so a **strong** word in a heading is not reported there.
        if (f.fontWeight() >= QFont::Bold && (!heading || formatted())) {
            bits |= FmtBold;
        }
        if (f.fontItalic()) {
            bits |= FmtItalic;
        }
        if (f.fontStrikeOut() && info->line.kind != Md::Task) {
            bits |= FmtStrike;
        }
        if (f.fontFamilies().toStringList().contains(m_style.monoFamily)) {
            bits |= FmtCode;
        }
    }
    return bits;
}

QString MarkdownEditor::linkAt(int position) const
{
    if (!m_doc) {
        return {};
    }
    const QTextBlock block = m_doc->findBlock(position);
    const QString text = block.text();
    size_t start = 0;
    size_t end = 0;
    if (!np_md_link_at(reinterpret_cast<const uint16_t *>(text.utf16()), size_t(text.size()), size_t(position - block.position()), &start, &end)) {
        return {};
    }
    QString url = text.mid(qsizetype(start), qsizetype(end - start));
    if (url.size() >= 2 && url.startsWith(u'<') && url.endsWith(u'>')) {
        url = url.mid(1, url.size() - 2); // the address without its <...>
    }
    return url;
}

void MarkdownEditor::clearFormatting()
{
    if (!editable()) {
        return;
    }
    int s = m_edit->property("selectionStart").toInt();
    int e = m_edit->property("selectionEnd").toInt();
    if (s == e) {
        // No selection: the caret's whole line is cleared.
        const QTextBlock line = m_doc->findBlock(s);
        s = line.position();
        e = s + line.text().size();
    }
    // The affected lines come from the selection as it was, before the
    // markers outside it are taken in below.
    const QTextBlock first = m_doc->findBlock(s);
    const int last = lastBlock(m_doc, s, e);
    // Take in the markers right outside the selection: selecting a bold
    // word selects its visible letters only. Not across a line start: whole
    // lines selected end where the next line begins, and its markers stay.
    // Touching hidden ranges are merged (`](u)**b**`), so a link's `[` or
    // `](url)` is taken alone: the next span's markers must stay paired.
    int edge = 0;
    if (hiddenEndingAt(s, &edge)) {
        s = m_doc->characterAt(s - 1) == u'[' ? s - 1 : edge;
    }
    if (e > s && e != m_doc->findBlock(e).position() && hiddenStartingAt(e, &edge)) {
        if (m_doc->characterAt(e) == u']') {
            const QTextBlock b = m_doc->findBlock(e);
            const QString line = b.text();
            size_t start = 0;
            size_t end = 0;
            const size_t at = size_t(e - b.position()) + 2;
            if (np_md_link_at(reinterpret_cast<const uint16_t *>(line.utf16()), size_t(line.size()), at, &start, &end)) {
                const qsizetype close = line.indexOf(u')', qsizetype(end));
                e = close >= 0 ? qMin(edge, b.position() + int(close) + 1) : edge;
            } else {
                e = edge;
            }
        } else {
            e = edge;
        }
    }
    QTextCursor c(m_doc);
    c.beginEditBlock();
    // From the end, so earlier positions stay put.
    for (QTextBlock b = m_doc->findBlockByNumber(last); b.isValid() && b.blockNumber() >= first.blockNumber(); b = b.previous()) {
        const BlockInfo *info = BlockInfo::of(b);
        if (!info) {
            continue;
        }
        const NpLine &l = info->line;
        if (l.kind == Md::FenceLine || l.kind == Md::CodeLine) {
            continue;
        }
        const QString text = b.text();
        const int from = qMax(s - b.position(), int(l.contentStart));
        const int to = qMin(e - b.position(), int(text.size()));
        for (qsizetype i = info->hidden.size() - 1; i >= 0; --i) {
            // Touching ranges are merged (`# **A**`): only the part inside
            // the selection goes, and the line's prefix is handled below.
            const int hs = qMax(info->hidden.at(i).first, from);
            const int he = qMin(info->hidden.at(i).second, to);
            // A backslash escape stays: without it the character would format.
            if (hs >= he || (he - hs == 1 && text.at(hs) == u'\\')) {
                continue;
            }
            c.setPosition(b.position() + hs);
            c.setPosition(b.position() + he, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        // The line's own prefix: heading, list marker, quote.
        const bool prefixed = l.kind == Md::HeadingLine || l.kind == Md::Bullet || l.kind == Md::Numbered || l.kind == Md::Task || l.quoteDepth > 0;
        if (prefixed && l.contentStart > 0) {
            c.setPosition(b.position());
            c.setPosition(b.position() + int(l.contentStart), QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
    }
    c.endEditBlock();
}

void MarkdownEditor::insertLink(const QString &text, const QString &url)
{
    if (!editable() || url.isEmpty()) {
        return;
    }
    // An address with spaces or brackets goes in <...>.
    static const QRegularExpression awkward(QStringLiteral("[\\s()<>]"));
    const QString target = url.contains(awkward) ? u'<' + QString(url).remove(u'<').remove(u'>') + u'>' : url;
    const int s = m_edit->property("selectionStart").toInt();
    const int e = m_edit->property("selectionEnd").toInt();
    QString label = text.isEmpty() ? url : text;
    QTextCursor c(m_doc);
    if (s == e) {
        const QTextBlock block = m_doc->findBlock(s);
        const QString line = block.text();
        size_t start = 0;
        size_t end = 0;
        if (np_md_link_at(reinterpret_cast<const uint16_t *>(line.utf16()), size_t(line.size()), size_t(s - block.position()), &start, &end)) {
            c.setPosition(block.position() + int(start));
            c.setPosition(block.position() + int(end), QTextCursor::KeepAnchor);
            QString replacement = target;
            if (start == 0 || line.at(qsizetype(start) - 1) != u'(') {
                // An autolink, <url> or a bare address, stays a link only
                // while the new address still qualifies; else it gets a label.
                static const QRegularExpression angled(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]{1,31}:[^\\s<>]*$"));
                static const QRegularExpression bare(QStringLiteral("^(?:https?://|www\\.)[^\\s<>]+$"));
                const bool wasAngled = line.at(qsizetype(start)) == u'<';
                if (wasAngled && angled.match(url).hasMatch()) {
                    replacement = u'<' + url + u'>';
                } else if (!wasAngled && bare.match(url).hasMatch()) {
                    replacement = url;
                } else {
                    label.replace(u'[', QStringLiteral("\\[")).replace(u']', QStringLiteral("\\]"));
                    replacement = u'[' + label + QStringLiteral("](") + target + u')';
                }
            }
            c.insertText(replacement);
            m_edit->setProperty("cursorPosition", c.position());
            return;
        }
    }
    label.replace(u'[', QStringLiteral("\\[")).replace(u']', QStringLiteral("\\]"));
    c.setPosition(s);
    c.setPosition(e, QTextCursor::KeepAnchor);
    c.insertText(u'[' + label + QStringLiteral("](") + target + u')');
    m_edit->setProperty("cursorPosition", c.position());
}
