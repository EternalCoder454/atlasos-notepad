// The process: settings, recent files, the windows and the session.
#include "app.h"

#include "document_p.h"
#include "session.h"

#include <KWindowSystem>

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLocale>
#include <QPrintDialog>
#include <QPrinter>
#include <QPrinterInfo>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTextDocument>
#include <QTimer>

#include <memory>

namespace
{
constexpr int recentLimit = 10;
constexpr auto menuService = "com.canonical.AppMenu.Registrar";
App *s_instance = nullptr;

// `rect` moved and shrunk onto the screen it overlaps most (the primary one if
// it overlaps none), so a restored window can't end up off the desktop.
QRect clampToScreens(const QRect &rect)
{
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty() || !rect.isValid()) {
        return rect;
    }
    QScreen *best = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen() : screens.first();
    qint64 bestArea = 0;
    for (QScreen *screen : screens) {
        const QRect i = screen->availableGeometry().intersected(rect);
        const qint64 area = qint64(qMax(0, i.width())) * qMax(0, i.height());
        if (area > bestArea) {
            bestArea = area;
            best = screen;
        }
    }
    const QRect avail = best->availableGeometry();
    QRect out = rect;
    out.setSize(rect.size().boundedTo(avail.size()));
    out.moveLeft(qBound(avail.left(), out.left(), avail.right() + 1 - out.width()));
    out.moveTop(qBound(avail.top(), out.top(), avail.bottom() + 1 - out.height()));
    return out;
}

struct Window {
    DocumentList *list = nullptr;
    QPointer<QQuickWindow> window;
    QRect geometry;
    bool maximized = false;
};
}

struct App::Private {
    App *q = nullptr;
    QQmlApplicationEngine *engine = nullptr;
    Settings *settings = nullptr;
    Session session;
    bool sessionEnabled = true;
    bool quitting = false;
    bool exiting = false; // quitting for good: never reset
    bool quitInProgress = false; // quit() without a session, waiting on a dialog
    DocumentList *quitWaitingOn = nullptr;
    QList<QPointer<QQuickWindow>> closed; // closed windows not yet destroyed
    QList<std::shared_ptr<Window>> windows; // most recent first
    QTimer saveTimer; // quiet period
    QTimer maxTimer; // longest an unsaved change waits
    bool hasMenu = false;

    std::shared_ptr<Window> windowOf(const DocumentList *list) const
    {
        for (const auto &w : windows) {
            if (w->list == list) {
                return w;
            }
        }
        return nullptr;
    }

    // The stored list, whether the files exist now or not (an unmounted
    // drive keeps its entries); only the display filters.
    QStringList readRecent() const
    {
        QStringList out;
        settings->rc().beginGroup(QStringLiteral("Recent"));
        for (int i = 0; i < recentLimit; ++i) {
            const QString p = settings->rc().value(QString::number(i)).toString();
            if (!p.isEmpty()) {
                out.append(p);
            }
        }
        settings->rc().endGroup();
        return out;
    }

    void writeRecent(const QStringList &paths)
    {
        QSettings &rc = settings->rc();
        rc.remove(QStringLiteral("Recent"));
        rc.beginGroup(QStringLiteral("Recent"));
        for (int i = 0; i < paths.size(); ++i) {
            rc.setValue(QString::number(i), paths.at(i));
        }
        rc.endGroup();
        rc.sync();
    }

    bool sessionOn() const
    {
        return sessionEnabled && settings->continueSession();
    }

    void write(bool wait)
    {
        if (!sessionEnabled) {
            return;
        }
        if (!settings->continueSession()) {
            session.remove();
            return;
        }
        QList<Session::Live> live;
        for (const auto &w : windows) {
            live.append({w->list, w->geometry, w->maximized});
        }
        session.write(live, wait);
    }

    void scheduleSave()
    {
        if (sessionOn()) {
            saveTimer.start();
            if (!maxTimer.isActive()) {
                maxTimer.start();
            }
        }
    }

