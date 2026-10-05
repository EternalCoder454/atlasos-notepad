// Line tools. See linetools.h.
#include "linetools.h"

#include <algorithm>
#include <numeric>
#include <vector>

#include <QCollator>
#include <QLocale>
#include <QQuickTextDocument>
#include <QSet>
#include <QTextCursor>

namespace
{
bool isBlank(QStringView s)
{
    return std::all_of(s.begin(), s.end(), [](QChar c) { return c == u' ' || c == u'\t'; });
}

int leadingLength(QStringView s)
{
    int n = 0;
    while (n < s.size() && (s[n] == u' ' || s[n] == u'\t')) {
        ++n;
    }
    return n;
}

int trailingStart(QStringView s)
{
    int end = int(s.size());
    while (end > 0 && (s[end - 1] == u' ' || s[end - 1] == u'\t')) {
        --end;
    }
    return end;
}

qsizetype joinedSize(const QStringList &lines)
{
    qsizetype n = lines.isEmpty() ? 0 : lines.size() - 1;
    for (const QString &l : lines) {
        n += l.size();
    }
    return n;
}

QString titleCase(const QString &s)
{
    QString out = s.toLower();
    bool start = true;
    for (QChar &c : out) {
        if (c.isLetterOrNumber()) {
            if (start) {
                c = c.toUpper();
            }
            start = false;
        } else if (c != u'\'') {
            start = true;
        }
    }
    return out;
}

// Lower case, then a capital at the start of the text, of each paragraph and
// after ". ", "! " and "? ".
QString sentenceCase(const QString &s)
{
    QString out = s.toLower();
    bool cap = true;
    bool ended = false;
    for (QChar &c : out) {
        if (c.isLetterOrNumber()) {
            if (cap) {
                c = c.toUpper();
            }
            cap = false;
            ended = false;
        } else if (c == u'.' || c == u'!' || c == u'?') {
            ended = true;
        } else if (c == u'\n' || c == QChar::ParagraphSeparator) {
            cap = true;
            ended = false;
        } else if (c.isSpace() && ended) {
            cap = true;
            ended = false;
        }
    }
    return out;
}

QString invertCase(const QString &s)
{
    QString out = s;
    for (QChar &c : out) {
        if (c.isUpper()) {
            c = c.toLower();
        } else if (c.isLower()) {
            c = c.toUpper();
        }
    }
    return out;
}

struct Number {
    bool has = false;
    double value = 0;
};

// The number a line starts with (after blanks): -12, +3.5, 7.
Number leadingNumber(QStringView line)
{
    int i = leadingLength(line);
    const int start = i;
    if (i < line.size() && (line[i] == u'-' || line[i] == u'+')) {
        ++i;
    }
    const int digits = i;
    while (i < line.size() && line[i].isDigit() && line[i].unicode() < 128) {
        ++i;
    }
    if (i == digits) {
        return {};
    }
    if (i + 1 < line.size() && line[i] == u'.' && line[i + 1].isDigit() && line[i + 1].unicode() < 128) {
        ++i;
        while (i < line.size() && line[i].isDigit() && line[i].unicode() < 128) {
            ++i;
        }
    }
    bool ok = false;
    const double v = QLocale::c().toDouble(line.mid(start, i - start), &ok);
    return ok ? Number{true, v} : Number{};
}
} // namespace

LineTools::LineTools(QObject *parent)
    : QObject(parent)
{
}

void LineTools::setTextEdit(QQuickItem *edit)
{
    if (edit == m_edit) {
        return;
    }
    m_edit = edit;
    m_doc = nullptr;
    if (edit) {
        auto *textDocument = qobject_cast<QQuickTextDocument *>(edit->property("textDocument").value<QObject *>());
        m_doc = textDocument ? textDocument->textDocument() : nullptr;
    }
    Q_EMIT textEditChanged();
}

void LineTools::setIndentWidth(int width)
{
    width = qBound(1, width, 16);
    if (width != m_width) {
        m_width = width;
        Q_EMIT indentWidthChanged();
    }
}

bool LineTools::editable() const
{
    return m_doc && m_edit && !m_edit->property("readOnly").toBool();
}

