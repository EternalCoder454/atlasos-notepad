// Starts Qt, picks the Qt Quick backend and loads the window. The Markdown
// reading is in Rust (src/); this file and the others in cpp/ are the glue.
#include "app.h"
#include "bench.h"
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

int main(int argc, char *argv[])
{
    // As Atlas Monitor (see its main.cpp): no thread hand-off for the raster
    // engine's fills, and partial repaints at fractional scales, which
    // otherwise repaint the whole window on every keystroke.
    if (qEnvironmentVariableIsEmpty("QT_NO_GUI_THREADPOOL")) {
        qputenv("QT_NO_GUI_THREADPOOL", "1");
    }
    if (qEnvironmentVariableIsEmpty("QSG_SOFTWARE_RENDERER_FORCE_PARTIAL_UPDATES")) {
        qputenv("QSG_SOFTWARE_RENDERER_FORCE_PARTIAL_UPDATES", "1");
    }
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
        notepad.start({parser.value(benchOption)});
        DocumentList *list = notepad.windows().value(0);
        Document *doc = list ? list->current() : nullptr;
        if (!doc) {
            return 2;
        }
        // The window is ready when the file is read and its TextEdit exists.
        auto *poll = new QTimer(&app);
        auto *deadline = new QElapsedTimer;
        deadline->start();
        const QString file = parser.value(benchOption);
        QObject::connect(poll, &QTimer::timeout, &app, [&, poll, deadline, doc, file] {
            if (deadline->elapsed() > 30000) {
                fprintf(stderr, "atlas-notepad: the window never got its editor\n");
                QCoreApplication::exit(1);
                return;
            }
            QQuickItem *edit = doc->textEdit();
            QQuickWindow *window = notepad.activeWindow();
            if (doc->isLoading() || !edit || !window) {
                return;
            }
            poll->stop();
            auto *view = window->findChild<QQuickItem *>(QStringLiteral("view"));
            MarkdownEditor *editor = nullptr;
            for (auto *candidate : window->findChildren<MarkdownEditor *>()) {
                if (candidate->textEdit() == edit) {
                    editor = candidate;
                }
            }
            if (!view || !editor) {
                fprintf(stderr, "atlas-notepad: the window has no editor\n");
                QCoreApplication::exit(1);
                return;
            }
            // The baseline: the same TextEdit without Markdown.
            if (qEnvironmentVariableIntValue("NP_BENCH_PLAIN")) {
                editor->setTextEdit(nullptr);
            }
            (new Bench(window, edit, view, editor, file))->start();
        });
        poll->start(20);
    } else {
        notepad.start(parser.positionalArguments());
    }
    return app.exec();
}
