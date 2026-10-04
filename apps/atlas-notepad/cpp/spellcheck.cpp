#include "spellcheck.h"

#include "markdown.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QSet>
#include <QTextCursor>
#include <QTimer>
#include <QtMath>

#include <Sonnet/Speller>

#include <memory>

namespace
{
// The script of a dictionary's language, for the scripts whose words have
// spaces between them. Others (and CJK, which has none) aren't limited.
QChar::Script scriptOf(const QString &language)
{
    switch (QLocale(language).script()) {
    case QLocale::LatinScript:
        return QChar::Script_Latin;
    case QLocale::CyrillicScript:
        return QChar::Script_Cyrillic;
    case QLocale::GreekScript:
        return QChar::Script_Greek;
    case QLocale::ArabicScript:
        return QChar::Script_Arabic;
    case QLocale::HebrewScript:
        return QChar::Script_Hebrew;
    default:
        return QChar::Script_Unknown;
    }
}

// One speller for every view, made on first use (loading a dictionary takes
// a while), and what it said about each word so far.
struct Shared : QObject {
    Q_OBJECT
public:
    std::unique_ptr<Sonnet::Speller> speller;
    QString language;
    // The script the dictionary's words are in; words in another are never
    // underlined. Script_Unknown: no limit.
    QChar::Script script = QChar::Script_Unknown;
    mutable QHash<QString, bool> answers;

    Shared()
    {
        // The speller saves its personal dictionary; do it while Qt is up.
        connect(qApp, &QCoreApplication::aboutToQuit, this, &Shared::forget);
    }

    Sonnet::Speller *get()
    {
        if (!speller) {
            speller = std::make_unique<Sonnet::Speller>(language);
            // No dictionary for the locale (or the C locale): the UI
            // languages, then English, then any installed one.
            if (!speller->isValid() && language.isEmpty()) {
                const QStringList available = speller->availableLanguages();
                QStringList wanted = QLocale::system().uiLanguages(QLocale::TagSeparator::Underscore);
                wanted << QStringLiteral("en_US") << available.value(0);
                for (const QString &candidate : std::as_const(wanted)) {
                    if (available.contains(candidate)) {
                        speller->setLanguage(candidate);
                        break;
                    }
                }
            }
            script = scriptOf(speller->language());
        }
        return speller->isValid() ? speller.get() : nullptr;
    }
    void forget()
    {
        speller.reset();
        answers.clear();
    }

Q_SIGNALS:
    // A word was added or ignored, or the language changed.
    void changed();
};

Shared &shared()
{
    // Owned by the app, so it goes before Qt does.
    static QPointer<Shared> s;
    if (!s) {
        s = new Shared;
        s->setParent(QCoreApplication::instance());
    }
    return *s;
}

bool isApostrophe(QChar c)
{
    return c == u'\'' || c == QChar(0x2019);
}

bool isWordChar(char32_t c)
{
    return QChar::isLetterOrNumber(c) || QChar::isMark(c) || c == u'_';
}

// The code point at `k` and how many UTF-16 units it takes (a lone
// surrogate is U+FFFD).
std::pair<char32_t, int> codePointAt(const QString &text, qsizetype k)
{
    const QChar c = text.at(k);
    if (c.isHighSurrogate() && k + 1 < text.size() && text.at(k + 1).isLowSurrogate()) {
        return {QChar::surrogateToUcs4(c, text.at(k + 1)), 2};
    }
    return {c.isSurrogate() ? char32_t(0xFFFD) : char32_t(c.unicode()), 1};
}

// Only letters of `script` (or ones shared by all scripts).
bool inScript(const QString &word, QChar::Script script)
{
    if (script == QChar::Script_Unknown) {
        return true;
    }
    for (qsizetype k = 0; k < word.size();) {
        const auto [c, units] = codePointAt(word, k);
        const QChar::Script s = QChar::script(c);
        if (s != script && s != QChar::Script_Common && s != QChar::Script_Inherited) {
            return false;
        }
        k += units;
    }
    return true;
}

// A web or mail address: never checked.
bool isAddress(QStringView chunk)
{
    return chunk.contains(u"://") || chunk.contains(u'@') || chunk.startsWith(u"www.", Qt::CaseInsensitive);
}

// Worth asking the dictionary about: no digits or underscores, more than one
// letter, and not an acronym or camelCase (BBC, iPhone, fooBar).
bool isCheckable(const QString &word)
{
    if (word.size() < 2) {
        return false;
    }
    bool upperAfterFirst = false;
    for (qsizetype i = 0; i < word.size(); ++i) {
        const QChar c = word.at(i);
        if (c.isDigit() || c == u'_') {
            return false;
        }
        if (i > 0 && c.isUpper()) {
            upperAfterFirst = true;
        }
    }
    return !upperAfterFirst;
}
} // namespace

