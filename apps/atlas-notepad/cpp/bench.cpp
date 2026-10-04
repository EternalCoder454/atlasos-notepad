#include "bench.h"
#include "codeeditor.h"
#include "markdown.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>
#include <cstdio>

namespace
{
const char *const groupNames[] = {"type", "enter", "backspace", "arrows", "scroll"};
enum Group { Type, Enter, Erase, Arrows, Scroll, Groups };
// A step that types its text (sent with key 0, as an input method would).
constexpr int TextKey = -1;
}

Bench::Bench(QQuickWindow *window, QQuickItem *edit, QQuickItem *view, MarkdownEditor *editor, CodeEditor *code, const QString &file)
    : QObject(window)
    , m_window(window)
    , m_edit(edit)
    , m_view(view)
    , m_editor(editor)
    , m_code(code)
    , m_file(file)
    , m_total(Groups)
    , m_handle(Groups)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(1000);
    connect(&m_timeout, &QTimer::timeout, this, &Bench::timedOut);
    connect(window, &QQuickWindow::frameSwapped, this, &Bench::frame);

    // A sentence with the usual inline syntax, typed into the middle of the
    // document, four times, a line each; then some erasing and caret moves.
    // A code file gets a line of C++ instead.
    const QString sentence = code ? QStringLiteral("total += compute(values[i], 42) * scale; // keep \"quoted\" text ")
                                  : QStringLiteral("The quick **brown** fox jumps over the _lazy_ dog, see `code` and [a link](https://example.org). ");
    for (int r = 0; r < 4; ++r) {
        for (const QChar ch : sentence) {
            m_steps.push_back({Type, TextKey, QString(ch)});
        }
        m_steps.push_back({Enter, Qt::Key_Return, QStringLiteral("\r")});
    }
    for (int i = 0; i < 30; ++i) {
        m_steps.push_back({Erase, Qt::Key_Backspace, QString()});
    }
    for (int i = 0; i < 30; ++i) {
        m_steps.push_back({Arrows, i % 2 ? Qt::Key_Right : Qt::Key_Left, QString()});
    }
    // From the top: ten pages down and back.
    m_steps.push_back({Scroll, 0, QStringLiteral("0")});
    for (int i = 0; i < 20; ++i) {
        m_steps.push_back({Scroll, 0, i < 10 ? QStringLiteral("+") : QStringLiteral("-")});
    }
}

QTextDocument *Bench::document() const
{
    auto *textDocument = qobject_cast<QQuickTextDocument *>(m_edit->property("textDocument").value<QObject *>());
    return textDocument->textDocument();
}

void Bench::start()
{
    // A blinking caret would add frames of its own.
    QGuiApplication::styleHints()->setCursorFlashTime(0);
    m_clock.start();
    // The file is already open (main.cpp times that): let the delayed first
    // highlight and layout run, then start.
    QTimer::singleShot(800, this, [this] {
        const qint64 t = m_clock.nsecsElapsed();
        if (m_editor) {
            m_editor->rehighlightNow();
        } else if (m_code) {
            m_code->rehighlightNow();
        }
        m_highlightMs = double(m_clock.nsecsElapsed() - t) / 1e6;
        QTextDocument *doc = document();
        const QTextBlock middle = doc->findBlockByNumber(doc->blockCount() / 2);
        m_edit->forceActiveFocus();
        m_edit->setProperty("cursorPosition", middle.position() + middle.length() - 1);
        QTimer::singleShot(500, this, &Bench::next);
    });
}

void Bench::frame()
{
    const qint64 now = m_clock.nsecsElapsed();
    if (m_sent < 0) {
        return;
    }
    const Step &step = m_steps[m_next - 1];
    m_total[step.group].push_back(double(now - m_sent) / 1e6);
    m_sent = -1;
    m_timeout.stop();
    QTimer::singleShot(4, this, &Bench::next);
}

void Bench::timedOut()
{
    ++m_missed;
    m_sent = -1;
    next();
}

void Bench::next()
{
    if (m_next >= m_steps.size()) {
        finish();
        return;
    }
    const Step &step = m_steps[m_next++];
    const qint64 t0 = m_clock.nsecsElapsed();
    m_sent = t0;
    if (step.key == 0) {
        const qreal page = m_view->height();
        const qreal y = step.text == u"0" ? 0 : m_view->property("contentY").toReal() + (step.text == u"+" ? page : -page);
        const qreal max = qMax<qreal>(0, m_view->property("contentHeight").toReal() - page);
        m_view->setProperty("contentY", qBound<qreal>(0, y, max));
    } else {
        const int key = step.key == TextKey ? 0 : step.key;
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, step.text);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, step.text);
        QCoreApplication::sendEvent(m_edit, &press);
        QCoreApplication::sendEvent(m_edit, &release);
    }
    m_handle[step.group].push_back(double(m_clock.nsecsElapsed() - t0) / 1e6);
    m_timeout.start();
}

static void line(const char *name, std::vector<double> v, const std::vector<double> &handle)
{
    if (v.empty()) {
        printf("%-10s no frames\n", name);
        return;
    }
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) {
        sum += x;
    }
    double hsum = 0;
    for (double x : handle) {
        hsum += x;
    }
    auto at = [&](double q) {
        return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
    };
    printf("%-10s n=%-4zu mean=%6.3f  p50=%6.3f  p95=%6.3f  max=%6.3f ms   (event alone: mean %.3f ms)\n", name, v.size(), sum / double(v.size()),
           at(0.5), at(0.95), v.back(), handle.empty() ? 0 : hsum / double(handle.size()));
}

void Bench::finish()
{
    QTextDocument *doc = document();
    printf("file: %s (%d characters, %d lines), window %dx%d at %.2fx\n", qPrintable(m_file), doc->characterCount(), doc->blockCount(),
           m_window->width(), m_window->height(), m_window->effectiveDevicePixelRatio());
    printf("highlight all: %.1f ms\n", m_highlightMs);
    for (int g = 0; g < Groups; ++g) {
        line(groupNames[g], m_total[g], m_handle[g]);
    }
    if (m_missed) {
        printf("steps without a frame: %d\n", m_missed);
    }
    QFile status(QStringLiteral("/proc/self/status"));
    if (status.open(QIODevice::ReadOnly)) {
        for (const QByteArray &l : status.readAll().split('\n')) {
            if (l.startsWith("VmRSS") || l.startsWith("VmHWM")) {
                printf("%s\n", l.simplified().constData());
            }
        }
    }
    fflush(stdout);
    QCoreApplication::exit(0);
}
