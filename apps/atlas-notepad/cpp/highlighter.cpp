// Formats for each line's Markdown, from what Rust reads in it.
#include "markdown.h"

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
}

void MarkdownHighlighter::setStyle(const MarkdownStyle &style)
{
    m_style = style;
    m_formats.clear();
    rehighlight();
}

void MarkdownHighlighter::highlightBlock(const QString &text)
{
    const int previous = qMax(0, previousBlockState());
    const auto *utf16 = reinterpret_cast<const uint16_t *>(text.utf16());
    NpLine line{};
    size_t n = np_md_line(utf16, size_t(text.size()), previous, m_runs.data(), m_runs.size(), &line);
    if (n > m_runs.size()) {
        m_runs.resize(n * 2);
        n = np_md_line(utf16, size_t(text.size()), previous, m_runs.data(), m_runs.size(), &line);
    }

    auto *info = static_cast<BlockInfo *>(currentBlockUserData());
    if (!info) {
        info = new BlockInfo;
        setCurrentBlockUserData(info);
    }
    info->line = line;
    info->hidden.clear();
    info->quoteMarks.clear();
    for (size_t i = 0; i < n; ++i) {
        const NpRun &r = m_runs[i];
        const int start = int(r.start);
        const int end = int(r.start + r.len);
        setFormat(start, int(r.len), format(r.flags, line.heading));
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
    setCurrentBlockState(line.state);
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