SpellChecker::SpellChecker(QObject *parent)
    : QObject(parent)
{
    connect(&shared(), &Shared::changed, this, [this] {
        rehighlight();
        Q_EMIT changed();
    });
}

SpellChecker::~SpellChecker()
{
    delete m_highlighter;
}

void SpellChecker::setTextEdit(QQuickItem *edit)
{
    if (edit == m_edit) {
        return;
    }
    m_edit = edit;
    auto *textDocument = edit ? qobject_cast<QQuickTextDocument *>(edit->property("textDocument").value<QObject *>()) : nullptr;
    QTextDocument *doc = textDocument ? textDocument->textDocument() : nullptr;
    if (doc != m_doc) {
        delete m_highlighter;
        m_doc = doc;
    }
    updateHighlighter();
    Q_EMIT textEditChanged();
}

void SpellChecker::setActive(bool on)
{
    if (on == m_active) {
        return;
    }
    m_active = on;
    updateHighlighter();
    Q_EMIT activeChanged();
    Q_EMIT changed();
}

void SpellChecker::setPlainText(bool on)
{
    if (on == m_plainText) {
        return;
    }
    m_plainText = on;
    updateHighlighter();
    Q_EMIT plainTextChanged();
}

void SpellChecker::setUnderlineColor(const QColor &color)
{
    if (color == m_color) {
        return;
    }
    m_color = color;
    Q_EMIT underlineColorChanged();
}

void SpellChecker::rehighlight()
{
    if (m_highlighter) {
        m_highlighter->rehighlightAll();
    }
}

// Plain text needs a highlighter of its own while spell check is on.
// Removing a highlighter clears every line's formats, so a MarkdownEditor
// on the same document is told (changed) to highlight again.
void SpellChecker::updateHighlighter()
{
    const bool want = m_active && m_plainText && m_doc;
    if (!want) {
        if (m_highlighter) {
            delete m_highlighter;
            Q_EMIT changed();
        }
        return;
    }
    if (!m_highlighter) {
        m_highlighter = new MarkdownHighlighter(m_doc, false);
        m_highlighter->setSpellChecker(this);
        MarkdownStyle style;
        style.formatted = false;
        m_highlighter->setStyle(style); // highlights
    } else {
        m_highlighter->rehighlightAll(); // on again
    }
}

