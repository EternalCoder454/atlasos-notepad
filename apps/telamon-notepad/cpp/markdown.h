// The Formatted view: Markdown kept as the document's text and drawn
// formatted. Rust reads each line (crates/notepad-core/src/markdown.rs); the
// highlighter here turns that into formats, MarkdownEditor keeps the caret
// out of hidden syntax and does the toolbar's edits, and MarkdownDecorations
// draws what text can't show: bullets, checkboxes, quote bars, rules and
// code block backgrounds.
#pragma once

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBlockUserData>
#include <QTextCursor>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "spellcheck.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

// Rust, see apps/telamon-notepad/src/lib.rs. Positions count UTF-16 units.
extern "C" {
struct NpRun {
    uint32_t start;
    uint32_t len;
    uint32_t flags;
};
struct NpLine {
    uint8_t kind;
    uint8_t heading;
    uint8_t quoteDepth;
    uint8_t checked;
    uint32_t markerStart;
    uint32_t markerLen;
    uint32_t contentStart;
    int32_t state;
};
size_t np_md_line(const uint16_t *text, size_t len, int32_t state, NpRun *runs, size_t cap, NpLine *line);
bool np_md_link_at(const uint16_t *text, size_t len, size_t pos, size_t *start, size_t *end);
}

namespace Md
{
// markdown::flags
enum Flag : uint32_t {
    Hidden = 1,
    Pad = 1 << 1,
    Strong = 1 << 2,
    Emph = 1 << 3,
    Strike = 1 << 4,
    Code = 1 << 5,
    Link = 1 << 6,
    Heading = 1 << 7,
    Quote = 1 << 8,
    CodeBlock = 1 << 9,
    Fence = 1 << 10,
    Rule = 1 << 11,
    ListNumber = 1 << 12,
    Done = 1 << 13,
};
// markdown::Kind
enum Kind : uint8_t {
    Blank,
    Paragraph,
    HeadingLine,
    Bullet,
    Numbered,
    Task,
    FenceLine,
    CodeLine,
    RuleLine,
};
} // namespace Md

// What the highlighter learned about a block, for the caret and the drawing.
class BlockInfo : public QTextBlockUserData
{
public:
    NpLine line{};
    // [start, end) of the syntax hidden in the Formatted view, in the block.
    QList<QPair<int, int>> hidden;
    // Where each `>` of a block quote is.
    QList<int> quoteMarks;
    // [start, end) of each misspelled word, when spell check is on.
    QList<QPair<int, int>> misspelled;
    // An opening fence line's language, cleaned for drawing (see
    // MarkdownDecorations::fenceLabel); "" for any other line.
    QString fenceLabel;

    static BlockInfo *of(const QTextBlock &block)
    {
        return static_cast<BlockInfo *>(block.userData());
    }
    // Where a bullet, number or task item's marker starts (the `-` of `- [ ]`).
    int listStart() const
    {
        return line.kind == Md::Task ? int(line.markerStart) - 2 : int(line.markerStart);
    }
};

struct MarkdownStyle {
    bool formatted = true;
    QFont font;
    QString monoFamily;
    QColor text;
    QColor dim;
    QColor link;
    QColor code;
};

class SpellChecker;

// Formats a document's lines. With `markdown` off it only notes
// misspelled words (SpellChecker's highlighter for plain text).
class MarkdownHighlighter : public QSyntaxHighlighter
{
public:
    explicit MarkdownHighlighter(QTextDocument *document, bool markdown = true);

    void setStyle(const MarkdownStyle &style);
    // Notes misspelled words (BlockInfo::misspelled) while it is active.
    // Call rehighlightAll after it changes.
    void setSpellChecker(SpellChecker *spell);
    // Every line again. A big document is done a slice at a time from the
    // event loop, so the window keeps drawing; `now` does it all at once.
    void rehighlightAll(bool now = false);
    const MarkdownStyle &style() const
    {
        return m_style;
    }
    // Where the caret is (a document position). A fence line keeps its
    // markers visible in the Formatted view while the caret is on it; the
    // lines the caret leaves and enters are read again if they are fences.
    void setCaret(int position);
    // Where to ask for the caret when the whole document is read again (the
    // text may have been replaced without the caret's signal firing).
    void setCaretSource(std::function<int()> source)
    {
        m_caretSource = std::move(source);
    }
    bool isCaretBlock(const QTextBlock &block) const;

protected:
    void highlightBlock(const QString &text) override;

private:
    const QTextCharFormat &format(uint32_t flags, int heading);
    int read(const QString &text, int previous, BlockInfo *info, bool caretHere);
    int readBlock(QTextBlock block, int previous, int caretPos);
    void syncCaret();
    int caretBlockPosition() const;
    void rehighlightSlice();

