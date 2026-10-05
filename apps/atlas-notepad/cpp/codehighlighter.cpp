// See codehighlighter.h.
#include "codehighlighter.h"
#include "sizelimits.h"

#include <QElapsedTimer>
#include <QTextDocument>

namespace
{
using KSyntaxHighlighting::State;

// Up to this many characters, a pass does it all at once.
constexpr int sliceFrom = 64 * 1024;
// Main-thread time wanted per slice: reading the lines and laying them out.
constexpr qint64 sliceNs = 8'000'000;
// Main-thread time an edit may spend passing a state change on, before the
// slices take over.
constexpr qint64 editNs = 2'000'000;
// A bigger insert is left to a pass that is already due before it (a file
// being filled in pieces).
constexpr int bigInsert = 2048;
// After such an insert, the pass waits this long for the next one: a file
// filled in pieces is read first (its slices between the pieces made the
// filling slower), and the pass then goes over it once.
constexpr int settleMs = 50;

// Ours only: a block may still carry the Markdown highlighter's data.
CodeBlockData *dataOf(const QTextBlock &block)
{
    return dynamic_cast<CodeBlockData *>(block.userData());
}

// Where position `p` is after `removed` characters at `from` became `added`:
// a position inside the removed text goes to the end of the added text when
// `toEnd`, else to its start.
int shifted(int p, int from, int removed, int added, bool toEnd)
{
    if (p <= from) {
        return p;
    }
    if (p >= from + removed) {
        return p + added - removed;
    }
    return toEnd ? from + added : from;
}
} // namespace

CodeHighlighter::CodeHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    m_passTimer.setSingleShot(true);
    m_passTimer.setInterval(0);
    connect(&m_passTimer, &QTimer::timeout, this, &CodeHighlighter::runPass);
    if (!document) {
        return;
    }
    // After QSyntaxHighlighter's own slot (made by its constructor), which
    // finds nothing to do for the blocks it reformats.
    connect(document, &QTextDocument::contentsChange, this, &CodeHighlighter::documentChanged);
    // For a document with text, QSyntaxHighlighter queued a rehighlight() of
    // its own, which would clear what is highlighted by then. Run it now, with
    // no formats to clear (highlightBlock does nothing): the queued one finds
    // it done.
    if (!document->isEmpty()) {
        rehighlight();
    }
}

CodeHighlighter::~CodeHighlighter()
{
    m_passTimer.stop();
    if (QTextDocument *doc = document()) {
        // QSyntaxHighlighter's destructor (it clears the formats) edits the
        // document once our half is gone: don't be told.
        disconnect(doc, &QTextDocument::contentsChange, this, &CodeHighlighter::documentChanged);
        // Our user data must not outlive us in a document another highlighter
        // (or the Markdown one) will read; theirs is left alone.
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            if (dataOf(b)) {
                b.setUserData(nullptr);
            }
        }
    }
}

void CodeHighlighter::highlightBlock(const QString &)
{
}

void CodeHighlighter::setDefinition(const KSyntaxHighlighting::Definition &def)
{
    AbstractHighlighter::setDefinition(def);
    m_styles.clear();
    rehighlightAll();
}

void CodeHighlighter::setTheme(const KSyntaxHighlighting::Theme &theme)
{
    AbstractHighlighter::setTheme(theme);
    m_styles.clear();
    rehighlightAll();
}

// The same mapping as KSyntaxHighlighting::SyntaxHighlighter::applyFormat.
const CodeHighlighter::Style &CodeHighlighter::style(const KSyntaxHighlighting::Format &format)
{
    const int id = format.id();
    if (auto it = m_styles.constFind(id); it != m_styles.cend()) {
        return *it;
    }
    Style s;
    const KSyntaxHighlighting::Theme t = theme();
    // (KSyntaxHighlighting 6.30's own highlighter styles every token, normal
    // text too, and always with the text colour.)
    s.format.setForeground(format.textColor(t));
    if (format.hasBackgroundColor(t)) {
        s.format.setBackground(format.backgroundColor(t));
    }
    if (format.isBold(t)) {
        s.format.setFontWeight(QFont::Bold);
    }
    if (format.isItalic(t)) {
        s.format.setFontItalic(true);
    }
    if (format.isUnderline(t)) {
        s.format.setFontUnderline(true);
    }
    if (format.isStrikeThrough(t)) {
        s.format.setFontStrikeOut(true);
    }
    s.plain = s.format == QTextCharFormat();
    return *m_styles.insert(id, s);
}