QList<SpellChecker::Word> SpellChecker::words(const QString &line, const QList<CharClass> &classes)
{
    QList<Word> found;
    const qsizetype n = line.size();
    auto classOf = [&](qsizetype i) {
        return classes.isEmpty() ? Normal : classes.at(i);
    };
    qsizetype i = 0;
    while (i < n) {
        // One whitespace-separated chunk at a time, so addresses are skipped
        // whole.
        while (i < n && line.at(i).isSpace()) {
            ++i;
        }
        const qsizetype chunkStart = i;
        while (i < n && !line.at(i).isSpace()) {
            ++i;
        }
        if (isAddress(QStringView(line).mid(chunkStart, i - chunkStart))) {
            continue;
        }
        Word word{-1, -1, {}};
        auto finish = [&] {
            if (word.start >= 0 && isCheckable(word.text)) {
                found.append(word);
            }
            word = {-1, -1, {}};
        };
        for (qsizetype k = chunkStart; k < i; ++k) {
            const CharClass cls = classOf(k);
            if (cls == Skip) {
                continue;
            }
            const auto [c, units] = codePointAt(line, k);
            if (cls == Normal && isWordChar(c)) {
                if (word.start < 0) {
                    word.start = int(k);
                }
                word.text.append(QStringView(line).mid(k, units));
                k += units - 1;
                word.end = int(k) + 1;
                continue;
            }
            // An apostrophe between letters belongs to the word (don't, it's).
            if (cls == Normal && word.start >= 0 && isApostrophe(line.at(k))) {
                qsizetype next = k + 1;
                while (next < i && classOf(next) == Skip) {
                    ++next;
                }
                if (next < i && classOf(next) == Normal && line.at(next).isLetter()) {
                    word.text.append(u'\'');
                    continue;
                }
            }
            finish();
        }
        finish();
    }
    return found;
}

QList<SpellChecker::Word> SpellChecker::misspelled(const QString &line, const QList<CharClass> &classes) const
{
    QList<Word> found = words(line, classes);
    found.removeIf([this](const Word &w) {
        return !isMisspelled(w.text);
    });
    return found;
}

bool SpellChecker::isMisspelled(const QString &word) const
{
    Shared &s = shared();
    if (auto it = s.answers.constFind(word); it != s.answers.cend()) {
        return *it;
    }
    Sonnet::Speller *speller = s.get();
    const bool wrong = speller && inScript(word, s.script) && speller->isMisspelled(word);
    // Every half-typed word is asked about too; don't keep them forever.
    if (s.answers.size() > 20000) {
        s.answers.clear();
    }
    s.answers.insert(word, wrong);
    return wrong;
}

QVariantMap SpellChecker::wordAt(int position) const
{
    if (!m_active || !m_doc) {
        return {};
    }
    const QTextBlock block = m_doc->findBlock(position);
    const BlockInfo *info = BlockInfo::of(block);
    if (!info) {
        return {};
    }
    const int at = position - block.position();
    for (const auto &[start, end] : info->misspelled) {
        // The caret just after a word counts too.
        if (at < start || at > end) {
            continue;
        }
        // The word as the dictionary saw it: without hidden markers.
        const QString text = block.text();
        QString word;
        bool split = false;
        for (int k = start; k < end; ++k) {
            bool hidden = false;
            for (const auto &[hs, he] : info->hidden) {
                hidden = hidden || (k >= hs && k < he);
            }
            if (!hidden) {
                word.append(isApostrophe(text.at(k)) ? QChar(u'\'') : text.at(k));
            }
            // Markers inside it (**bo**ld): a suggestion would replace them.
            const QChar c = text.at(k);
            split = split || hidden || !(c.isLetterOrNumber() || c.isMark() || c.isSurrogate() || isApostrophe(c));
        }
        QStringList suggestions;
        Sonnet::Speller *speller = shared().get();
        if (speller && !split) {
            suggestions = speller->suggest(word).mid(0, 5);
        }
        return {
            {QStringLiteral("start"), block.position() + start},
            {QStringLiteral("end"), block.position() + end},
            {QStringLiteral("word"), word},
            {QStringLiteral("suggestions"), suggestions},
        };
    }
    return {};
}

void SpellChecker::replace(int start, int end, const QString &word, const QString &text)
{
    if (!m_doc || start < 0 || end < start || end >= m_doc->characterCount()) {
        return;
    }
    QTextCursor cursor(m_doc);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    // The text changed since the menu opened.
    QString was = cursor.selectedText();
    was.replace(QChar(0x2019), u'\'');
    if (was != word) {
        return;
    }
    cursor.beginEditBlock();
    cursor.insertText(text);
    cursor.endEditBlock();
}