    void stopTimers()
    {
        saveTimer.stop();
        maxTimer.stop();
    }

    void connectDocument(Document *doc)
    {
        for (auto signal : {&Document::edited, &Document::modifiedChanged, &Document::pathChanged, &Document::encodingChanged, &Document::lineEndingChanged,
                            &Document::formattedChanged, &Document::saved, &Document::viewStateChanged}) {
            QObject::connect(doc, signal, q, [this] { scheduleSave(); });
        }
    }

    void watchList(DocumentList *list)
    {
        for (Document *doc : list->documents()) {
            connectDocument(doc);
        }
        QObject::connect(list, &QAbstractItemModel::rowsInserted, q, [this, list](const QModelIndex &, int first, int last) {
            const auto docs = list->documents();
            for (int i = first; i <= last && i < docs.size(); ++i) {
                connectDocument(docs.at(i));
            }
            scheduleSave();
        });
        QObject::connect(list, &QAbstractItemModel::rowsRemoved, q, [this] { scheduleSave(); });
        QObject::connect(list, &QAbstractItemModel::rowsMoved, q, [this] { scheduleSave(); });
        QObject::connect(list, &DocumentList::currentIndexChanged, q, [this] { scheduleSave(); });
    }

    // A window: its tab list (restored from `restore` if given), then its QML.
    std::shared_ptr<Window> createWindow(const WindowState *restore)
    {
        auto w = std::make_shared<Window>();
        w->list = new DocumentList(q);
        QQmlEngine::setObjectOwnership(w->list, QQmlEngine::CppOwnership);
        if (restore) {
            for (const TabState &tab : restore->tabs) {
                session.restoreTab(w->list, tab);
            }
            w->list->setCurrentIndex(restore->currentIndex);
            w->geometry = restore->geometry;
            w->maximized = restore->maximized;
        } else {
            w->geometry = settings->windowGeometry();
            w->maximized = settings->windowMaximized();
        }
        windows.prepend(w);
        watchList(w->list);

        if (engine) {
            const int before = int(engine->rootObjects().size());
            engine->setInitialProperties({{QStringLiteral("documents"), QVariant::fromValue(w->list)}});
            engine->loadFromModule(QStringLiteral("net.eterneon.atlas.notepad"), QStringLiteral("Main"));
            if (engine->rootObjects().size() > before) {
                w->window = qobject_cast<QQuickWindow *>(engine->rootObjects().last());
            }
        }
        if (QQuickWindow *win = w->window) {
            if (w->geometry.isValid()) {
                w->geometry = clampToScreens(w->geometry);
                win->setGeometry(w->geometry);
            }
            // Follow the normal (not maximized) geometry for the session.
            auto track = [w] {
                if (!w->window) {
                    return;
                }
                // Hidden and minimized say nothing about the window's size.
                const auto visibility = w->window->visibility();
                if (visibility == QWindow::Windowed) {
                    w->geometry = w->window->geometry();
                    w->maximized = false;
                } else if (visibility == QWindow::Maximized) {
                    w->maximized = true;
                }
            };
            for (auto signal : {&QWindow::xChanged, &QWindow::yChanged, &QWindow::widthChanged, &QWindow::heightChanged}) {
                QObject::connect(win, signal, q, track);
            }
            QObject::connect(win, &QWindow::visibilityChanged, q, track);
            QObject::connect(win, &QQuickWindow::activeChanged, q, [this, w] {
                if (w->window && w->window->isActive()) {
                    const auto i = windows.indexOf(w);
                    if (i > 0) {
                        windows.move(i, 0);
                    }
                }
            });
            if (w->maximized) {
                win->showMaximized();
            } else {
                win->show();
            }
        }
        return w;
    }

    void ensureTab(DocumentList *list)
    {
        if (list->rowCount() == 0) {
            list->newTab();
        }
    }

