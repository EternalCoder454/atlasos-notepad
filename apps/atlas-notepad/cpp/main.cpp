// Starts Qt, picks the Qt Quick backend and loads the window. The Markdown
// reading is in Rust (src/); this file and the others in cpp/ are the glue.
#include "bench.h"
#include "markdown.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <cstdio>

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
    // Draw on the CPU: for a window of text, the GPU path costs tens of MiB
    // (Mesa, LLVM) and start-up time for nothing. QT_QUICK_BACKEND overrides.
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
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
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("A file to open."));
    parser.process(app);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("net.eterneon.atlas.notepad"), QStringLiteral("Main"));
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (!window) {
        return 1;
    }
    auto *edit = window->findChild<QQuickItem *>(QStringLiteral("editor"));
    auto *view = window->findChild<QQuickItem *>(QStringLiteral("view"));
    auto *editor = window->findChild<MarkdownEditor *>();
    if (!edit || !view || !editor) {
        fprintf(stderr, "atlas-notepad: the window has no editor\n");
        return 1;
    }

    if (parser.isSet(benchOption)) {
        // The baseline: the same TextEdit without Markdown.
        if (qEnvironmentVariableIntValue("NP_BENCH_PLAIN")) {
            editor->setTextEdit(nullptr);
        }
        (new Bench(window, edit, view, editor, parser.value(benchOption)))->start();
    } else if (!parser.positionalArguments().isEmpty()) {
        // Spike: UTF-8 only. Phase 1 brings encodings, line endings and saving.
        QFile f(parser.positionalArguments().first());
        if (f.open(QIODevice::ReadOnly)) {
            edit->setProperty("text", QString::fromUtf8(f.readAll()));
        }
    }
    return app.exec();
}
