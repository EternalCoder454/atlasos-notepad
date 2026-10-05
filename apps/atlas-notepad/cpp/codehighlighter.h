// Highlights a code document with KSyntaxHighlighting, in linear time. The
// stock KSyntaxHighlighting::SyntaxHighlighter goes through QSyntaxHighlighter
// block by block, and each block tells the layout, which walks the document
// from the top: quadratic (a 1 MB file took 90 s to highlight again), and a
// state change (typing `/*`) is passed on one queued block at a time, each a
// walk. This one sets the same formats straight on the blocks and tells the
// layout once per slice. See docs/DESIGN.md, Performance.
#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBlockUserData>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextLayout>
#include <QTimer>

#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

#include <QHash>
#include <QList>

// What a block was highlighted with: the state it started in and ended in.
class CodeBlockData : public QTextBlockUserData
{
public:
    KSyntaxHighlighting::State in;
    KSyntaxHighlighting::State end;
};

// QSyntaxHighlighter is only the hook into the document (its own highlightBlock
// does nothing): the formats are set from the document's contentsChange
// signal and from the slices of a pass over the whole document.
class CodeHighlighter : public QSyntaxHighlighter, public KSyntaxHighlighting::AbstractHighlighter
{
public:
    explicit CodeHighlighter(QTextDocument *document);
    ~CodeHighlighter() override;

    // Both highlight the document again (a big one in slices).
    void setDefinition(const KSyntaxHighlighting::Definition &def) override;
    void setTheme(const KSyntaxHighlighting::Theme &theme) override;
    // Every line again. A big document is done a slice at a time from the
    // event loop, so the window keeps drawing; `now` does it all at once.
    // (Not rehighlight(): that would clear the formats.)
    void rehighlightAll(bool now = false);

protected:
    void highlightBlock(const QString &text) override;
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override;

private:
    struct Style {
        QTextCharFormat format;
        bool plain; // nothing to set: the same as no format
    };

    void documentChanged(int from, int removed, int added);
    const Style &style(const KSyntaxHighlighting::Format &format);
    KSyntaxHighlighting::State highlightOne(QTextBlock block, const KSyntaxHighlighting::State &in, bool *changed);
    QTextBlock run(QTextBlock block, bool all, int through, qint64 budgetNs, int *from, int *end);
    void flushDirty(bool force);
    void schedule(const QTextBlock &block, bool all);
    void runPass();

    QHash<int, Style> m_styles;
    QList<QTextLayout::FormatRange> m_ranges;
    // The slices of a pass over the rest of the document: where the next
    // starts (moves with edits; null when none is due), whether it goes to the
    // end (otherwise it stops where a line starts in the state it had), and
    // how long its reading may take.
    QTextCursor m_pass;
    bool m_all = false;
    QTimer m_passTimer;
    qint64 m_readBudgetNs = 2'000'000;
    // Formats set by slices the layout hasn't been told of (see flushDirty).
    int m_dirtyFrom = -1;
    int m_dirtyEnd = 0;
    QElapsedTimer m_flushClock;
    qint64 m_flushCostNs = 0;
};