void SpellChecker::addToDictionary(const QString &word)
{
    Shared &s = shared();
    if (Sonnet::Speller *speller = s.get()) {
        speller->addToPersonal(word);
    }
    s.answers.clear(); // other forms of it too (Zach's, ZACH)
    Q_EMIT s.changed();
}

void SpellChecker::ignore(const QString &word)
{
    Shared &s = shared();
    if (Sonnet::Speller *speller = s.get()) {
        speller->addToSession(word);
    }
    s.answers.clear(); // other forms of it too (Zach's, ZACH)
    Q_EMIT s.changed();
}

QString SpellChecker::language()
{
    Sonnet::Speller *speller = shared().get();
    return speller ? speller->language() : QString();
}

void SpellChecker::setLanguage(const QString &language)
{
    Shared &s = shared();
    s.forget();
    s.language = language;
    Q_EMIT s.changed();
}

SpellUnderlines::SpellUnderlines(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
}

void SpellUnderlines::setSpellChecker(SpellChecker *spell)
{
    if (spell == m_spell) {
        return;
    }
    if (m_spell) {
        disconnect(m_spell, nullptr, this, nullptr);
    }
    m_spell = spell;
    if (spell) {
        connect(spell, &SpellChecker::textEditChanged, this, &SpellUnderlines::watch);
        connect(spell, &SpellChecker::changed, this, [this] {
            polish();
        });
        connect(spell, &SpellChecker::underlineColorChanged, this, [this] {
            update();
        });
        connect(spell, &QObject::destroyed, this, [this] {
            polish();
        });
    }
    watch();
    Q_EMIT spellCheckerChanged();
}

void SpellUnderlines::watch()
{
    if (m_doc) {
        disconnect(m_doc->documentLayout(), nullptr, this, nullptr);
    }
    if (m_edit) {
        disconnect(m_edit, nullptr, this, nullptr);
    }
    // The squiggles are placed by the padding (updatePaintNode).
    m_edit = m_spell ? m_spell->textEdit() : nullptr;
    if (m_edit) {
        connect(m_edit, SIGNAL(leftPaddingChanged()), this, SLOT(paddingChanged()));
        connect(m_edit, SIGNAL(topPaddingChanged()), this, SLOT(paddingChanged()));
    }
    m_doc = m_spell ? m_spell->document() : nullptr;
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

void SpellUnderlines::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    // Moved with the view: the same lines, painted at another place.
    if (newGeometry.y() != oldGeometry.y()) {
        update();
    }
    polish();
}

void SpellUnderlines::updatePolish()
{
    QList<QLineF> lines = layOut();
    if (lines != m_lines) {
        m_lines.swap(lines);
        update();
    }
}

QList<QLineF> SpellUnderlines::layOut() const
{
    QList<QLineF> out;
    QQuickItem *edit = m_spell ? m_spell->textEdit() : nullptr;
    if (!m_spell || !m_spell->isActive() || !m_doc || !edit) {
        return out;
    }
    auto *layout = m_doc->documentLayout();
    const qreal originY = edit->property("topPadding").toReal();
    const qreal top = y() - originY;
    const qreal bottom = top + height();
    const int first = layout->hitTest(QPointF(0, qMax<qreal>(0, top)), Qt::FuzzyHit);
    for (QTextBlock block = m_doc->findBlock(qMax(0, first)); block.isValid(); block = block.next()) {
        const QRectF br = layout->blockBoundingRect(block);
        if (br.top() > bottom) {
            break;
        }
        const BlockInfo *info = BlockInfo::of(block);
        QTextLayout *tl = block.layout();
        if (!info || info->misspelled.isEmpty() || !tl || tl->lineCount() == 0) {
            continue;
        }
        for (const auto &[start, end] : info->misspelled) {
            // A wrapped word: one squiggle per line it is on.
            for (int from = start; from < end;) {
                const QTextLine line = tl->lineForTextPosition(from);
                if (!line.isValid()) {
                    break;
                }
                const int to = qMin(end, line.textStart() + line.textLength());
                const qreal y = br.top() + line.y() + line.ascent() + qMax<qreal>(1.5, line.descent() * 0.45);
                const qreal a = line.cursorToX(from);
                const qreal b = line.cursorToX(to); // left of a in right-to-left text
                out.append(QLineF(br.left() + qMin(a, b), y, br.left() + qMax(a, b), y));
                if (to <= from) {
                    break;
                }
                from = to;
            }
        }
    }
    return out;
}