    void addMisspelled(const QString &text, size_t runs, BlockInfo *info);

    MarkdownStyle m_style;
    QHash<quint64, QTextCharFormat> m_formats;
    std::vector<NpRun> m_runs;
    QList<QTextLayout::FormatRange> m_ranges;
    QPointer<SpellChecker> m_spell;
    QTextCursor m_caret; // moves with edits; null until setCaret
    std::function<int()> m_caretSource;
    bool m_markdown;
    // The slices of rehighlightAll: where the next starts (moves with edits;
    // null when none is due) and how long its reading may take.
    QTextCursor m_pass;
    QTimer m_passTimer;
    qint64 m_readBudgetNs = 2'000'000;
    bool m_muted = false; // highlightBlock does nothing (see the constructor)
};

// Attach to a TextEdit (textEdit) to edit Markdown in it.
class MarkdownEditor : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *textEdit READ textEdit WRITE setTextEdit NOTIFY textEditChanged)
    // Formatted (true) or Syntax (false) view.
    Q_PROPERTY(bool formatted READ formatted WRITE setFormatted NOTIFY formattedChanged)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor NOTIFY styleChanged)
    Q_PROPERTY(QColor dimColor READ dimColor WRITE setDimColor NOTIFY styleChanged)
    Q_PROPERTY(QColor linkColor READ linkColor WRITE setLinkColor NOTIFY styleChanged)
    Q_PROPERTY(QColor codeColor READ codeColor WRITE setCodeColor NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)
    // Notes misspelled words for SpellUnderlines while it is active.
    Q_PROPERTY(SpellChecker *spellChecker READ spellChecker WRITE setSpellChecker NOTIFY spellCheckerChanged)

public:
    explicit MarkdownEditor(QObject *parent = nullptr);

    SpellChecker *spellChecker() const
    {
        return m_spell;
    }
    void setSpellChecker(SpellChecker *spell);

    QQuickItem *textEdit() const
    {
        return m_edit;
    }
    void setTextEdit(QQuickItem *edit);
    bool formatted() const
    {
        return m_style.formatted;
    }
    void setFormatted(bool formatted);
    QFont font() const
    {
        return m_style.font;
    }
    void setFont(const QFont &font);
    QColor textColor() const
    {
        return m_style.text;
    }
    void setTextColor(const QColor &c);
    QColor dimColor() const
    {
        return m_style.dim;
    }
    void setDimColor(const QColor &c);
    QColor linkColor() const
    {
        return m_style.link;
    }
    void setLinkColor(const QColor &c);
    QColor codeColor() const
    {
        return m_style.code;
    }
    void setCodeColor(const QColor &c);
    QColor accentColor() const
    {
        return m_accent;
    }

    QTextDocument *document() const
    {
        return m_doc;
    }
    // Where the TextEdit draws the document (its padding).
    QPointF textOrigin() const;
    // Whether a fence line shows its markers: the caret is on it.
    bool fenceRevealed(const QTextBlock &block) const
    {
        return m_highlighter && m_highlighter->isCaretBlock(block);
    }
    // The checkbox of a task item, in document coordinates.
    static QRectF taskBox(QTextDocument *doc, const QTextBlock &block);

    // Toolbar and shortcuts. Each is one step to undo.
    // Wraps the selection in `marker` (**, *, ~~, `), or unwraps it.
    Q_INVOKABLE void toggleInline(const QString &marker);
    // 1-6, or 0 for body text, on every line of the selection.
    Q_INVOKABLE void setHeading(int level);
    // "bullet", "numbered", "task" or "quote", on every line of the selection.
    Q_INVOKABLE void toggleBlock(const QString &kind);
    // The heading level of the caret's line (0 for none), for the toolbar.
    Q_INVOKABLE int headingAt(int position) const;
    // Which formats the caret (start == end) or the selection is in, for the
    // toolbar's highlights: a bit each of Bold, Italic, Code, Strike, and of
    // the line's Bullet, Numbered, Task, Quote. Read from what the highlighter
    // already laid out (no parsing), on the first line of the range.
    enum Format { FmtBold = 1, FmtItalic = 2, FmtCode = 4, FmtStrike = 8, FmtBullet = 16, FmtNumbered = 32, FmtTask = 64, FmtQuote = 128 };
    Q_ENUM(Format)
    Q_INVOKABLE int formatsAt(int start, int end) const;
    // The URL of the link at a position, or "".
    Q_INVOKABLE QString linkAt(int position) const;
    // Takes the Markdown out of the selection: bold, italic, code, links
    // (their text stays) and, on its lines, heading, list and quote prefixes.
    // Code blocks are left alone. With no selection it clears the caret's
    // whole line.
    Q_INVOKABLE void clearFormatting();
    // On a link with no selection: changes its address. Otherwise replaces
    // the selection with [text](url) (text defaults to the url).
    Q_INVOKABLE void insertLink(const QString &text, const QString &url);