    void updateMenu()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        const bool now = bus.isConnected() && bus.interface() && bus.interface()->isServiceRegistered(QLatin1String(menuService));
        if (now != hasMenu) {
            hasMenu = now;
            Q_EMIT q->hasGlobalMenuChanged();
        }
    }
};

App::App(QQmlApplicationEngine *engine, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    s_instance = this;
    d->q = this;
    d->engine = engine;
    d->settings = new Settings(this);
    d->saveTimer.setSingleShot(true);
    d->saveTimer.setInterval(1000);
    d->maxTimer.setSingleShot(true);
    d->maxTimer.setInterval(5000);
    auto writeNow = [this] {
        d->stopTimers();
        d->write(false);
    };
    connect(&d->saveTimer, &QTimer::timeout, this, writeNow);
    connect(&d->maxTimer, &QTimer::timeout, this, writeNow);
    connect(d->settings, &Settings::formattingChanged, this, [this] {
        for (const auto &w : d->windows) {
            for (Document *doc : w->list->documents()) {
                doc->d->applyMarkdown(doc->d->currentBytes());
            }
        }
    });

    if (engine) {
        // Logout and quit: the session is written before anything goes away.
        auto *guiApp = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
        if (guiApp) {
            connect(guiApp, &QGuiApplication::commitDataRequest, this, [this] {
                // No signal says a logout was cancelled: the app becoming
                // active again (below) resets this.
                d->quitting = true;
                saveSession();
            }, Qt::DirectConnection);
            connect(guiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive) {
                    return;
                }
                if (!d->exiting) {
                    d->quitting = false;
                }
                for (const auto &w : d->windows) {
                    for (Document *doc : w->list->documents()) {
                        doc->d->checkOnDisk();
                    }
                }
            });
        }
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
            d->quitting = true;
            d->exiting = true;
            saveSession();
        });
        auto *watcher = new QDBusServiceWatcher(QLatin1String(menuService), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { d->updateMenu(); });
        d->updateMenu();
    }
}

App::~App()
{
    d->stopTimers();
    // The engine outlives App (main.cpp makes it first); its windows go
    // now, while the App singleton and their documents still exist: those
    // still open and those closed but not yet deleted.
    for (const auto &w : std::as_const(d->windows)) {
        delete w->window.data();
    }
    for (const auto &win : std::as_const(d->closed)) {
        delete win.data();
    }
    s_instance = nullptr;
}

App *App::instance()
{
    return s_instance;
}

App *App::create(QQmlEngine *, QJSEngine *)
{
    QQmlEngine::setObjectOwnership(s_instance, QQmlEngine::CppOwnership);
    return s_instance;
}

Settings *App::settings() const
{
    return d->settings;
}

QStringList App::recentFiles() const
{
    QStringList out;
    for (const QString &p : d->readRecent()) {
        if (QFileInfo::exists(p)) {
            out.append(p);
        }
    }
    return out;
}

bool App::hasGlobalMenu() const
{
    return d->hasMenu;
}

QString App::version() const
{
    return QCoreApplication::applicationVersion();
}

QList<DocumentList *> App::windows() const
{
    QList<DocumentList *> out;
    for (const auto &w : d->windows) {
        out.append(w->list);
    }
    return out;
}

QQuickWindow *App::activeWindow() const
{
    for (const auto &w : d->windows) {
        if (w->window && w->window->isVisible()) {
            return w->window.data();
        }
    }
    return nullptr;
}

void App::setSaveDelays(int quietMs, int maxMs)
{
    d->saveTimer.setInterval(quietMs);
    d->maxTimer.setInterval(maxMs);
}

void App::setSessionEnabled(bool enabled)
{
    d->sessionEnabled = enabled;
}

