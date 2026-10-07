// Line tools for any editable tab (plain, code, Markdown source): duplicate,
// move, delete, join, sort, trim, case. Each call works on the lines [from,
// to) touches (the caret's line when from == to; the whole text for the
// ones marked so) and is ONE edit on the document, so one undo step and one
// layout pass: the changed span of lines is replaced with a single
// QTextCursor operation, never block by block.
#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QtQml/qqmlregistration.h>

class LineTools : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *textEdit READ textEdit WRITE setTextEdit NOTIFY textEditChanged)
    // Tabs to Spaces and Spaces to Leading Tabs count columns with this.
    Q_PROPERTY(int indentWidth READ indentWidth WRITE setIndentWidth NOTIFY indentWidthChanged)

public:
    // sortLines flags, combined.
    enum SortFlag { Descending = 1, CaseInsensitive = 2, Numeric = 4 };
    Q_ENUM(SortFlag)
    // trimSpaces modes.
    enum TrimMode { TrimTrailing, TrimLeading, TrimBoth };
    Q_ENUM(TrimMode)
    // changeCase modes.
    enum CaseMode { UpperCase, LowerCase, TitleCase, SentenceCase, InvertCase };
    Q_ENUM(CaseMode)

    explicit LineTools(QObject *parent = nullptr);

    QQuickItem *textEdit() const { return m_edit; }
    void setTextEdit(QQuickItem *edit);
    int indentWidth() const { return m_width; }
    void setIndentWidth(int width);

    // The selection (or caret) moves with the lines.
    Q_INVOKABLE void duplicateLines(int from, int to);
    Q_INVOKABLE void moveLines(int from, int to, bool down);
    Q_INVOKABLE void deleteLines(int from, int to);
    // The caret's line and the next one when from == to.
    Q_INVOKABLE void joinLines(int from, int to);

    // The whole text when from == to (these seven and the trims).
    Q_INVOKABLE void sortLines(int from, int to, int flags);
    Q_INVOKABLE void reverseLines(int from, int to);
    Q_INVOKABLE void removeDuplicateLines(int from, int to); // keeps the first of each
    Q_INVOKABLE void removeEmptyLines(int from, int to); // empty or blank
    Q_INVOKABLE void trimSpaces(int from, int to, int mode);
    Q_INVOKABLE void tabsToSpaces(int from, int to);
    Q_INVOKABLE void spacesToLeadingTabs(int from, int to);

    // The selected text; the word at the caret when from == to.
    Q_INVOKABLE void changeCase(int from, int to, int mode);

    // Replaces the old lines (first block `first`) with the new ones in one
    // edit; only the span that differs is touched. False when nothing differs.
    static bool applyLines(QTextDocument *doc, const QTextBlock &first, const QStringList &oldLines, const QStringList &newLines);
    // Where a position (before the edit) lands after oldLines became newLines
    // (the same count): a caret or selection can follow an edit.
    static int mapPosition(const QTextBlock &first, const QStringList &oldLines, const QStringList &newLines, int pos);
    static int mapColumn(const QString &oldLine, const QString &newLine, int column);

    // The text in the given case (CaseMode); paragraph breaks may be U+2029.
    static QString convertCase(const QString &text, int mode);

Q_SIGNALS:
    void textEditChanged();
    void indentWidthChanged();

private:
    bool editable() const;
    // The blocks [from, to) touches; all of them when whole and from == to.
    bool lineRange(int from, int to, bool whole, QTextBlock *first, QTextBlock *last) const;
    QStringList textOf(const QTextBlock &first, const QTextBlock &last) const;
    // Runs a whole-or-selected-lines transform; the lines it leaves are selected.
    template<typename F>
    void transform(int from, int to, bool whole, F &&fn);
    void select(int start, int end);

    QPointer<QQuickItem> m_edit;
    QPointer<QTextDocument> m_doc;
    int m_width = 4;
};
