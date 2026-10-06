// Starts Qt, picks the Qt Quick backend and loads the window. The Markdown
// reading is in Rust (src/); this file and the others in cpp/ are the glue.
#include "app.h"
#include "bench.h"
#include "codeeditor.h"
#include "markdown.h"

#include <KDBusService>

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <unistd.h>

// Asked to quit by a signal (SIGTERM from kill or systemd, SIGINT, SIGHUP):
// the session is saved and the app quits. The handler only writes to a pipe;
// the event loop does the rest. A second signal kills as before
// (SA_RESETHAND), in case quitting itself hangs.
static int s_quitPipe[2] = {-1, -1};

static void onQuitSignal(int)
{
    const int saved = errno;
    const char b = 1;
    [[maybe_unused]] const ssize_t n = ::write(s_quitPipe[1], &b, 1);
    errno = saved;
}

static void quitOnSignals(App *notepad)
{
    if (::pipe2(s_quitPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        return;
    }
    auto *notifier = new QSocketNotifier(s_quitPipe[0], QSocketNotifier::Read, notepad);
    QObject::connect(notifier, &QSocketNotifier::activated, notepad, [notepad] {
        char buf[16];
        while (::read(s_quitPipe[0], buf, sizeof buf) > 0) {
        }
        notepad->saveSession();
        QCoreApplication::quit();
    });
    struct sigaction sa = {};
    sa.sa_handler = onQuitSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_RESETHAND;
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) {
        sigaction(sig, &sa, nullptr);
    }
}

// Crash reports (src/crash.rs): saved only if the user turned them on in
// Atlas Updater.
extern "C" void atlas_crash_install();
extern "C" void atlas_crash_fatal(const char *msg);

static QtMessageHandler s_previousHandler = nullptr;

static void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (type == QtFatalMsg) {
        atlas_crash_fatal(msg.toUtf8().constData());
    }
    if (s_previousHandler) {
        s_previousHandler(type, context, msg);
    } else {
        // Qt's default handler isn't returned by qInstallMessageHandler: print
        // the message ourselves so warnings and fatal errors aren't lost.
        fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, msg)));
        fflush(stderr);
    }
}

