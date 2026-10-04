// The Formatted view: Markdown kept as the document's text and drawn
// formatted. Rust reads each line (crates/notepad-core/src/markdown.rs); the
// highlighter here turns that into formats, MarkdownEditor keeps the caret
// out of hidden syntax and does the toolbar's edits, and MarkdownDecorations
// draws what text can't show: bullets, checkboxes, quote bars, rules and
// code block backgrounds.
#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickTextDocument>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBlockUserData>
#include <QtQml/qqmlregistration.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Rust, see apps/atlas-notepad/src/lib.rs. Positions count UTF-16 units.
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

class MarkdownHighlighter : public QSyntaxHighlighter
{
public:
    explicit MarkdownHighlighter(QTextDocument *document);

    void setStyle(const MarkdownStyle &style);
    void rehighlightAll();
    const MarkdownStyle &style() const
    {
        return m_style;
    }

protected:
    void highlightBlock(const QString &text) override;

private:
    const QTextCharFormat &format(uint32_t flags, int heading);
    int read(const QString &text, int previous, BlockInfo *info);

    MarkdownStyle m_style;
    QHash<quint64, QTextCharFormat> m_formats;
    std::vector<NpRun> m_runs;
    QList<QTextLayout::FormatRange> m_ranges;
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

public:
    explicit MarkdownEditor(QObject *parent = nullptr);

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
    // The URL of the link at a position, or "".
    Q_INVOKABLE QString linkAt(int position) const;
    // Takes the Markdown out of the selection: bold, italic, code, links
    // (their text stays) and, on its lines, heading, list and quote prefixes.
    // Code blocks are left alone.
    Q_INVOKABLE void clearFormatting();
    // On a link with no selection: changes its address. Otherwise replaces
    // the selection with [text](url) (text defaults to the url).
    Q_INVOKABLE void insertLink(const QString &text, const QString &url);

Q_SIGNALS:
    void textEditChanged();
    void formattedChanged();
    void styleChanged();

public:
    // For the bench: highlights the whole document now.
    void rehighlightNow();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void snapCursor();

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
    bool indent(bool outdent);
    bool pressTaskBox(const QPointF &point);
    void toggleTask(const QTextBlock &block);

    QPointer<QQuickItem> m_edit;
    QPointer<QTextDocument> m_doc;
    QPointer<MarkdownHighlighter> m_highlighter;
    MarkdownStyle m_style;
    QColor m_accent;
    bool m_snapping = false;
    bool m_stylePending = false;
};

// Draws a Formatted view's bullets, checkboxes, quote bars, rules and code
// block backgrounds. Make it a child of the TextEdit with z below it, laid
// over the visible part only (y = the view's contentY, height = the view's):
// sized to the whole document, its image would be as tall.
class MarkdownDecorations : public QQuickPaintedItem
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

    void paint(QPainter *painter) override;

Q_SIGNALS:
    void editorChanged();

protected:
    void updatePolish() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Shape {
        enum Type : uint8_t { Bullet, Box, Quote, Rule, Code } type;
        uint8_t level; // bullets: nesting; boxes: ticked; code: rounded ends (1 top, 2 bottom)
        QRectF rect;
        bool operator==(const Shape &o) const
        {
            return type == o.type && level == o.level && rect == o.rect;
        }
    };
    void watch();
    void layOut(std::vector<Shape> &out) const;
    static int afterQuotesOf(const QTextBlock &block);

    QPointer<MarkdownEditor> m_editor;
    QPointer<QTextDocument> m_doc;
    std::vector<Shape> m_shapes;
    std::vector<Shape> m_scratch;
};