void App::start(const QStringList &files)
{
    if (d->sessionOn()) {
        const QList<WindowState> states = d->session.read();
        // Stored most recent first; the first made last is the most recent.
        for (auto it = states.crbegin(); it != states.crend(); ++it) {
            d->createWindow(&*it);
        }
    }
    if (d->windows.isEmpty()) {
        d->createWindow(nullptr);
    }
    QList<QUrl> urls;
    for (const QString &file : files) {
        urls.append(QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()));
    }
    if (!urls.isEmpty()) {
        d->windows.first()->list->open(urls);
    }
    for (const auto &w : d->windows) {
        d->ensureTab(w->list);
    }
}

void App::activate(const QStringList &arguments, const QString &workingDirectory)
{
    // The first argument is the program name.
    QList<QUrl> urls;
    bool newWindow = false;
    for (const QString &arg : arguments.mid(1)) {
        if (arg == QLatin1String("--new-window")) {
            newWindow = true;
        }
        if (arg.startsWith(QLatin1Char('-'))) {
            continue;
        }
        const QUrl url(arg);
        const QString path = url.isLocalFile() ? url.toLocalFile() : arg;
        urls.append(QUrl::fromLocalFile(QDir(workingDirectory).absoluteFilePath(path)));
    }
    // A closed last window stays (the session keeps its tabs) but is hidden:
    // when none is visible, bring that one back.
    bool anyVisible = false;
    for (const auto &w : d->windows) {
        anyVisible = anyVisible || !w->window || w->window->isVisible();
    }
    if (d->windows.isEmpty() || newWindow || (!urls.isEmpty() && d->settings->openInNewWindow())) {
        d->createWindow(nullptr);
    } else if (!anyVisible && d->windows.first()->window) {
        d->windows.first()->window->show();
    }
    DocumentList *target = d->windows.first()->list;
    if (!urls.isEmpty()) {
        target->open(urls);
    }
    d->ensureTab(target);
    if (QQuickWindow *win = d->windows.first()->window) {
        // KDBusService put the second launch's activation token in the
        // environment; on Wayland, KWin only lets a window take focus with one.
        KWindowSystem::updateStartupId(win);
        win->show();
        win->raise();
        KWindowSystem::activateWindow(win);
    }
}

void App::newWindow()
{
    d->ensureTab(d->createWindow(nullptr)->list);
}

void App::addRecentFile(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QStringList paths = d->readRecent();
    paths.removeAll(absolute);
    paths.prepend(absolute);
    while (paths.size() > recentLimit) {
        paths.removeLast();
    }
    d->writeRecent(paths);
    Q_EMIT recentFilesChanged();
}

void App::clearRecentFiles()
{
    d->writeRecent({});
    Q_EMIT recentFilesChanged();
}

