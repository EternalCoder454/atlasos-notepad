// The line-number gutter. Like the Markdown decorations it works out the
// visible blocks (the layout's hit test finds the first one) and draws only
// those; it repaints when the caret changes block, the view scrolls or the
// layout changes, not on every key.
#include "app.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QPointer>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

struct LineNumbers::Private {
    QPointer<QQuickItem> edit;
    QPointer<QQuickItem> flickable;
    QPointer<QTextDocument> doc;
    QFont font;
    QColor color = Qt::gray;
    QColor current = Qt::black;
    qreal padding = 6;
    int caretBlock = -1;
};

namespace
{
// Connects a QML item's signal if it has one (the names are stable, the
// types are private to Qt Quick).
void hook(QObject *from, const char *signal, QObject *to, const char *slot)
{
    if (from->metaObject()->indexOfSignal(signal) >= 0) {
        QObject::connect(from, QByteArray("2") + signal, to, QByteArray("1") + slot);
    }
}
}

LineNumbers::LineNumbers(QQuickItem *parent)
    : QQuickPaintedItem(parent)
    , d(std::make_unique<Private>())
{
    // setAntialiasing(false) only drops the shape render hints: text is
    // antialiased by the font's own strategy, so the digits stay smooth.
    setAntialiasing(false);
    d->font = QFont();
}

LineNumbers::~LineNumbers() = default;

QQuickItem *LineNumbers::textEdit() const
{
    return d->edit;
}

void LineNumbers::setTextEdit(QQuickItem *edit)
{
    if (edit == d->edit) {
        return;
    }
    if (d->edit) {
        disconnect(d->edit, nullptr, this, nullptr);
    }
    if (d->doc) {
        disconnect(d->doc, nullptr, this, nullptr);
        disconnect(d->doc->documentLayout(), nullptr, this, nullptr);
    }
    d->edit = edit;
    d->doc = nullptr;
    d->caretBlock = -1;
    if (edit) {
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(edit->property("textDocument").value<QObject *>());
        d->doc = quickDocument ? quickDocument->textDocument() : nullptr;
        hook(edit, "cursorPositionChanged()", this, "caretMoved()");
        hook(edit, "contentHeightChanged()", this, "relayout()");
        hook(edit, "heightChanged()", this, "relayout()");
        hook(edit, "widthChanged()", this, "relayout()");
        hook(edit, "topPaddingChanged()", this, "relayout()");
        hook(edit, "yChanged()", this, "relayout()");
        if (d->doc) {
            connect(d->doc, &QTextDocument::blockCountChanged, this, &LineNumbers::relayout);
            connect(d->doc->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, &LineNumbers::relayout);
        }
        caretMoved();
    }
    relayout();
    Q_EMIT textEditChanged();
}

QQuickItem *LineNumbers::flickable() const
{
    return d->flickable;
}

void LineNumbers::setFlickable(QQuickItem *flickable)
{
    if (flickable == d->flickable) {
        return;
    }
    if (d->flickable) {
        disconnect(d->flickable, nullptr, this, nullptr);
    }
    d->flickable = flickable;
    if (flickable) {
        hook(flickable, "contentYChanged()", this, "relayout()");
        hook(flickable, "heightChanged()", this, "relayout()");
    }
    update();
    Q_EMIT flickableChanged();
}

QFont LineNumbers::font() const
{
    return d->font;
}

void LineNumbers::setFont(const QFont &font)
{
    if (font == d->font) {
        return;
    }
    d->font = font;
    relayout();
    Q_EMIT styleChanged();
}

QColor LineNumbers::color() const
{
    return d->color;
}

void LineNumbers::setColor(const QColor &color)
{
    if (color != d->color) {
        d->color = color;
        update();
        Q_EMIT styleChanged();
    }
}

QColor LineNumbers::currentColor() const
{
    return d->current;
}

void LineNumbers::setCurrentColor(const QColor &color)
{
    if (color != d->current) {
        d->current = color;
        update();
        Q_EMIT styleChanged();
    }
}

qreal LineNumbers::padding() const
{
    return d->padding;
}

void LineNumbers::setPadding(qreal padding)
{
    if (padding != d->padding) {
        d->padding = padding;
        relayout();
        Q_EMIT styleChanged();
    }
}

void LineNumbers::caretMoved()
{
    if (!d->edit || !d->doc) {
        return;
    }
    const int block = d->doc->findBlock(d->edit->property("cursorPosition").toInt()).blockNumber();
    if (block != d->caretBlock) {
        d->caretBlock = block;
        update();
    }
}

void LineNumbers::relayout()
{
    // The width follows the digits of the line count.
    const int blocks = d->doc ? d->doc->blockCount() : 1;
    const int digits = qMax(2, int(QString::number(blocks).size()));
    const QFontMetricsF fm(d->font);
    const qreal width = fm.horizontalAdvance(QLatin1Char('0')) * digits + 2 * d->padding;
    if (!qFuzzyCompare(width, implicitWidth())) {
        setImplicitWidth(width);
    }
    update();
}

void LineNumbers::paint(QPainter *painter)
{
    if (!d->edit || !d->doc) {
        return;
    }
    auto *layout = d->doc->documentLayout();
    const qreal topPadding = d->edit->property("topPadding").toReal();
    // The part of the document this item shows, in document coordinates.
    const qreal docTop = mapToItem(d->edit, QPointF(0, 0)).y() - topPadding;
    const qreal docBottom = mapToItem(d->edit, QPointF(0, height())).y() - topPadding;
    const int first = layout->hitTest(QPointF(0, qMax<qreal>(0, docTop)), Qt::FuzzyHit);
    const QFontMetricsF fm(d->font);

    painter->setFont(d->font);
    for (QTextBlock block = d->doc->findBlock(qMax(0, first)); block.isValid(); block = block.next()) {
        const QRectF br = layout->blockBoundingRect(block);
        if (br.top() > docBottom) {
            break;
        }
        QTextLayout *tl = block.layout();
        if (br.bottom() < docTop || !block.isVisible() || !tl || tl->lineCount() == 0) {
            continue;
        }
        const QTextLine line = tl->lineAt(0);
        const qreal baseline = d->edit->mapToItem(this, QPointF(0, topPadding + br.top() + line.y() + line.ascent())).y();
        const QString number = QString::number(block.blockNumber() + 1);
        painter->setPen(block.blockNumber() == d->caretBlock ? d->current : d->color);
        painter->drawText(QPointF(width() - d->padding - fm.horizontalAdvance(number), baseline), number);
    }
}
