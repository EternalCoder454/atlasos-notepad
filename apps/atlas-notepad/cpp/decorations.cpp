// Bullets, checkboxes, quote bars, rules and code block backgrounds for the
// visible lines. The shapes are worked out before each frame (updatePolish)
// and the item only repaints when they changed: typing inside a paragraph
// moves none of them.
#include "markdown.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

namespace
{
QFont labelFont(const QFont &base)
{
    QFont f = base;
    if (f.pointSizeF() > 0) {
        f.setPointSizeF(f.pointSizeF() * 0.78);
    } else if (f.pixelSize() > 0) {
        f.setPixelSize(qMax(1, qRound(f.pixelSize() * 0.78)));
    }
    return f;
}
} // namespace

QString MarkdownDecorations::fenceLabel(const QString &line)
{
    constexpr qsizetype maxScan = 256;
    constexpr qsizetype maxChars = 64;
    qsizetype i = 0;
    while (i < line.size() && line.at(i) != u'`' && line.at(i) != u'~') {
        ++i;
    }
    if (i == line.size()) {
        return {};
    }
    const QChar fence = line.at(i);
    while (i < line.size() && line.at(i) == fence) {
        ++i;
    }
    while (i < line.size() && line.at(i).isSpace()) {
        ++i;
    }
    qsizetype j = i;
    while (j < line.size() && j - i < maxScan && !line.at(j).isSpace()) {
        ++j;
    }
    // Only what can be seen: no controls, no bidi or other format
    // characters, no separators, no unpaired surrogates (they come out as
    // U+FFFD), private use or unassigned code points.
    QList<char32_t> kept;
    for (const char32_t c : line.mid(i, j - i).toUcs4()) {
        if (c == 0xFFFD) {
            continue;
        }
        switch (QChar::category(c)) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Other_PrivateUse:
        case QChar::Other_NotAssigned:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
        case QChar::Separator_Space:
            continue;
        default:
            break;
        }
        if (kept.size() < maxChars) {
            kept.append(c);
        }
    }
    return QString::fromUcs4(kept.constData(), kept.size());
}

MarkdownDecorations::MarkdownDecorations(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

void MarkdownDecorations::setEditor(MarkdownEditor *editor)
{
    if (editor == m_editor) {
        return;
    }
    if (m_editor) {
        disconnect(m_editor, nullptr, this, nullptr);
    }
    m_editor = editor;
    if (editor) {
        connect(editor, &MarkdownEditor::textEditChanged, this, &MarkdownDecorations::watch);
        connect(editor, &MarkdownEditor::formattedChanged, this, [this] {
            polish();
        });
        connect(editor, &MarkdownEditor::styleChanged, this, [this] {
            update();
            polish();
        });
    }
    watch();
    Q_EMIT editorChanged();
}

void MarkdownDecorations::watch()
{
    if (m_doc) {
        disconnect(m_doc->documentLayout(), nullptr, this, nullptr);
    }
    m_doc = m_editor ? m_editor->document() : nullptr;
    if (m_doc) {
        auto *layout = m_doc->documentLayout();
        connect(layout, &QAbstractTextDocumentLayout::update, this, [this] {
            polish();
        });
        connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] {
            polish();
        });
    }
    polish();
}

void MarkdownDecorations::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    polish();
}

void MarkdownDecorations::updatePolish()
{
    layOut(m_scratch);
    if (m_scratch != m_shapes) {
        m_shapes.swap(m_scratch);
        update();
    }
}

static bool isCode(const QTextBlock &block)
{
    const BlockInfo *info = block.isValid() ? BlockInfo::of(block) : nullptr;
    return info && (info->line.kind == Md::CodeLine || info->line.kind == Md::FenceLine);
}