void SpellUnderlines::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    // Another screen: the texture is drawn again at its scale.
    if (change == ItemDevicePixelRatioHasChanged) {
        update();
    }
}

namespace
{
// The wave: half-waves of waveStep, the curve waveAmp above and below the
// baseline, a 1.1 px pen. Stroking it is slow (a few ms for the strip), so
// one tile of it is drawn, copied along a strip, and the strip kept: each
// squiggle shows part of it. The tile is four waves wide, a whole number of
// pixels at the usual scales (1, 1.25, 1.5, 1.75, 2).
constexpr qreal waveStep = 2.5;
constexpr qreal waveAmp = 1.25;
constexpr qreal tileWidth = 8 * waveStep;
constexpr qreal stripWidth = 25 * tileWidth;
constexpr qreal stripHeight = 7; // at least

// The baseline in the strip: the middle of a pixel row, so a squiggle at
// 1x is crisp rather than spread over two rows.
qreal baseline(int pixelHeight, qreal dpr)
{
    return (pixelHeight / 2 + 0.5) / dpr;
}

QImage drawStrip(const QColor &color, qreal dpr)
{
    const int tileW = qMax(1, qRound(tileWidth * dpr));
    // Whole pixels high, and shown that high (see updatePaintNode), so the
    // wave isn't squashed at 1.25x or 1.75x.
    const int h = qCeil(stripHeight * dpr);
    QImage tile(tileW, h, QImage::Format_ARGB32_Premultiplied);
    tile.fill(Qt::transparent);
    {
        QPainter painter(&tile);
        painter.setRenderHint(QPainter::Antialiasing);
        // Scaled to the tile's whole pixel width, so the copies join.
        painter.scale(tileW / tileWidth, dpr);
        painter.setPen(QPen(color, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal y = baseline(h, dpr);
        // A wave either side past the edges, so the copies join without caps.
        QPainterPath wave(QPointF(-2 * waveStep, y));
        bool up = true;
        for (qreal x = -2 * waveStep; x < tileWidth + 2 * waveStep; x += waveStep) {
            wave.quadTo(x + waveStep / 2, y + (up ? -waveAmp : waveAmp) * 2, x + waveStep, y);
            up = !up;
        }
        painter.drawPath(wave);
    }
    QImage strip(tileW * qRound(stripWidth / tileWidth), h, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&strip);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (int x = 0; x < strip.width(); x += tileW) {
        painter.drawImage(x, 0, tile);
    }
    return strip;
}

// The squiggles' parent: places them (document to item coordinates) and owns
// the strip they share.
class SquiggleRoot : public QSGTransformNode
{
public:
    ~SquiggleRoot() override
    {
        delete texture;
    }
    QSGTexture *texture = nullptr;
    QColor color;
    qreal dpr = 0;
};
}

QSGNode *SpellUnderlines::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    QQuickItem *edit = m_spell ? m_spell->textEdit() : nullptr;
    if (!edit || !window()) {
        delete old;
        return nullptr;
    }
    // Kept without squiggles too: the next misspelling, often the next
    // keystroke, would make the strip again.
    auto *root = static_cast<SquiggleRoot *>(old);
    if (!root) {
        root = new SquiggleRoot;
    }
    const qreal dpr = window()->effectiveDevicePixelRatio();
    const QColor color = m_spell->underlineColor();
    if (!root->texture || root->color != color || root->dpr != dpr) {
        QSGTexture *texture = window()->createTextureFromImage(drawStrip(color, dpr));
        if (!texture) {
            // Tried again after 1, 2, 4, 8 and 16 s, then left until the
            // next change; underlines missing beat a warning every second.
            if (m_textureFailures < 5) {
                const int wait = 1000 << m_textureFailures++;
                qWarning("atlas-notepad: no texture for the spelling underlines; trying again in %d s", wait / 1000);
                QMetaObject::invokeMethod(this, [this, wait] { QTimer::singleShot(wait, this, &QQuickItem::update); }, Qt::QueuedConnection);
            }
            delete root;
            return nullptr;
        }
        m_textureFailures = 0;
        texture->setFiltering(QSGTexture::Linear);
        // Their source rects are in the old strip's pixels: made anew below.
        while (QSGNode *n = root->firstChild()) {
            root->removeChildNode(n);
            delete n;
        }
        delete root->texture;
        root->texture = texture;
        root->color = color;
        root->dpr = dpr;
    }
    // Setting the matrix repaints every squiggle: only when it changed.
    QMatrix4x4 matrix;
    matrix.translate(edit->property("leftPadding").toReal(), edit->property("topPadding").toReal() - y());
    if (root->matrix() != matrix) {
        root->setMatrix(matrix);
    }

    const QSize pixels = root->texture->textureSize();
    const qreal height = pixels.height() / dpr;
    const qreal top = baseline(pixels.height(), dpr);
    // A squiggle longer than the strip is shown in pieces.
    QList<QRectF> rects;
    for (const QLineF &l : std::as_const(m_lines)) {
        for (qreal x = l.x1(); x < l.x2(); x += stripWidth) {
            rects.append(QRectF(x, l.y1() - top, qMin(stripWidth, l.x2() - x), height));
        }
    }
    // A node whose rect hasn't changed is left alone, so it isn't repainted;
    // the others are moved to the new rects, added or removed.
    QMultiHash<std::pair<qreal, qreal>, qsizetype> wanted;
    for (qsizetype i = 0; i < rects.size(); ++i) {
        wanted.insert({rects[i].x(), rects[i].y()}, i);
    }
    QList<bool> placed(rects.size(), false);
    QList<QSGImageNode *> spare;
    for (QSGNode *n = root->firstChild(); n; n = n->nextSibling()) {
        auto *image = static_cast<QSGImageNode *>(n);
        const QRectF r = image->rect();
        bool kept = false;
        for (auto it = wanted.find({r.x(), r.y()}); it != wanted.end() && it.key() == std::pair(r.x(), r.y()); ++it) {
            if (!placed[*it] && rects[*it] == r) {
                placed[*it] = true;
                kept = true;
                break;
            }
        }
        if (!kept) {
            spare.append(image);
        }
    }
    for (qsizetype i = 0; i < rects.size(); ++i) {
        if (placed[i]) {
            continue;
        }
        QSGImageNode *image;
        if (!spare.isEmpty()) {
            image = spare.takeLast();
        } else {
            image = window()->createImageNode();
            image->setOwnsTexture(false);
            image->setFiltering(QSGTexture::Linear);
            image->setTexture(root->texture);
            root->appendChildNode(image);
        }
        image->setRect(rects[i]);
        image->setSourceRect(QRectF(0, 0, rects[i].width() * pixels.width() / stripWidth, pixels.height()));
    }
    for (QSGImageNode *image : std::as_const(spare)) {
        root->removeChildNode(image);
        delete image;
    }
    return root;
}

#include "spellcheck.moc"
