// Formats for each line's Markdown, from what Rust reads in it.
#include "markdown.h"
#include "spellcheck.h"

#include <QElapsedTimer>

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document, bool markdown)
    : QSyntaxHighlighter(document)
    , m_markdown(markdown)
{
    m_passTimer.setSingleShot(true);
    m_passTimer.setInterval(0);
    connect(&m_passTimer, &QTimer::timeout, this, &MarkdownHighlighter::rehighlightSlice);
    // For a document with text, QSyntaxHighlighter queued a rehighlight() of
    // its own, which lays out each formatted line on its own: quadratic. Run
    // it now with highlightBlock muted (no formats: nothing to lay out), so
    // the queued one finds nothing to do; whoever made this calls setStyle,
    // which highlights.
    if (document && !document->isEmpty()) {
        m_muted = true;
        rehighlight();
        m_muted = false;
    }
}

void MarkdownHighlighter::setSpellChecker(SpellChecker *spell)
{
    m_spell = spell;
}

void MarkdownHighlighter::setStyle(const MarkdownStyle &style)
{
    m_style = style;
    m_formats.clear();
    rehighlightAll();
}

// Reads one line: fills `info` (the line, its hidden ranges, its quote
// marks) and m_ranges (the formats), and returns the state for the next line.
int MarkdownHighlighter::read(const QString &text, int previous, BlockInfo *info)
{
    if (!m_markdown) {
        info->hidden.clear();
        m_ranges.clear();
        addMisspelled(text, 0, info);
        return 0;
    }
    const auto *utf16 = reinterpret_cast<const uint16_t *>(text.utf16());
    NpLine line{};
    size_t n = np_md_line(utf16, size_t(text.size()), previous, m_runs.data(), m_runs.size(), &line);
    if (n > m_runs.size()) {
        m_runs.resize(n * 2);
        n = np_md_line(utf16, size_t(text.size()), previous, m_runs.data(), m_runs.size(), &line);
    }

    info->line = line;
    info->hidden.clear();
    info->quoteMarks.clear();
    m_ranges.clear();
    for (size_t i = 0; i < n; ++i) {
        const NpRun &r = m_runs[i];
        const int start = int(r.start);
        const int end = int(r.start + r.len);
        if (r.flags) {
            m_ranges.append({start, int(r.len), format(r.flags, line.heading)});
        }
        if (r.flags & Md::Hidden) {
            if (!info->hidden.isEmpty() && info->hidden.last().second == start) {
                info->hidden.last().second = end;
            } else {
                info->hidden.append({start, end});
            }
        }
        if (line.quoteDepth && (r.flags & Md::Pad)) {
            for (int k = start; k < end; ++k) {
                if (text.at(k) == u'>') {
                    info->quoteMarks.append(k);
                }
            }
        }
    }
    addMisspelled(text, n, info);
    return line.state;
}

void MarkdownHighlighter::highlightBlock(const QString &text)
{
    if (m_muted) {
        return;
    }
    auto *info = static_cast<BlockInfo *>(currentBlockUserData());
    if (!info) {
        info = new BlockInfo;
        setCurrentBlockUserData(info);
    }
    const int state = read(text, qMax(0, previousBlockState()), info);
    for (const QTextLayout::FormatRange &r : std::as_const(m_ranges)) {
        setFormat(r.start, r.length, r.format);
    }
    // A line rehighlightAll's slices haven't reached yet keeps its state:
    // a new one would make Qt go on to the next line, and the next, through
    // the rest of the document in this keystroke. The slices get there.
    if (!m_pass.isNull() && currentBlock().position() >= m_pass.block().position()) {
        return;
    }
    setCurrentBlockState(state);
}

// Notes the line's misspelled words in `info` for SpellUnderlines (Qt Quick
// draws an underline format in the text colour only). Code, links and URLs
// aren't checked; hidden markers inside a word (**bo**ld) don't split it.
// `runs` is how many of m_runs belong to this line.
void MarkdownHighlighter::addMisspelled(const QString &text, size_t runs, BlockInfo *info)
{
    info->misspelled.clear();
    if (!m_spell || !m_spell->isActive()) {
        return;
    }
    QList<SpellChecker::CharClass> classes;
    if (runs) {
        classes.resize(text.size(), SpellChecker::Normal);
        for (size_t i = 0; i < runs; ++i) {
            const NpRun &r = m_runs[i];
            const auto cls = (r.flags & (Md::Code | Md::CodeBlock | Md::Fence | Md::Link)) ? SpellChecker::Break
                : (r.flags & Md::Hidden)                                                 ? SpellChecker::Skip
                                                                                         : SpellChecker::Normal;
            const qsizetype from = qMin<qsizetype>(r.start, text.size());
            const qsizetype to = qMin<qsizetype>(qsizetype(r.start) + r.len, text.size());
            if (cls != SpellChecker::Normal && from < to) {
                std::fill(classes.begin() + from, classes.begin() + to, cls);
            }
        }
    }
    for (const SpellChecker::Word &w : m_spell->misspelled(text, classes)) {
        info->misspelled.append({w.start, w.end});
    }
}

namespace
{
// Up to this many characters, rehighlightAll does it all at once.
constexpr int sliceFrom = 64 * 1024;
// Main-thread time wanted per slice: reading the lines and laying them out.
constexpr qint64 sliceNs = 8'000'000;
} // namespace

