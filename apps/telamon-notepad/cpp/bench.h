// `telamon-notepad --bench FILE`: opens FILE, types into the middle of it and
// scrolls through it, timing each key from the event to the frame on screen
// (QQuickWindow::frameSwapped), then prints the figures and quits.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTimer>

#include <vector>

class CodeEditor;
class MarkdownEditor;
class QTextDocument;

class Bench : public QObject
{
public:
    Bench(QQuickWindow *window, QQuickItem *edit, QQuickItem *view, MarkdownEditor *editor, CodeEditor *code, const QString &file);
    void start();

private:
    struct Step {
        int group;
        int key; // 0: scroll a page down (+1) or up (-1) instead, in `text`
        QString text;
    };
    void next();
    void frame();
    void timedOut();
    void finish();
    QTextDocument *document() const;

    QPointer<QQuickWindow> m_window;
    QPointer<QQuickItem> m_edit;
    QPointer<QQuickItem> m_view;
    QPointer<MarkdownEditor> m_editor; // null for a code file
    QPointer<CodeEditor> m_code; // null for anything else
    QString m_file;
    QElapsedTimer m_clock;
    QTimer m_timeout;
    std::vector<Step> m_steps;
    size_t m_next = 0;
    qint64 m_sent = -1; // when the step being timed was sent; -1: none
    double m_highlightMs = 0;
    int m_missed = 0;
    std::vector<std::vector<double>> m_total;  // per group: event to frame
    std::vector<std::vector<double>> m_handle; // per group: the event alone
};
