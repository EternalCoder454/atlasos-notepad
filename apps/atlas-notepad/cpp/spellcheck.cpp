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
#include <QSet>
#include <QTextCursor>

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
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
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
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
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

void SpellUnderlines::paint(QPainter *painter)
{
    QQuickItem *edit = m_spell ? m_spell->textEdit() : nullptr;
    if (m_lines.isEmpty() || !edit) {
        return;
    }
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(edit->property("leftPadding").toReal(), edit->property("topPadding").toReal() - y());
    painter->setPen(QPen(m_spell->underlineColor(), 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    constexpr qreal step = 2.5; // half a wave
    constexpr qreal amp = 1.25;
    for (const QLineF &l : std::as_const(m_lines)) {
        QPainterPath wave(QPointF(l.x1(), l.y1()));
        bool up = true;
        for (qreal x = l.x1(); x < l.x2(); x += step) {
            const qreal next = qMin(x + step, l.x2());
            wave.quadTo((x + next) / 2, l.y1() + (up ? -amp : amp) * 2, next, l.y1());
            up = !up;
        }
        painter->drawPath(wave);
    }
}

#include "spellcheck.moc"