int MarkdownHighlighter::readBlock(QTextBlock block, int previous)
{
    auto *info = static_cast<BlockInfo *>(block.userData());
    if (!info) {
        info = new BlockInfo;
        block.setUserData(info);
    }
    const int state = read(block.text(), previous, info);
    block.layout()->setFormats(m_ranges);
    block.setUserState(state);
    return state;
}

// QSyntaxHighlighter::rehighlight() tells the layout about each block on its
// own, and the layout walks the document from the top every time: quadratic,
// 9 s for a 1 MB file. This sets the same formats, user data and states
// straight on the blocks and tells the layout once (once a slice: laying out
// a whole 1 MB file again takes 200 ms).
void MarkdownHighlighter::rehighlightAll(bool now)
{
    m_passTimer.stop();
    m_pass = QTextCursor();
    QTextDocument *doc = document();
    if (!doc) {
        return;
    }
    if (now || doc->characterCount() <= sliceFrom) {
        int state = 0;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
            state = readBlock(b, state);
        }
        doc->markContentsDirty(0, doc->characterCount());
        return;
    }
    m_pass = QTextCursor(doc);
    rehighlightSlice();
}

void MarkdownHighlighter::rehighlightSlice()
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
    QTextBlock b = m_pass.block();
    const int from = b.position();
    int state = qMax(0, b.previous().userState());
    for (int n = 1; b.isValid(); ++n) {
        state = readBlock(b, state);
        b = b.next();
        if (n % 32 == 0 && clock.nsecsElapsed() > m_readBudgetNs) {
            break;
        }
    }
    const qint64 read = qMax<qint64>(clock.nsecsElapsed(), 1);
    const int end = b.isValid() ? b.position() : doc->characterCount();
    doc->markContentsDirty(from, end - from);
    // Laying out costs a few times the reading: give the reading the share of
    // sliceNs it had this time, changing by at most half or double a slice.
    const double share = double(read) / double(qMax<qint64>(clock.nsecsElapsed(), 1));
    m_readBudgetNs = qBound<qint64>(qMax<qint64>(500'000, m_readBudgetNs / 2), qint64(double(sliceNs) * share), 2 * m_readBudgetNs);
    if (b.isValid()) {
        m_pass.setPosition(b.position());
        m_passTimer.start();
    } else {
        m_pass = QTextCursor();
    }
}

const QTextCharFormat &MarkdownHighlighter::format(uint32_t flags, int heading)
{
    const quint64 key = quint64(flags) | (quint64(heading) << 32);
    if (auto it = m_formats.constFind(key); it != m_formats.cend()) {
        return *it;
    }
    const bool formatted = m_style.formatted;
    QTextCharFormat f;
    auto scaled = [&](qreal factor) {
        if (m_style.font.pointSizeF() > 0) {
            f.setFontPointSize(m_style.font.pointSizeF() * factor);
        } else {
            f.setProperty(QTextFormat::FontPixelSize, qRound(m_style.font.pixelSize() * factor));
        }
    };

    if (formatted && (flags & Md::Hidden)) {
        // Takes (almost) no room and isn't drawn. Qt can't lay out a glyph of
        // no width: at 1 px a character is still half a pixel wide (a hidden
        // URL, ten), so its advance is cut to 1 % as well (0 % reads as
        // "not set" in QTextEngine).
        f.setProperty(QTextFormat::FontPixelSize, 1);
        f.setFontLetterSpacingType(QFont::PercentageSpacing);
        f.setFontLetterSpacing(1);
        f.setForeground(Qt::transparent);
        return *m_formats.insert(key, f);
    }
    if (flags & (Md::Code | Md::CodeBlock | Md::Fence)) {
        f.setFontFamilies({m_style.monoFamily});
    }
    if ((flags & Md::Code) && formatted) {
        f.setBackground(m_style.code);
    }
    if (flags & Md::Strong) {
        f.setFontWeight(QFont::Bold);
    }
    if (flags & Md::Emph) {
        f.setFontItalic(true);
    }
    if (flags & Md::Strike) {
        f.setFontStrikeOut(true);
    }
    if (flags & Md::Link) {
        f.setForeground(m_style.link);
        f.setFontUnderline(formatted);
    }
    if (flags & Md::Heading) {
        if (formatted) {
            static constexpr qreal sizes[] = {1, 1.75, 1.45, 1.2, 1.05, 1, 1};
            scaled(sizes[qBound(0, heading, 6)]);
            f.setFontWeight((flags & Md::Strong) ? QFont::Bold : QFont::DemiBold);
        } else {
            f.setFontWeight(QFont::Bold);
        }
    }
    if ((flags & Md::Quote) && formatted) {
        f.setForeground(m_style.dim);
    }
    if ((flags & Md::Done) && formatted) {
        f.setForeground(m_style.dim);
        f.setFontStrikeOut(true);
    }
    if (flags & Md::Fence) {
        f.setForeground(m_style.dim);
        if (formatted) {
            scaled(0.85);
        }
    }
    if (flags & Md::ListNumber) {
        f.setForeground(m_style.dim);
    }
    if (flags & (Md::Pad | Md::Rule)) {
        // Formatted: MarkdownDecorations draws a bullet, box, bar or line there.
        f.setForeground(formatted ? QColor(Qt::transparent) : m_style.dim);
    }
    if ((flags & Md::Pad) && formatted) {
        // "- " is narrow for a bullet and the space after it.
        f.setFontLetterSpacingType(QFont::PercentageSpacing);
        f.setFontLetterSpacing(160);
    }
    if (!formatted && (flags & Md::Hidden)) {
        f.setForeground(m_style.dim);
    }
    return *m_formats.insert(key, f);
}
