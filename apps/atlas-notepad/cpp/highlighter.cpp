// Formats for each line's Markdown, from what Rust reads in it.
#include "markdown.h"
#include "spellcheck.h"

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document, bool markdown)
    : QSyntaxHighlighter(document)
    , m_markdown(markdown)
{
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
    auto *info = static_cast<BlockInfo *>(currentBlockUserData());
    if (!info) {
        info = new BlockInfo;
        setCurrentBlockUserData(info);
    }
    const int state = read(text, qMax(0, previousBlockState()), info);
    for (const QTextLayout::FormatRange &r : std::as_const(m_ranges)) {
        setFormat(r.start, r.length, r.format);
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

// QSyntaxHighlighter::rehighlight() tells the layout about each block on its
// own, and the layout walks the document from the top every time: quadratic,
// 9 s for a 1 MB file. This sets the same formats, user data and states
// straight on the blocks and tells the layout once.
void MarkdownHighlighter::rehighlightAll()
{
    QTextDocument *doc = document();
    if (!doc) {
        return;
    }
    int state = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        auto *info = static_cast<BlockInfo *>(b.userData());
        if (!info) {
            info = new BlockInfo;
            b.setUserData(info);
        }
        state = read(b.text(), state, info);
        b.layout()->setFormats(m_ranges);
        b.setUserState(state);
    }
    doc->markContentsDirty(0, doc->characterCount());
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