int main(int argc, char *argv[])
{
    atlas_crash_install(); // Rust panic hook, before anything can panic.
    s_previousHandler = qInstallMessageHandler(messageHandler);
    // As Atlas Monitor (see its main.cpp): no thread hand-off for the raster
    // engine's fills.
    if (qEnvironmentVariableIsEmpty("QT_NO_GUI_THREADPOOL")) {
        qputenv("QT_NO_GUI_THREADPOOL", "1");
    }
    // Partial repaints are left to Qt, which turns them off at a fractional
    // scale. They used to be forced on (QSG_SOFTWARE_RENDERER_FORCE_PARTIAL_UPDATES)
    // so a keystroke at 1.5x didn't repaint the whole window, but the window's
    // last device pixel row and column are then only partly covered and never
    // cleared. Notepad's frameless window is see-through (blur, and its
    // rounded corners), so nothing can cover them: on a 4K screen at 1.5x the
    // window's border was seen left behind while it was dragged, until a full
    // repaint.
    // Draw on the CPU unless Settings.gpuRendering is on: for a window of text, the GPU path costs tens of MiB
    // (Mesa, LLVM) and start-up time for nothing. QT_QUICK_BACKEND overrides.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND") && !Settings::readGpuRendering()) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    QApplication app(argc, argv);
    // Together these give the app ID net.eterneon.atlas.notepad.
    QApplication::setOrganizationDomain(QStringLiteral("atlas.eterneon.net"));
    QApplication::setApplicationName(QStringLiteral("notepad"));
    QApplication::setApplicationDisplayName(QStringLiteral("Notepad"));
    QApplication::setApplicationVersion(QStringLiteral(ATLAS_NOTEPAD_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("net.eterneon.atlas.notepad"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("accessories-text-editor")));
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption benchOption(QStringLiteral("bench"), QStringLiteral("Time typing and scrolling in <file>, print the figures and quit."),
                                        QStringLiteral("file"));
    parser.addOption(benchOption);
    // Handled by App::activate when another Notepad is running; a first
    // launch opens a window anyway.
    parser.addOption(QCommandLineOption(QStringLiteral("new-window"), QStringLiteral("Open a new window.")));
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Files to open."), QStringLiteral("[file...]"));
    parser.process(app);
    const bool bench = parser.isSet(benchOption);

    // One instance per session: a second launch asks this one to open its
    // files (activateRequested) and exits. Not for --bench.
    std::optional<KDBusService> service;
    if (!bench) {
        service.emplace(KDBusService::Unique);
    }

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    App notepad(&engine);
    if (service) {
        QObject::connect(&*service, &KDBusService::activateRequested, &notepad, &App::activate);
        quitOnSignals(&notepad);
    }
    if (bench) {
        notepad.setSessionEnabled(false);
        auto *deadline = new QElapsedTimer;
        deadline->start();
        notepad.start({parser.value(benchOption)});
        DocumentList *list = notepad.windows().value(0);
        Document *doc = list ? list->current() : nullptr;
        if (!doc) {
            return 2;
        }
        // Opening as the user sees it: the first frame with text, the frame
        // with all of it, and the longest the event loop was blocked once
        // the window was up (a 1 ms timer that should tick on time).
        // NP_BENCH_OPEN_ONLY=1 adds the longest in the next 2 s and stops.
        struct Open {
            qint64 firstFrame = -1, firstText = -1, lastTick = -1, longest = 0;
        };
        auto *open = new Open;
        if (qEnvironmentVariableIntValue("NP_BENCH_OPEN_ONLY")) {
            QTimer::singleShot(60000, &app, [] {
                fprintf(stderr, "atlas-notepad: the file never showed\n");
                QCoreApplication::exit(1);
            });
        }
        auto *tick = new QTimer(&app);
        tick->setTimerType(Qt::PreciseTimer);
        tick->setInterval(1);
        QObject::connect(tick, &QTimer::timeout, &app, [open, deadline] {
            const qint64 now = deadline->nsecsElapsed();
            if (open->lastTick >= 0) {
                open->longest = qMax(open->longest, now - open->lastTick);
            }
            open->lastTick = now;
        });
        tick->start();
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
            auto *once = new QObject(window);
            QObject::connect(window, &QQuickWindow::frameSwapped, once, [once, open, tick, deadline, doc] {
                const qint64 now = deadline->nsecsElapsed();
                if (open->firstFrame < 0) {
                    open->firstFrame = now;
                    open->lastTick = now; // start-up isn't counted
                    open->longest = 0;
                }
                QQuickItem *edit = doc->textEdit();
                if (open->firstText < 0 && edit && edit->property("length").toInt() > 0) {
                    open->firstText = now;
                }
                if (open->firstText < 0 || doc->isLoading()) {
                    return;
                }
                open->longest = qMax(open->longest, now - open->lastTick); // a stall this frame ended
                printf("open: window %.1f ms, first text %.1f ms, all text %.1f ms after start; longest stall %.1f ms\n", double(open->firstFrame) / 1e6,
                       double(open->firstText) / 1e6, double(now) / 1e6, double(open->longest) / 1e6);
                fflush(stdout);
                delete once;
                if (!qEnvironmentVariableIntValue("NP_BENCH_OPEN_ONLY")) {
                    tick->stop(); // it would wake the typing bench
                    return;
                }
                // And what follows the text (highlighting, spell check).
                open->longest = 0;
                QTimer::singleShot(2000, tick, [open, tick] {
                    printf("open: longest stall in the 2 s after %.1f ms\n", double(open->longest) / 1e6);
                    fflush(stdout);
                    tick->stop();
                    QCoreApplication::exit(0);
                });
            });
        }
        auto *poll = new QTimer(&app);
        const QString file = parser.value(benchOption);
        QObject::connect(poll, &QTimer::timeout, &app, [&, poll, deadline, doc, file] {
            // The window is ready when the file is read and its TextEdit, view
            // and MarkdownEditor exist. They are found up from the TextEdit:
            // what a Loader makes isn't a QObject child of the window.
            QQuickItem *edit = doc->textEdit();
            QQuickWindow *window = edit ? edit->window() : nullptr;
            QQuickItem *view = nullptr;
            MarkdownEditor *editor = nullptr;
            CodeEditor *code = nullptr;
            for (QQuickItem *item = edit; item; item = item->parentItem()) {
                if (!view && item->objectName() == QLatin1String("view")) {
                    view = item;
                }
                for (auto *candidate : item->findChildren<MarkdownEditor *>(Qt::FindDirectChildrenOnly)) {
                    // Not attached when the file opens as plain text (over
                    // Limits::formattedBytes, or not Markdown).
                    if (candidate->textEdit() == edit || (!candidate->textEdit() && !doc->isMarkdown())) {
                        editor = candidate;
                    }
                }
                for (auto *candidate : item->findChildren<CodeEditor *>(Qt::FindDirectChildrenOnly)) {
                    if (candidate->textEdit() == edit) {
                        code = candidate;
                    }
                }
            }
            const bool isCode = doc->property("code").toBool();
            if (isCode ? !code : !editor) {
                editor = nullptr; // not ready: a code file waits for its CodeEditor
            }
            if (doc->isLoading() || !view || (!editor && !code)) {
                if (deadline->elapsed() > 30000) {
                    fprintf(stderr, "atlas-notepad: the window never got its %s\n", !edit ? "TextEdit" : !view ? "view" : "MarkdownEditor");
                    QCoreApplication::exit(1);
                    poll->stop();
                }
                return;
            }
            poll->stop();
            if (qEnvironmentVariableIntValue("NP_BENCH_OPEN_ONLY")) {
                return;
            }
            // The baseline: the same TextEdit without Markdown.
            if (qEnvironmentVariableIntValue("NP_BENCH_PLAIN")) {
                if (isCode) {
                    code->setTextEdit(nullptr);
                } else {
                    editor->setTextEdit(nullptr);
                }
            }
            (new Bench(window, edit, view, isCode ? nullptr : editor, isCode ? code : nullptr, file))->start();
        });
        poll->start(20);
    } else {
        notepad.start(parser.positionalArguments());
    }
    return app.exec();
}
