// Spell check through Sonnet (the system's Hunspell dictionaries). One
// SpellChecker per editor view; they share one speller and its answers.
// The Markdown highlighter asks it about the words it reads; for plain text
// it runs a highlighter of its own (MarkdownHighlighter with Markdown off).
#pragma once

#include <QColor>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTextDocument>

class MarkdownHighlighter;

class SpellChecker : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // The TextEdit whose document it reads.
    Q_PROPERTY(QQuickItem *textEdit READ textEdit WRITE setTextEdit NOTIFY textEditChanged)
    // Off: no underlines and no suggestions.
    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)
    // On for plain text: underlines with its own highlighter. Off when a
    // MarkdownEditor's highlighter does it.
    Q_PROPERTY(bool plainText READ plainText WRITE setPlainText NOTIFY plainTextChanged)
    Q_PROPERTY(QColor underlineColor READ underlineColor WRITE setUnderlineColor NOTIFY underlineColorChanged)

public:
    // A word to check: [start, end) in the line, which may include
    // characters the word skips over (hidden Markdown markers).
    struct Word {
        int start;
        int end;
        QString text;
    };
    // How each character of a line takes part in words.
    enum CharClass : quint8 {
        Normal, // letters join, others break
        Skip,   // not part of the word, but doesn't break it (a hidden "**")
        Break,  // ends any word and is never checked (code, links)
    };

    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    QQuickItem *textEdit() const
    {
        return m_edit;
    }
    QTextDocument *document() const
    {
        return m_doc;
    }
    void setTextEdit(QQuickItem *edit);
    bool isActive() const
    {
        return m_active;
    }
    void setActive(bool on);
    bool plainText() const
    {
        return m_plainText;
    }
    void setPlainText(bool on);
    QColor underlineColor() const
    {
        return m_color;
    }
    void setUnderlineColor(const QColor &color);
    // Highlights its plain-text document again (after a MarkdownEditor's
    // highlighter left it and cleared the formats, or a word was added).
    void rehighlight();

    // The words of a line worth checking. `classes` is empty (all Normal) or
    // one per character. Words with digits, underscores, capitals after the
    // first letter (camelCase, acronyms), or a single letter are left out, as
    // are web and mail addresses.
    static QList<Word> words(const QString &line, const QList<CharClass> &classes = {});
    // The misspelled words of a line, by `words`.
    QList<Word> misspelled(const QString &line, const QList<CharClass> &classes = {}) const;
    bool isMisspelled(const QString &word) const;

    // The misspelled word at a position of the document, for the context
    // menu: {start, end, word, suggestions}, or empty. No suggestions for a
    // word with markers inside (**bo**ld): replacing would drop them.
    Q_INVOKABLE QVariantMap wordAt(int position) const;
    // Replaces [start, end) of the document, if it still reads `word`, with
    // `text`, one undo step.
    Q_INVOKABLE void replace(int start, int end, const QString &word, const QString &text);
    // Never underlined again, in any app using the same dictionary.
    Q_INVOKABLE void addToDictionary(const QString &word);
    // Not underlined until Notepad quits.
    Q_INVOKABLE void ignore(const QString &word);

    // For tests: the dictionary's language ("" when there is none).
    static QString language();
    static void setLanguage(const QString &language);

Q_SIGNALS:
    void textEditChanged();
    void activeChanged();
    void plainTextChanged();
    void underlineColorChanged();
    // The underlines changed (on, off, a word added): highlight again.
    void changed();

private:
    void updateHighlighter();

    QPointer<QQuickItem> m_edit;
    QPointer<QTextDocument> m_doc;
    QPointer<MarkdownHighlighter> m_highlighter;
    bool m_active = false;
    bool m_plainText = false;
    QColor m_color = Qt::red;
};

// Red squiggles under the misspelled words of the visible lines (from
// BlockInfo::misspelled). Make it a child of the TextEdit laid over the
// visible part only (y = the view's contentY, height = the view's), as
// MarkdownDecorations. Each squiggle is a node of its own, cut from one
// shared texture, so typing repaints only the squiggles that moved, not
// everything under the item.
class SpellUnderlines : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(SpellChecker *spellChecker READ spellChecker WRITE setSpellChecker NOTIFY spellCheckerChanged)

public:
    explicit SpellUnderlines(QQuickItem *parent = nullptr);

    SpellChecker *spellChecker() const
    {
        return m_spell;
    }
    void setSpellChecker(SpellChecker *spell);

Q_SIGNALS:
    void spellCheckerChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void updatePolish() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private Q_SLOTS:
    void paddingChanged()
    {
        polish(); // which lines are visible
        update(); // where the squiggles go
    }

private:
    void watch();
    // Each squiggle's baseline: x from, x to, y; in document coordinates.
    QList<QLineF> layOut() const;

    QPointer<SpellChecker> m_spell;
    QPointer<QQuickItem> m_edit; // for its padding
    QPointer<QTextDocument> m_doc;
    QList<QLineF> m_lines;
    int m_textureFailures = 0; // in a row
};