void App::print(Document *document, QQuickWindow *parent)
{
    if (!document) {
        return;
    }
    QPrinter printer;
    const QString title = document->title();
    printer.setDocName(title);
    // With no printer, Print to File is the choice: suggest <name>.pdf beside
    // the file (in Documents for a new tab). Qt names the file only on X11;
    // on Wayland it suggests a bare folder. With a printer, an output file
    // would make the dialog choose Print to File over it.
    if (QPrinterInfo::defaultPrinterName().isEmpty()) {
        const QString path = document->path();
        const QString base = path.isEmpty() ? title : QFileInfo(path).completeBaseName();
        const QString dir = path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : QFileInfo(path).absolutePath();
        printer.setOutputFileName(QDir(dir).filePath(base + QStringLiteral(".pdf")));
    }
    QPrintDialog dialog(&printer);
    dialog.setAttribute(Qt::WA_NativeWindow);
    dialog.winId();
    if (parent && dialog.windowHandle()) {
        dialog.windowHandle()->setTransientParent(parent);
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QTextDocument doc;
    if (document->isMarkdown() && document->isFormatted()) {
        doc.setMarkdown(document->text());
    } else {
        doc.setDefaultFont(d->settings->font());
        doc.setPlainText(document->text());
    }
    doc.print(&printer);
}

QString App::timeDate() const
{
    const QLocale locale = QLocale::system();
    const QDateTime now = QDateTime::currentDateTime();
    return locale.toString(now.time(), QLocale::ShortFormat) + QLatin1Char(' ') + locale.toString(now.date(), QLocale::ShortFormat);
}

bool App::closeWindow(DocumentList *documents, bool force)
{
    const auto w = d->windowOf(documents);
    if (!w) {
        return true;
    }
    const bool last = d->windows.size() == 1;
    if (d->settings->continueSession() && (last || d->quitting)) {
        // The session keeps this window's tabs.
        d->settings->setWindowGeometry(w->geometry, w->maximized);
        if (!d->quitting) {
            saveSession();
        }
        return true;
    }
    if (documents->anyModified() && !force) {
        if (d->quitInProgress) {
            d->quitWaitingOn = documents; // its dialog decides the quit
        }
        return false;
    }
    if (last) {
        d->settings->setWindowGeometry(w->geometry, w->maximized);
    }
    // The tabs leave the session with the window.
    d->windows.removeAll(w);
    d->write(false);
    // Nothing of this window reaches App any more (no saves, no tracking).
    QObject::disconnect(documents, nullptr, this, nullptr);
    for (Document *doc : documents->documents()) {
        QObject::disconnect(doc, nullptr, this, nullptr);
    }
    if (w->window) {
        // Closing only hides it (the engine owns it): delete it, and its
        // tabs with it.
        QObject::disconnect(w->window, nullptr, this, nullptr);
        QObject::connect(w->window, &QObject::destroyed, documents, &QObject::deleteLater);
        d->closed.removeAll(nullptr);
        d->closed.append(w->window);
        w->window->deleteLater();
    } else {
        documents->deleteLater();
    }
    if (force && d->quitInProgress && d->quitWaitingOn == documents) {
        // The dialog's "discard" ended the wait: on to the next window.
        d->quitWaitingOn = nullptr;
        QMetaObject::invokeMethod(this, &App::quit, Qt::QueuedConnection);
    }
    return true;
}

void App::quit()
{
    const auto windows = d->windows;
    if (d->settings->continueSession()) {
        d->quitting = true;
        d->exiting = true;
        saveSession();
        for (const auto &w : windows) {
            if (w->window) {
                w->window->close();
            }
        }
        QCoreApplication::quit();
        return;
    }
    d->quitInProgress = true;
    d->quitWaitingOn = nullptr;
    for (const auto &w : windows) {
        if (w->window) {
            w->window->close();
            if (w->window && w->window->isVisible()) {
                return; // refused: its QML asks, then closeWindow(force) continues
            }
        } else if (!closeWindow(w->list, false)) {
            return;
        }
    }
    d->quitInProgress = false;
    d->exiting = true;
    QCoreApplication::quit();
}

void App::cancelQuit()
{
    d->quitInProgress = false;
    d->quitWaitingOn = nullptr;
}

QString App::displayPath(const QString &path) const
{
    const QString home = QDir::homePath();
    if (path == home || (path.startsWith(home) && path.at(home.size()) == u'/')) {
        return u'~' + path.mid(home.size());
    }
    return path;
}

bool App::openUpdater() const
{
    // A running Updater answers the new one, which then exits.
    return QProcess::startDetached(QStringLiteral("atlas-updater"), {});
}

QString App::shortcutText(const QVariant &shortcut) const
{
    if (!shortcut.isValid() || shortcut.isNull()) {
        return {};
    }
    // StandardKey arrives as a number; a string is a sequence like "Ctrl+H".
    if (shortcut.typeId() != QMetaType::QString) {
        bool ok = false;
        const int key = shortcut.toInt(&ok);
        if (ok) {
            return QKeySequence(QKeySequence::StandardKey(key)).toString(QKeySequence::NativeText);
        }
    }
    return QKeySequence(shortcut.toString()).toString(QKeySequence::NativeText);
}

void App::copyToClipboard(const QString &text)
{
    if (QGuiApplication::clipboard()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

void App::saveSession()
{
    d->stopTimers();
    d->write(true);
}