void MarkdownDecorations::layOut(std::vector<Shape> &out) const
{
    out.clear();
    if (!m_editor || !m_doc || !m_editor->formatted() || !m_editor->textEdit()) {
        return;
    }
    auto *layout = m_doc->documentLayout();
    const QPointF origin = m_editor->textOrigin();
    // This item covers the view; in document coordinates:
    const qreal top = y() - origin.y();
    const qreal bottom = top + height();
    const qreal right = m_doc->textWidth() > 0 ? m_doc->textWidth() : m_editor->textEdit()->width() - 2 * origin.x();
    const qreal xHeight = QFontMetricsF(m_editor->font()).xHeight();
    const int first = layout->hitTest(QPointF(0, qMax<qreal>(0, top)), Qt::FuzzyHit);

    for (QTextBlock block = m_doc->findBlock(qMax(0, first)); block.isValid(); block = block.next()) {
        const QRectF br = layout->blockBoundingRect(block);
        if (br.top() > bottom) {
            break;
        }
        const BlockInfo *info = BlockInfo::of(block);
        QTextLayout *tl = block.layout();
        if (!info || !tl || tl->lineCount() == 0) {
            continue;
        }
        const NpLine &l = info->line;
        for (const int q : info->quoteMarks) {
            const QTextLine line = tl->lineForTextPosition(q);
            const qreal x = br.left() + line.cursorToX(q);
            out.push_back({Shape::Quote, 0, QRectF(qRound(x) + 1, br.top(), 3, br.height())});
        }
        switch (l.kind) {
        case Md::Bullet: {
            const int at = int(l.markerStart);
            const QTextLine line = tl->lineForTextPosition(at);
            const qreal cx = br.left() + (line.cursorToX(at) + line.cursorToX(at + 1)) / 2;
            const qreal cy = br.top() + line.y() + line.ascent() - xHeight / 2;
            const qreal r = qMax<qreal>(2, xHeight * 0.3);
            const auto level = uint8_t((at - afterQuotesOf(block)) / 2 % 3);
            out.push_back({Shape::Bullet, level, QRectF(cx - r, cy - r, 2 * r, 2 * r)});
            break;
        }
        case Md::Task:
            out.push_back({Shape::Box, l.checked, MarkdownEditor::taskBox(m_doc, block)});
            break;
        case Md::RuleLine:
            out.push_back({Shape::Rule, 0, QRectF(br.left(), qRound(br.center().y()), right - br.left(), 1)});
            break;
        case Md::CodeLine:
        case Md::FenceLine: {
            const uint8_t ends = (isCode(block.previous()) ? 0 : 1) | (isCode(block.next()) ? 0 : 2);
            const qreal pad = 6;
            const QRectF box(br.left() - pad, br.top(), right - br.left() + 2 * pad, br.height());
            out.push_back({Shape::Code, ends, box});
            // The language, small at the top right of an opening fence. While
            // the caret is on the line its own text shows instead.
            if (l.kind == Md::FenceLine && l.state != 0 && !m_editor->fenceRevealed(block)) {
                const QString label = fenceLabel(block.text());
                if (!label.isEmpty()) {
                    const QFontMetricsF fm(labelFont(m_editor->font()));
                    const qreal inset = 10;
                    const QString shown = fm.elidedText(label, Qt::ElideRight, qMax<qreal>(24, box.width() * 0.4));
                    const qreal w = fm.horizontalAdvance(shown);
                    out.push_back({Shape::Label, 0, QRectF(box.right() - inset - w, box.top(), w, box.height()), shown});
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

int MarkdownDecorations::afterQuotesOf(const QTextBlock &block)
{
    const BlockInfo *info = BlockInfo::of(block);
    if (!info || info->quoteMarks.isEmpty()) {
        return 0;
    }
    return info->quoteMarks.last() + 2;
}

void MarkdownDecorations::paint(QPainter *painter)
{
    if (!m_editor || m_shapes.empty()) {
        return;
    }
    const QPointF origin = m_editor->textOrigin();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(origin.x(), origin.y() - y());
    const QColor text = m_editor->textColor();
    const QColor dim = m_editor->dimColor();
    const QColor accent = m_editor->accentColor();
    QColor faint = dim;
    faint.setAlphaF(0.45f);

    for (const Shape &s : m_shapes) {
        switch (s.type) {
        case Shape::Bullet:
            if (s.level == 1) {
                painter->setPen(QPen(text, 1.2));
                painter->setBrush(Qt::NoBrush);
                painter->drawEllipse(s.rect.adjusted(0.6, 0.6, -0.6, -0.6));
            } else {
                painter->setPen(Qt::NoPen);
                painter->setBrush(text);
                if (s.level == 0) {
                    painter->drawEllipse(s.rect);
                } else {
                    painter->drawRect(s.rect.adjusted(0.5, 0.5, -0.5, -0.5));
                }
            }
            break;
        case Shape::Box:
            if (s.level) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(accent);
                painter->drawRoundedRect(s.rect.adjusted(-0.5, -0.5, 0.5, 0.5), 3.5, 3.5);
                QPainterPath tick;
                const QRectF r = s.rect;
                tick.moveTo(r.left() + r.width() * 0.24, r.top() + r.height() * 0.52);
                tick.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.70);
                tick.lineTo(r.left() + r.width() * 0.77, r.top() + r.height() * 0.32);
                painter->setPen(QPen(Qt::white, qMax<qreal>(1.4, r.width() / 9), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(tick);
            } else {
                painter->setPen(QPen(dim, 1.3));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(s.rect, 3, 3);
            }
            break;
        case Shape::Quote:
            painter->setPen(Qt::NoPen);
            painter->setBrush(faint);
            painter->drawRoundedRect(s.rect, 1.5, 1.5);
            break;
        case Shape::Rule:
            painter->fillRect(s.rect, faint);
            break;
        case Shape::Label:
            painter->save();
            painter->setFont(labelFont(m_editor->font()));
            painter->setPen(dim);
            painter->drawText(s.rect, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, s.text);
            painter->restore();
            break;
        case Shape::Code: {
            constexpr qreal radius = 6;
            QRectF r = s.rect;
            if (!(s.level & 1)) {
                r.setTop(r.top() - 2 * radius);
            }
            if (!(s.level & 2)) {
                r.setBottom(r.bottom() + 2 * radius);
            }
            painter->save();
            painter->setClipRect(s.rect);
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_editor->codeColor());
            painter->drawRoundedRect(r, radius, radius);
            painter->restore();
            break;
        }
        }
    }
}