void CodeHighlighter::applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format)
{
    if (length <= 0) {
        return;
    }
    const Style &s = style(format);
    if (s.plain) {
        return;
    }
    // Neighbours with the same look are one range, as QSyntaxHighlighter has them.
    if (!m_ranges.isEmpty()) {
        QTextLayout::FormatRange &last = m_ranges.last();
        if (last.start + last.length == offset && last.format == s.format) {
            last.length += length;
            return;
        }
    }
    QTextLayout::FormatRange r;
    r.start = offset;
    r.length = length;
    r.format = s.format;
    m_ranges.append(r);
}

// Highlights one block from the state before it; returns the state after it.
// Formats and states are set on the block, nothing is told to the layout.
State CodeHighlighter::highlightOne(QTextBlock block, const State &in, bool *changed)
{
    auto *data = dataOf(block);
    if (!data) {
        data = new CodeBlockData;
        block.setUserData(data);
    }
    m_ranges.clear();
    State end = in;
    // A line this long is left unhighlighted: reading it costs a stall, and
    // the file is read-only anyway (Limits::lineLength).
    if (block.length() - 1 <= Limits::lineLength) {
        end = highlightLine(block.text(), in);
    }
    data->in = in;
    data->end = end;
    QTextLayout *layout = block.layout();
    const QList<QTextLayout::FormatRange> old = layout->formats();
    // While the input method composes text in the line, its formats (the
    // underline) stay and the line's are moved past it: what
    // QSyntaxHighlighter does.
    if (const int preeditLength = layout->preeditAreaText().size(); preeditLength > 0) {
        const int preedit = layout->preeditAreaPosition();
        QList<QTextLayout::FormatRange> ranges;
        for (const QTextLayout::FormatRange &r : old) {
            if (r.start >= preedit && r.start + r.length <= preedit + preeditLength) {
                ranges.append(r);
            }
        }
        for (QTextLayout::FormatRange r : std::as_const(m_ranges)) {
            if (r.start >= preedit) {
                r.start += preeditLength;
            } else if (r.start + r.length >= preedit) {
                r.length += preeditLength;
            }
            ranges.append(r);
        }
        m_ranges = std::move(ranges);
    }
    // Plain before and after: the layout has nothing to learn (a 5 MB line
    // would be laid out again for it).
    *changed = !m_ranges.isEmpty() || !old.isEmpty();
    if (*changed) {
        layout->setFormats(m_ranges);
    }
    return end;
}

// Highlights `block` and the blocks after it. It stops at the end of the
// document; when `budgetNs` (0: none) has passed, returning the next block
// to do; and, unless `all`, once a block after `through` (a position) would
// start in the state it was highlighted in, returning an invalid block (the
// rest is as it was). `end` is where the highlighted text ends.
QTextBlock CodeHighlighter::run(QTextBlock block, bool all, int through, qint64 budgetNs, int *from, int *end)
{
    QElapsedTimer clock;
    clock.start();
    // From the last line with a known state: one without our data (another
    // highlighter's, or never done) would be a guess.
    QTextBlock before = block.previous();
    while (before.isValid() && !dataOf(before)) {
        block = before;
        before = block.previous();
    }
    State state;
    if (before.isValid()) {
        if (const auto *data = dataOf(before)) {
            state = data->end;
        }
    }
    *from = -1;
    *end = block.position();
    for (int n = 1; block.isValid(); ++n) {
        bool changed = false;
        state = highlightOne(block, state, &changed);
        if (changed) {
            if (*from < 0) {
                *from = block.position();
            }
            *end = block.position() + block.length();
        }
        block = block.next();
        if (!all && block.isValid() && block.position() > through) {
            const auto *data = dataOf(block);
            if (data && data->in == state) {
                return QTextBlock();
            }
        }
        if (budgetNs > 0 && n % 16 == 0 && clock.nsecsElapsed() > budgetNs) {
            break;
        }
    }
    return block;
}

// An edit: its lines are highlighted at once; if the state after them is not
// what the next line was highlighted in, that is passed on for a moment, then
// by the slices. Also what reads a file filled in pieces, and a text set.
void CodeHighlighter::documentChanged(int from, int removed, int added)
{
    QTextDocument *doc = document();
    if (!doc || !definition().isValid()) {
        return;
    }
    // Formats set by slices but not yet told of were at the old positions.
    if (m_dirtyFrom >= 0) {
        m_dirtyFrom = shifted(m_dirtyFrom, from, removed, added, false);
        m_dirtyEnd = shifted(m_dirtyEnd, from, removed, added, true);
    }
    const int last = qBound(0, from + added, qMax(0, doc->characterCount() - 1));
    const QTextBlock first = doc->findBlock(qBound(0, from, last));
    if (!first.isValid()) {
        return;
    }
    if (!m_pass.isNull() && m_pass.document() == doc && first.position() >= m_pass.block().position()) {
        // A big insert after a pass that is due is left to it; so is an edit
        // after lines that pass hasn't done yet (one left to it before: the
        // state before the edit isn't known). The pass then goes to the end,
        // or it could stop before getting there.
        const QTextBlock before = first.previous();
        if (added > bigInsert || (before.isValid() && !dataOf(before))) {
            m_all = true;
            m_passTimer.start(settleMs);
            return;
        }
    }
    const int through = doc->findBlock(last).position();
    flushDirty(true);
    int dirty = 0, end = 0;
    const QTextBlock next = run(first, false, through, editNs, &dirty, &end);
    if (dirty >= 0) {
        doc->markContentsDirty(dirty, end - dirty);
    }
    if (next.isValid()) {
        // Lines of the edit not done yet: everything after them is stale.
        schedule(next, next.position() <= through);
    }
}