bool LineTools::lineRange(int from, int to, bool whole, QTextBlock *first, QTextBlock *last) const
{
    if (!editable()) {
        return false;
    }
    if (whole && from == to) {
        *first = m_doc->firstBlock();
        *last = m_doc->lastBlock();
        return first->isValid();
    }
    const int end = qMax(0, m_doc->characterCount() - 1);
    from = qBound(0, from, end);
    to = qBound(0, to, end);
    if (to < from) {
        std::swap(from, to);
    }
    *first = m_doc->findBlock(from);
    *last = to > from ? m_doc->findBlock(to - 1) : *first;
    return first->isValid() && last->isValid();
}

QStringList LineTools::textOf(const QTextBlock &first, const QTextBlock &last) const
{
    QStringList lines;
    lines.reserve(last.blockNumber() - first.blockNumber() + 1);
    for (QTextBlock b = first; b.isValid() && b.blockNumber() <= last.blockNumber(); b = b.next()) {
        lines << b.text();
    }
    return lines;
}

bool LineTools::applyLines(const QTextBlock &first, const QStringList &oldLines, const QStringList &newLines)
{
    const qsizetype n = oldLines.size();
    const qsizetype m = newLines.size();
    qsizetype p = 0;
    while (p < n && p < m && oldLines[p] == newLines[p]) {
        ++p;
    }
    qsizetype s = 0;
    while (s < n - p && s < m - p && oldLines[n - 1 - s] == newLines[m - 1 - s]) {
        ++s;
    }
    const qsizetype a = n - s; // old lines [p, a) become new lines [p, b)
    const qsizetype b = m - s;
    if (a == p && b == p) {
        return false;
    }
    const int firstNo = first.blockNumber();
    auto block = [&](qsizetype k) { return m_doc->findBlockByNumber(firstNo + int(k)); };
    auto start = [&](qsizetype k) { return block(k).position(); };
    auto end = [&](qsizetype k) {
        const QTextBlock bl = block(k);
        return bl.position() + bl.length() - 1;
    };
    QString text;
    if (b > p) {
        text = QStringList(newLines.mid(p, b - p)).join(u'\n');
    }

    QTextCursor c(m_doc);
    c.beginEditBlock();
    if (a > p && b > p) {
        c.setPosition(start(p));
        c.setPosition(end(a - 1), QTextCursor::KeepAnchor);
        c.insertText(text);
    } else if (a == p) { // only inserts
        if (block(p).isValid()) {
            c.setPosition(start(p));
            c.insertText(text + u'\n');
        } else {
            c.setPosition(end(p - 1));
            c.insertText(u'\n' + text);
        }
    } else if (block(a).isValid()) { // only deletes, with a line after
        c.setPosition(start(p));
        c.setPosition(start(a), QTextCursor::KeepAnchor);
        c.removeSelectedText();
    } else { // only deletes, through the last line
        const QTextBlock before = block(p).previous(); // its newline goes too
        c.setPosition(before.isValid() ? before.position() + before.length() - 1 : start(p));
        c.setPosition(end(a - 1), QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
    c.endEditBlock();
    return true;
}

void LineTools::select(int start, int end)
{
    if (!m_edit || !m_doc) {
        return;
    }
    const int len = qMax(0, m_doc->characterCount() - 1);
    start = qBound(0, start, len);
    end = qBound(0, end, len);
    if (start == end) {
        m_edit->setProperty("cursorPosition", start);
    } else {
        QMetaObject::invokeMethod(m_edit, "select", Q_ARG(int, start), Q_ARG(int, end));
    }
}

template<typename F>
void LineTools::transform(int from, int to, bool whole, F &&fn)
{
    QTextBlock first, last;
    if (!lineRange(from, to, whole, &first, &last)) {
        return;
    }
    const QStringList old = textOf(first, last);
    QStringList lines = old;
    fn(lines);
    const int start = first.position(); // read before the edit invalidates the block
    // A caret stays on its line and column when the line count holds.
    const QTextBlock caretBlock = m_doc->findBlock(m_edit->property("cursorPosition").toInt());
    const int caretLine = caretBlock.blockNumber() - first.blockNumber();
    const int caretColumn = m_edit->property("cursorPosition").toInt() - caretBlock.position();
    const int firstNo = first.blockNumber();
    if (!applyLines(first, old, lines)) {
        return;
    }
    if (from != to) { // keep the selection over the lines left
        select(start, start + int(joinedSize(lines)));
    } else if (lines.size() == old.size() && caretLine >= 0 && caretLine < lines.size()) {
        const QTextBlock b = m_doc->findBlockByNumber(firstNo + caretLine);
        const int at = b.position() + qMin(caretColumn, int(lines[caretLine].size()));
        select(at, at);
    }
}

void LineTools::duplicateLines(int from, int to)
{
    QTextBlock first, last;
    if (!lineRange(from, to, false, &first, &last)) {
        return;
    }
    const QStringList old = textOf(first, last);
    QStringList lines = old;
    lines += old;
    const int shift = int(joinedSize(old)) + 1;
    if (applyLines(first, old, lines)) {
        select(qMin(from, to) + shift, qMax(from, to) + shift);
    }
}

void LineTools::moveLines(int from, int to, bool down)
{
    QTextBlock first, last;
    if (!lineRange(from, to, false, &first, &last)) {
        return;
    }
    const QTextBlock other = down ? last.next() : first.previous();
    if (!other.isValid()) {
        return;
    }
    const int shift = int(other.text().size()) + 1;
    const QStringList moving = textOf(first, last);
    const QStringList across = {other.text()};
    const QTextBlock top = down ? first : other;
    const QStringList old = down ? moving + across : across + moving;
    const QStringList lines = down ? across + moving : moving + across;
    if (applyLines(top, old, lines)) {
        const int d = down ? shift : -shift;
        select(qMin(from, to) + d, qMax(from, to) + d);
    }
}

void LineTools::deleteLines(int from, int to)
{
    QTextBlock first, last;
    if (!lineRange(from, to, false, &first, &last)) {
        return;
    }
    const int firstNo = first.blockNumber();
    if (applyLines(first, textOf(first, last), {})) {
        const QTextBlock b = m_doc->findBlockByNumber(qMin(firstNo, m_doc->blockCount() - 1));
        select(b.position(), b.position());
    }
}

void LineTools::joinLines(int from, int to)
{
    QTextBlock first, last;
    if (!lineRange(from, to, false, &first, &last)) {
        return;
    }
    if (first == last) { // the caret's line and the next
        last = first.next();
        if (!last.isValid()) {
            return;
        }
    }
    const QStringList old = textOf(first, last);
    QString out = old.first();
    int point = -1; // where the caret goes: the first join
    for (qsizetype i = 1; i < old.size(); ++i) {
        const QStringView rest = QStringView(old[i]).mid(leadingLength(old[i]));
        if (rest.isEmpty()) {
            continue;
        }
        out.truncate(trailingStart(out));
        if (point < 0) {
            point = int(out.size());
        }
        if (!out.isEmpty()) {
            out += u' ';
        }
        out += rest;
    }
    point = qMax(point, 0);
    const int start = first.position();
    if (applyLines(first, old, {out})) {
        if (from != to) {
            select(start, start + int(out.size()));
        } else {
            select(start + point, start + point);
        }
    }
}

void LineTools::sortLines(int from, int to, int flags)
{
    transform(from, to, true, [flags](QStringList &lines) {
        const qsizetype n = lines.size();
        QCollator collator;
        collator.setCaseSensitivity(flags & CaseInsensitive ? Qt::CaseInsensitive : Qt::CaseSensitive);
        // Sort keys are worked out once per line, not once per comparison.
        std::vector<QCollatorSortKey> keys;
        keys.reserve(n);
        for (const QString &l : std::as_const(lines)) {
            keys.push_back(collator.sortKey(l));
        }
        std::vector<Number> numbers;
        if (flags & Numeric) {
            numbers.reserve(n);
            for (const QString &l : std::as_const(lines)) {
                numbers.push_back(leadingNumber(l));
            }
        }
        std::vector<qsizetype> order(n);
        std::iota(order.begin(), order.end(), 0);
        // Lines with no number first, then by value; ties by text.
        auto less = [&](qsizetype x, qsizetype y) {
            if (flags & Numeric) {
                const Number &a = numbers[x];
                const Number &b = numbers[y];
                if (a.has != b.has) {
                    return b.has;
                }
                if (a.has && a.value != b.value) {
                    return a.value < b.value;
                }
            }
            return keys[x].compare(keys[y]) < 0;
        };
        if (flags & Descending) {
            std::stable_sort(order.begin(), order.end(), [&](qsizetype x, qsizetype y) { return less(y, x); });
        } else {
            std::stable_sort(order.begin(), order.end(), less);
        }
        QStringList sorted;
        sorted.reserve(n);
        for (qsizetype i : order) {
            sorted.push_back(std::move(lines[i]));
        }
        lines = std::move(sorted);
    });
}

void LineTools::reverseLines(int from, int to)
{
    transform(from, to, true, [](QStringList &lines) { std::reverse(lines.begin(), lines.end()); });
}

void LineTools::removeDuplicateLines(int from, int to)
{
    transform(from, to, true, [](QStringList &lines) {
        QSet<QString> seen;
        seen.reserve(lines.size());
        QStringList kept;
        for (QString &l : lines) {
            if (!seen.contains(l)) {
                seen.insert(l);
                kept.push_back(std::move(l));
            }
        }
        lines = std::move(kept);
    });
}

void LineTools::removeEmptyLines(int from, int to)
{
    transform(from, to, true, [](QStringList &lines) {
        QStringList kept;
        for (QString &l : lines) {
            if (!isBlank(l)) {
                kept.push_back(std::move(l));
            }
        }
        lines = std::move(kept);
    });
}

void LineTools::trimSpaces(int from, int to, int mode)
{
    if (mode < TrimTrailing || mode > TrimBoth) {
        return;
    }
    transform(from, to, true, [mode](QStringList &lines) {
        for (QString &l : lines) {
            const int end = mode == TrimLeading ? int(l.size()) : trailingStart(l);
            const int start = mode == TrimTrailing ? 0 : qMin(leadingLength(l), end);
            if (start > 0 || end < l.size()) {
                l = l.mid(start, end - start);
            }
        }
    });
}

void LineTools::tabsToSpaces(int from, int to)
{
    const int width = m_width;
    transform(from, to, true, [width](QStringList &lines) {
        for (QString &l : lines) {
            if (!l.contains(u'\t')) {
                continue;
            }
            QString out;
            out.reserve(l.size() + 8);
            for (QChar c : std::as_const(l)) {
                if (c == u'\t') {
                    out.append(QString(width - out.size() % width, u' '));
                } else {
                    out.append(c);
                }
            }
            l = out;
        }
    });
}

void LineTools::spacesToLeadingTabs(int from, int to)
{
    const int width = m_width;
    transform(from, to, true, [width](QStringList &lines) {
        for (QString &l : lines) {
            const int lead = leadingLength(l);
            if (lead == 0) {
                continue;
            }
            int col = 0;
            for (int i = 0; i < lead; ++i) {
                col = l[i] == u'\t' ? col + width - col % width : col + 1;
            }
            const QString indent = QString(col / width, u'\t') + QString(col % width, u' ');
            if (QStringView(l).left(lead) != indent) {
                l = indent + QStringView(l).mid(lead);
            }
        }
    });
}

QString LineTools::convertCase(const QString &text, int mode)
{
    switch (mode) {
    case UpperCase:
        return text.toUpper();
    case LowerCase:
        return text.toLower();
    case TitleCase:
        return titleCase(text);
    case SentenceCase:
        return sentenceCase(text);
    case InvertCase:
        return invertCase(text);
    default:
        return text;
    }
}

void LineTools::changeCase(int from, int to, int mode)
{
    if (!editable() || mode < UpperCase || mode > InvertCase) {
        return;
    }
    const int end = qMax(0, m_doc->characterCount() - 1);
    from = qBound(0, from, end);
    to = qBound(0, to, end);
    QTextCursor c(m_doc);
    c.setPosition(from);
    if (from == to) {
        c.select(QTextCursor::WordUnderCursor);
    } else {
        c.setPosition(qMin(from, to));
        c.setPosition(qMax(from, to), QTextCursor::KeepAnchor);
    }
    const QString text = c.selectedText(); // paragraph breaks come back as U+2029
    if (text.isEmpty()) {
        return;
    }
    const QString out = convertCase(text, mode);
    if (out != text) {
        c.insertText(out);
    }
}