Q_SIGNALS:
    void textEditChanged();
    void formattedChanged();
    void styleChanged();
    void spellCheckerChanged();

public:
    // For the bench: highlights the whole document now.
    void rehighlightNow();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void snapCursor();
    void syncCaret();

private:
    void applyStyle();

    int cursor() const;
    int anchor() const;
    bool hasSelection() const;
    bool editable() const;
    void select(int anchor, int position);

    bool hiddenEndingAt(int pos, int *start) const;
    bool hiddenStartingAt(int pos, int *end) const;
    bool hiddenAround(int pos, int *start, int *end) const;
    bool loneSpan(int from, int to, int *start, int *end) const;
    int stepRight(int pos) const;
    int stepLeft(int pos) const;

    bool moveCaret(int direction, bool extend);
    bool backspace();
    bool deleteForward();
    bool newline();
    bool enterInCodeBlock(const QTextBlock &block, const BlockInfo *info);
    bool pressBelowCode(const QPointF &point);
    bool indent(bool outdent);
    bool pressTaskBox(const QPointF &point);
    void toggleTask(const QTextBlock &block);

    QPointer<QQuickItem> m_edit;
    QPointer<QTextDocument> m_doc;
    QPointer<MarkdownHighlighter> m_highlighter;
    QPointer<SpellChecker> m_spell;
    MarkdownStyle m_style;
    QColor m_accent;
    bool m_snapping = false;
    bool m_stylePending = false;
};

// Draws a Formatted view's bullets, checkboxes, quote bars, rules and code
// block backgrounds. Make it a child of the TextEdit with z below it, laid
// over the visible part only (y = the view's contentY, height = the view's).
// They are drawn into one image covering just the shapes, not the view.
class MarkdownDecorations : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(MarkdownEditor *editor READ editor WRITE setEditor NOTIFY editorChanged)

public:
    explicit MarkdownDecorations(QQuickItem *parent = nullptr);

    MarkdownEditor *editor() const
    {
        return m_editor;
    }
    void setEditor(MarkdownEditor *editor);

    // The language shown on an opening fence line ("```rust title=x" gives
    // "rust"): its first word, after an optional "{" (```{r}), cut at the first
    // character that isn't a letter, digit or one of + # . _ - /, and at 64
    // characters, then "…". "" for none.
    static QString fenceLabel(const QString &fenceLine);
    // Test-only (editor_test): the labels the visible part would show, top to
    // bottom. Nothing in the app calls it.
    QStringList labelsForTest() const;

Q_SIGNALS:
    void editorChanged();

protected:
    void updatePolish() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    struct Shape {
        enum Type : uint8_t { Bullet, Box, Quote, Rule, Code, Label } type;
        uint8_t level; // bullets: nesting; boxes: ticked; code: rounded ends (1 top, 2 bottom)
        QRectF rect;
        QString text; // labels: what to draw
        bool operator==(const Shape &o) const
        {
            return type == o.type && level == o.level && rect == o.rect && text == o.text;
        }
    };
    // Elides a label to `width`, remembering the answer (fonts and widths
    // change rarely, polish runs on every scroll).
    struct Elided {
        int width = -1;
        QString text;
        qreal advance = 0;
    };
    const Elided &elide(const QString &label, qreal width) const;
    void watch();
    void layOut(std::vector<Shape> &out) const;
    void paint(QPainter *painter) const;
    static int afterQuotesOf(const QTextBlock &block);

    QPointer<MarkdownEditor> m_editor;
    QPointer<QTextDocument> m_doc;
    std::vector<Shape> m_shapes; // in the document's coordinates
    std::vector<Shape> m_scratch;
    QPointF m_offset; // from the document's coordinates to the item's, for m_shapes
    QImage m_image; // what the node shows (its top left part), kept to be painted again
    QSize m_imageFor; // the item's size in pixels m_image was made for
    mutable QFont m_labelFont;
    mutable qreal m_labelDpr = 1;
    mutable QFontMetricsF m_labelMetrics{QFont()};
    mutable QHash<QString, Elided> m_elided;
};