// Tells the layout about the formats set by slices. The layout walks the
// rest of the document after the range it is told of, so on a big document
// every slice telling it costs more than the slice: it is told at most once
// in ten times what the last telling took (at least 100 ms), and at the end.
void CodeHighlighter::flushDirty(bool force)
{
    QTextDocument *doc = document();
    if (m_dirtyFrom < 0 || !doc) {
        return;
    }
    if (!force && m_flushClock.isValid() && m_flushClock.nsecsElapsed() < qMax<qint64>(100'000'000, 10 * m_flushCostNs)) {
        return;
    }
    QElapsedTimer cost;
    cost.start();
    const int from = qMin(m_dirtyFrom, doc->characterCount());
    doc->markContentsDirty(from, qMax(0, qMin(m_dirtyEnd, doc->characterCount()) - from));
    m_flushCostNs = cost.nsecsElapsed();
    m_flushClock.restart();
    m_dirtyFrom = -1;
    m_dirtyEnd = 0;
}

// Notes that the lines from `block` on are to be done by slices.
void CodeHighlighter::schedule(const QTextBlock &block, bool all)
{
    QTextDocument *doc = document();
    if (!doc || !block.isValid()) {
        return;
    }
    if (m_pass.isNull() || m_pass.document() != doc) {
        m_pass = QTextCursor(doc);
        m_pass.setPosition(block.position());
        m_all = all;
    } else {
        const int due = m_pass.block().position();
        // Two stretches that stop where they match can't be one, unless
        // they go to the end: one at a time would lose the second.
        m_all = m_all || all || block.position() > due;
        if (block.position() < due) {
            m_pass.setPosition(block.position());
        }
    }
    if (!m_passTimer.isActive()) {
        m_passTimer.start(0);
    }
}

void CodeHighlighter::rehighlightAll(bool now)
{
    m_passTimer.stop();
    m_pass = QTextCursor();
    m_all = false;
    flushDirty(true);
    QTextDocument *doc = document();
    if (!doc || !definition().isValid()) {
        return;
    }
    if (now || doc->characterCount() <= sliceFrom) {
        int dirty = 0, end = 0;
        run(doc->begin(), true, 0, 0, &dirty, &end);
        if (dirty >= 0) {
            doc->markContentsDirty(dirty, end - dirty);
        }
        return;
    }
    m_pass = QTextCursor(doc);
    m_all = true;
    runPass();
}

void CodeHighlighter::runPass()
{
    QTextDocument *doc = document();
    if (!doc || m_pass.isNull() || m_pass.document() != doc) {
        m_pass = QTextCursor();
        return;
    }
    QElapsedTimer clock;
    clock.start();
    // The cursor moved with any edits since the last slice; the line before
    // it has its state (typing highlights as it goes).
    const QTextBlock first = m_pass.block();
    int dirty = 0, end = 0;
    const QTextBlock next = run(first, m_all, first.position(), m_readBudgetNs, &dirty, &end);
    const qint64 read = qMax<qint64>(clock.nsecsElapsed(), 1);
    if (dirty >= 0) {
        m_dirtyFrom = m_dirtyFrom < 0 ? dirty : qMin(m_dirtyFrom, dirty);
        m_dirtyEnd = qMax(m_dirtyEnd, end);
    }
    flushDirty(!next.isValid());
    // Laying out costs a few times the reading: give the reading the share of
    // sliceNs it had this time, changing by at most half or double a slice.
    const double share = double(read) / double(qMax<qint64>(clock.nsecsElapsed(), 1));
    m_readBudgetNs = qBound<qint64>(qMax<qint64>(500'000, m_readBudgetNs / 2), qint64(double(sliceNs) * share), 2 * m_readBudgetNs);
    if (next.isValid()) {
        m_pass.setPosition(next.position());
        m_passTimer.start(0);
    } else {
        m_pass = QTextCursor();
        m_all = false;
    }
}
