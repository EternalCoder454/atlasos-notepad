// The process: settings, recent files, the windows and the session.
#include "app.h"
#include "dirnotify.h"
#include "remote.h"

#include <KRecentDocument>

#include "document_p.h"
#include "session.h"

#include <KWindowSystem>

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QDesktopServices>
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
#include <utility>

namespace
{
// Loads nothing: ![](/home/you/private.png) or ![](/dev/zero) in a printed
// file mustn't read local files into the PDF (or never finish).
class PrintDocument : public QTextDocument
{
protected:
    QVariant loadResource(int, const QUrl &) override
    {
        return {};
    }
};

constexpr int recentLimit = 10;
// KDE's recent-documents file is written this long after a file is opened.
constexpr int kdeRecentDelayMs = 1000;
// A launch that lives this long after restoring got past it.
constexpr int restoreSettleMs = 3000;
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
    DirNotifyListener *dirNotify = nullptr;
    QList<QUrl> kdeRecent; // waiting for kdeRecentTimer
    QTimer kdeRecentTimer;
    void flushKdeRecent()
    {
        kdeRecentTimer.stop();
        const QList<QUrl> urls = std::exchange(kdeRecent, {});
        for (const QUrl &url : urls) {
            KRecentDocument::add(url, QStringLiteral("net.eterneon.telamon.notepad"));
        }
    }
    bool hasMenu = false;
    bool restoring = false; // Session::beginRestore() without endRestore() yet
    QString problem; // sessionProblem

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
            refreshProblem();
            return;
        }
        QList<Session::Live> live;
        for (const auto &w : windows) {
            live.append({w->list, w->geometry, w->maximized});
        }
        session.write(live, wait);
        if (wait) {
            refreshProblem();
        }
    }

    // After a write: sessionProblem follows it, and the user hears once
    // when the session stops working (again after it worked in between).
    void refreshProblem()
    {
        const QString now = session.lastError();
        if (now == problem) {
            return;
        }
        const bool wasFine = problem.isEmpty();
        problem = now;
        Q_EMIT q->sessionProblemChanged();
        if (wasFine) {
            Q_EMIT q->message(App::tr("Notepad can't keep your unsaved changes for next time: %1").arg(now));
        }
    }

    // The restore is over once the restored tabs are read and have had
    // time to draw.
    void settleRestore()
    {
        auto *poll = new QTimer(q);
        poll->setInterval(250);
        QObject::connect(poll, &QTimer::timeout, q, [this, poll] {
            for (const auto &w : windows) {
                for (Document *doc : w->list->documents()) {
                    if (doc->isLoading()) {
                        return;
                    }
                }
            }
            poll->stop();
            poll->deleteLater();
            QTimer::singleShot(restoreSettleMs, q, [this] { endRestore(); });
        });
        poll->start();
    }

    void endRestore()
    {
        if (restoring) {
            restoring = false;
            session.endRestore();
        }
    }

    bool anyModified() const
    {
        for (const auto &w : windows) {
            if (w->list->anyModified()) {
                return true;
            }
        }
        return false;
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
    std::shared_ptr<Window> createWindow(const WindowState *restore, bool safe = false)
    {
        auto w = std::make_shared<Window>();
        w->list = new DocumentList(q);
        QQmlEngine::setObjectOwnership(w->list, QQmlEngine::CppOwnership);
        if (restore) {
            for (const TabState &tab : restore->tabs) {
                session.restoreTab(w->list, tab, safe);
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
            engine->loadFromModule(QStringLiteral("net.eterneon.telamon.notepad"), QStringLiteral("Main"));
            if (engine->rootObjects().size() > before) {
                w->window = qobject_cast<QQuickWindow *>(engine->rootObjects().last());
                w->list->setWindow(w->window);
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
    QTimer::singleShot(1500, this, &App::startDirNotify); // after the first frames
    d->kdeRecentTimer.setSingleShot(true);
    d->kdeRecentTimer.setInterval(kdeRecentDelayMs);
    connect(&d->kdeRecentTimer, &QTimer::timeout, this, [this] { d->flushKdeRecent(); });
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
    d->session.onWritten(this, [this] { d->refreshProblem(); });
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
                d->flushKdeRecent();
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
            d->endRestore();
        });
        auto *watcher = new QDBusServiceWatcher(QLatin1String(menuService), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { d->updateMenu(); });
        d->updateMenu();
    }
}

App::~App()
{
    d->flushKdeRecent(); // a file opened just before quitting still counts
    d->stopTimers();
    d->endRestore();
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
        // A remote file isn't looked for: that would wait on the network.
        if (Remote::isStoredUrl(p) || QFileInfo::exists(p)) {
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
    QString notice;
    if (d->sessionEnabled && !d->session.lock()) {
        // Another Notepad (on another D-Bus session) has it.
        d->sessionEnabled = false;
        notice = tr("Notepad is already running in another session, so these tabs won't come back next time. Closing asks about unsaved changes.");
    }
    if (d->sessionOn()) {
        const int died = d->session.restoreDeaths();
        QList<WindowState> states = d->session.read();
        bool safe = false;
        if (died >= 2 && !states.isEmpty()) {
            d->session.setAside();
            states.clear();
            notice = tr("Notepad closed unexpectedly while opening your last session, twice, so it starts empty. That session is kept in %1.")
                         .arg(displayPath(Session::directory() + QStringLiteral("/session.json.bak")));
        } else if (died == 1 && !states.isEmpty()) {
            safe = true;
            notice = tr("Notepad closed unexpectedly while opening your tabs, so this time they open as plain text.");
        }
        if (states.isEmpty()) {
            d->session.endRestore(); // nothing restored: no count to keep
        } else {
            d->session.beginRestore(died);
            d->restoring = true;
        }
        // Stored most recent first; the first made last is the most recent.
        for (auto it = states.crbegin(); it != states.crend(); ++it) {
            d->createWindow(&*it, safe);
        }
        if (d->restoring) {
            d->settleRestore();
        }
    }
    if (d->windows.isEmpty()) {
        d->createWindow(nullptr);
    }
    QList<QUrl> urls;
    for (const QString &file : files) {
        const QUrl url = urlFromArgument(file, QDir::currentPath());
        if (url.isValid()) {
            urls.append(url);
        }
    }
    if (!urls.isEmpty()) {
        d->windows.first()->list->open(urls);
    }
    for (const auto &w : d->windows) {
        d->ensureTab(w->list);
    }
    if (!notice.isEmpty()) {
        // Once the windows' QML is listening.
        QTimer::singleShot(0, this, [this, notice] { Q_EMIT message(notice); });
    }
}

void App::activate(const QStringList &arguments, const QString &workingDirectory)
{
    // The first argument is the program name. Another process on the bus
    // sends these: at most a hundred files, and relative paths only against
    // an absolute working directory.
    constexpr int maxFiles = 100;
    QList<QUrl> urls;
    bool newWindow = false;
    bool options = true;
    const bool absoluteDir = QDir::isAbsolutePath(workingDirectory);
    for (const QString &arg : arguments.mid(1)) {
        if (options && arg == QLatin1String("--")) {
            options = false;
            continue;
        }
        if (options && arg == QLatin1String("--new-window")) {
            newWindow = true;
        }
        if ((options && arg.startsWith(QLatin1Char('-'))) || arg.isEmpty() || urls.size() >= maxFiles) {
            continue;
        }
        const QUrl url = urlFromArgument(arg, absoluteDir ? workingDirectory : QString());
        if (url.isValid()) {
            urls.append(url);
        }
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

QUrl App::urlFromArgument(const QString &arg, const QString &workingDirectory)
{
    if (arg.isEmpty()) {
        return {};
    }
    // A URL KIO may know (sftp://host/file); a "file:" one is a local path.
    // Anything else is a path, relative to the working directory.
    if (Remote::isStoredUrl(arg)) {
        const QUrl url(arg, QUrl::StrictMode);
        if (!url.isValid()) {
            return {};
        }
        return url.isLocalFile() ? QUrl::fromLocalFile(QFileInfo(url.toLocalFile()).absoluteFilePath()) : url;
    }
    if (!QDir::isAbsolutePath(arg) && !QDir::isAbsolutePath(workingDirectory)) {
        return {};
    }
    return QUrl::fromLocalFile(QDir(workingDirectory).absoluteFilePath(arg));
}

void App::startDirNotify()
{
    if (!d->dirNotify) {
        d->dirNotify = new DirNotifyListener(this);
    }
}

void App::addRecentFile(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    // Remote entries are URLs without a password; local ones, absolute paths.
    const QString absolute = Remote::isStoredUrl(path) ? Remote::display(QUrl(path)) : QFileInfo(path).absoluteFilePath();
    QStringList paths = d->readRecent();
    paths.removeAll(absolute);
    paths.prepend(absolute);
    while (paths.size() > recentLimit) {
        paths.removeLast();
    }
    d->writeRecent(paths);
    Q_EMIT recentFilesChanged();
    // KDE's recent documents too (Dolphin, the Kickoff menu): the URL has no
    // password, a local file is a file: URL.
    // Writing the file takes ms (and the first time creates it): not before
    // the text is on screen.
    // (Written on quit at the latest.)
    const QUrl url = Remote::isStoredUrl(absolute) ? QUrl(absolute) : QUrl::fromLocalFile(absolute);
    if (!d->kdeRecent.contains(url)) {
        d->kdeRecent.append(url);
    }
    d->kdeRecentTimer.start();
}

void App::renameRecent(const QString &from, const QString &to)
{
    QStringList paths = d->readRecent();
    const qsizetype at = paths.indexOf(from);
    if (at < 0) {
        return;
    }
    paths.removeAll(to);
    paths.replace(paths.indexOf(from), to);
    d->writeRecent(paths);
    Q_EMIT recentFilesChanged();
    // KDE's list too: the old name goes, the new one comes.
    auto asUrl = [](const QString &p) { return Remote::isStoredUrl(p) ? QUrl(p) : QUrl::fromLocalFile(p); };
    // The old name may still be waiting to be written: it never is now.
    d->kdeRecent.removeAll(asUrl(from));
    KRecentDocument::removeFile(asUrl(from));
    KRecentDocument::add(asUrl(to), QStringLiteral("net.eterneon.telamon.notepad"));
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
    PrintDocument doc;
    if (document->isMarkdown() && document->isFormatted()) {
        // As the editor shows it: HTML as text, images as their alt text.
        doc.setMarkdown(document->text(), QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML));
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
    if (d->sessionOn() && (last || d->quitting)) {
        // The session keeps this window's tabs.
        d->settings->setWindowGeometry(w->geometry, w->maximized);
        if (!d->quitting) {
            saveSession();
        }
        if (force && d->quitInProgress && d->quitWaitingOn == documents) {
            // Asked because the session failed (below); the quit goes on.
            d->quitWaitingOn = nullptr;
            QMetaObject::invokeMethod(this, &App::quit, Qt::QueuedConnection);
        }
        if (force || d->problem.isEmpty() || !documents->anyModified()) {
            return true;
        }
        // It couldn't keep them: ask, as without a session.
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
    if (d->sessionOn()) {
        saveSession();
        if (d->problem.isEmpty() || !d->anyModified()) {
            d->quitting = true;
            d->exiting = true;
            for (const auto &w : windows) {
                if (w->window) {
                    w->window->close();
                }
            }
            QCoreApplication::quit();
            return;
        }
        // The session couldn't keep the unsaved tabs: ask, as without one.
    }
    d->quitInProgress = true;
    d->quitWaitingOn = nullptr;
    for (const auto &w : windows) {
        if (w->window) {
            if (!w->window->isVisible() && w->list->anyModified()) {
                // A closed last window the session kept, which can't keep
                // it now: shown, so its QML can ask.
                w->window->show();
            }
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
    // By its installed path, not whatever "telamon-updater" is first on $PATH.
    // An Updater that hasn't been renamed yet is atlas-updater (this release only).
    QString updater = QStandardPaths::findExecutable(QStringLiteral("telamon-updater"), {QStringLiteral("/usr/bin")});
    if (updater.isEmpty()) {
        updater = QStandardPaths::findExecutable(QStringLiteral("atlas-updater"), {QStringLiteral("/usr/bin")});
    }
    return !updater.isEmpty() && QProcess::startDetached(updater, {});
}

namespace
{
QUrl documentLink(const QString &link)
{
    QString text = link;
    if (text.startsWith(QLatin1String("www."), Qt::CaseInsensitive)) {
        text.prepend(QLatin1String("https://"));
    }
    QUrl url(text);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid()) {
        return {};
    }
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
        return url.host().isEmpty() ? QUrl() : url;
    }
    if (scheme == QLatin1String("mailto")) {
        // Some mail clients attach the file an attach= names (~/.ssh/id_rsa).
        // The other fields stay byte for byte (a "+" is a plus in mailto).
        if (url.hasQuery()) {
            QStringList kept;
            bool dropped = false;
            const QString query = url.query(QUrl::FullyEncoded);
            for (const QString &field : query.split(QLatin1Char('&'))) {
                const QString key = QUrl::fromPercentEncoding(field.section(QLatin1Char('='), 0, 0).toUtf8()).trimmed().toLower();
                if (key.startsWith(QLatin1String("attach"))) {
                    dropped = true;
                } else {
                    kept.append(field);
                }
            }
            if (dropped) {
                url.setQuery(kept.isEmpty() ? QString() : kept.join(QLatin1Char('&')), QUrl::StrictMode);
            }
        }
        return url.path().isEmpty() && !url.hasQuery() ? QUrl() : url;
    }
    return {};
}
}

QString App::linkUrl(const QString &link) const
{
    return documentLink(link).toString(QUrl::FullyEncoded);
}

QString App::linkTarget(const QString &link) const
{
    const QUrl url = documentLink(link);
    // Punycode, as linkUrl: a lookalike host (аpple.com in Cyrillic) shows.
    return url.scheme() == QLatin1String("mailto") ? url.path(QUrl::FullyEncoded) : url.host(QUrl::EncodeUnicode);
}

bool App::openLink(const QString &link) const
{
    const QUrl url = documentLink(link);
    return url.isValid() && !url.isEmpty() && QDesktopServices::openUrl(url);
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

QString App::sessionProblem() const
{
    return d->problem;
}
