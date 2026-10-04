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
    QList<std::shared_ptr<Window>> windows; // most recent first
    QTimer saveTimer;
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

    QStringList readRecent() const
    {
        QStringList out;
        settings->rc().beginGroup(QStringLiteral("Recent"));
        for (int i = 0; i < recentLimit; ++i) {
            const QString p = settings->rc().value(QString::number(i)).toString();
            if (!p.isEmpty() && QFileInfo::exists(p)) {
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
        }
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
                win->setGeometry(w->geometry);
            }
            // Follow the normal (not maximized) geometry for the session.
            auto track = [w] {
                if (!w->window) {
                    return;
                }
                const bool max = w->window->visibility() == QWindow::Maximized;
                if (w->window->visibility() == QWindow::Windowed) {
                    w->geometry = w->window->geometry();
                }
                w->maximized = max;
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
    connect(&d->saveTimer, &QTimer::timeout, this, [this] { d->write(false); });
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
                d->quitting = true;
                saveSession();
            }, Qt::DirectConnection);
            connect(guiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive) {
                    return;
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
            saveSession();
        });
        auto *watcher = new QDBusServiceWatcher(QLatin1String(menuService), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { d->updateMenu(); });
        d->updateMenu();
    }
}

App::~App()
{
    d->saveTimer.stop();
    // The engine outlives App (main.cpp makes it first); its windows go
    // now, while the App singleton and their documents still exist.
    for (const auto &w : std::as_const(d->windows)) {
        delete w->window.data();
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
    return d->readRecent();
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
    return d->windows.isEmpty() ? nullptr : d->windows.first()->window.data();
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
    if (d->windows.isEmpty() || newWindow || (!urls.isEmpty() && d->settings->openInNewWindow())) {
        d->createWindow(nullptr);
    }
    DocumentList *target = d->windows.first()->list;
    if (!urls.isEmpty()) {
        target->open(urls);
    }
    d->ensureTab(target);
    if (QQuickWindow *win = activeWindow()) {
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
        return false;
    }
    if (last) {
        d->settings->setWindowGeometry(w->geometry, w->maximized);
    }
    // The tabs leave the session with the window.
    d->windows.removeAll(w);
    d->write(false);
    if (w->window) {
        QObject::connect(w->window, &QObject::destroyed, documents, &QObject::deleteLater);
    } else {
        documents->deleteLater();
    }
    return true;
}

void App::quit()
{
    const auto windows = d->windows;
    if (d->settings->continueSession()) {
        d->quitting = true;
        saveSession();
        for (const auto &w : windows) {
            if (w->window) {
                w->window->close();
            }
        }
        QCoreApplication::quit();
        return;
    }
    for (const auto &w : windows) {
        if (w->window) {
            w->window->close();
            if (w->window && w->window->isVisible()) {
                return; // refused: its QML asks, then calls quit() again
            }
        } else if (!closeWindow(w->list, false)) {
            return;
        }
    }
    QCoreApplication::quit();
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
    d->saveTimer.stop();
    d->write(true);
}
