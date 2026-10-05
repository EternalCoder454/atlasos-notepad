// Light coding: a CodeEditor attached to a TextEdit highlights it with
// KSyntaxHighlighting and edits like a code editor (auto-indent, Tab and
// Backtab, comment toggling, line tools). Every edit goes through
// QTextCursor, so each is one step on the document's undo stack.
#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QQuickItem>
#include <QStringList>
#include <QTextDocument>
#include <QtQml/qqmlregistration.h>

#include <KSyntaxHighlighting/Definition>

namespace KSyntaxHighlighting
{
class Repository;
class SyntaxHighlighter;
}

// The one shared definition repository, built on first use (main thread).
KSyntaxHighlighting::Repository &codeRepository();

class CodeEditor : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *textEdit READ textEdit WRITE setTextEdit NOTIFY textEditChanged)
    // A KSyntaxHighlighting definition name; "" is no highlighting.
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY darkChanged)
    Q_PROPERTY(bool insertSpaces READ insertSpaces WRITE setInsertSpaces NOTIFY insertSpacesChanged)
    Q_PROPERTY(int indentWidth READ indentWidth WRITE setIndentWidth NOTIFY indentWidthChanged)

public:
    explicit CodeEditor(QObject *parent = nullptr);
    ~CodeEditor() override;

    QQuickItem *textEdit() const { return m_edit; }
    void setTextEdit(QQuickItem *edit);
    QString language() const { return m_language; }
    void setLanguage(const QString &name);
    bool dark() const { return m_dark; }
    void setDark(bool dark);
    bool insertSpaces() const { return m_spaces; }
    void setInsertSpaces(bool on);
    int indentWidth() const { return m_width; }
    void setIndentWidth(int width);

    // Each works on the lines touched by [from, to) and is one undo step.
    Q_INVOKABLE void toggleComment(int from, int to);
    Q_INVOKABLE void indentLines(int from, int to);
    Q_INVOKABLE void outdentLines(int from, int to);
    // The bracket just before or at cursor and its match: (-1, -1) if none.
    Q_INVOKABLE QPoint bracketPair(int cursor);
    Q_INVOKABLE QString commentMarker() const;
    Q_INVOKABLE static QStringList languages();
    // Highlights the whole document again, now (the bench times it).
    void rehighlightNow();

Q_SIGNALS:
    void textEditChanged();
    void languageChanged();
    void darkChanged();
    void insertSpacesChanged();
    void indentWidthChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool editable() const;
    QString indentUnit(int column) const;
    bool newline();
    bool tab();
    bool closingBracket(const QString &text);
    void applyHighlighting();
    void firstLast(int from, int to, QTextBlock *first, QTextBlock *last) const;
    void replaceLines(const QTextBlock &first, const QStringList &old, const QStringList &lines);

    QPointer<QQuickItem> m_edit;
    QPointer<QTextDocument> m_doc;
    QPointer<KSyntaxHighlighting::SyntaxHighlighter> m_highlighter;
    QString m_language;
    KSyntaxHighlighting::Definition m_def;
    bool m_dark = false;
    bool m_spaces = true;
    int m_width = 4;
};
