#include "dirnotify.h"

#include "app.h"
#include "document_p.h"
#include "remote.h"

#include <QDBusConnection>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QWindow>
#include <QPointer>

namespace
{
// A notice lists at most this many files; the rest is dropped.
constexpr int maxListed = 1000;

QString trimmed(const QString &path)
{
    return path.size() > 1 && path.endsWith(QLatin1Char('/')) ? path.chopped(1) : path;
}

// The part of the document's path below `dir` ("" when it is `dir` itself);
// null when the document isn't at or under it.
QString below(const QUrl &doc, const QUrl &dir)
{
    if (doc.scheme() != dir.scheme() || doc.host() != dir.host() || doc.port() != dir.port()) {
        return {};
    }
    const QString d = trimmed(doc.path()), s = trimmed(dir.path());
    if (d == s) {
        return QStringLiteral("");
    }
    const QString prefix = s == QLatin1String("/") ? QString() : s;
    return d.startsWith(prefix + QLatin1Char('/')) ? d.mid(prefix.size()) : QString();
}

QList<Document *> allDocuments(App *app)
{
    QList<Document *> out;
    for (DocumentList *list : app->windows()) {
        out += list->documents();
    }
    return out;
}

QString keyOf(const QUrl &url)
{
    return url.isLocalFile() ? url.toLocalFile() : Remote::display(url);
}
}

QUrl DirNotifyListener::locationOf(Document *doc)
{
    if (doc->d->path.isEmpty()) {
        return {};
    }
    return doc->d->isRemote() ? doc->d->url : QUrl::fromLocalFile(doc->d->path);
}

DirNotifyListener::DirNotifyListener(App *app)
    : QObject(app)
    , m_app(app)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &DirNotifyListener::flush);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return; // no session bus: nothing to follow, quietly
    }
    const QString iface = QStringLiteral("org.kde.KDirNotify");
    bool all = true;
    auto join = [&](const char *name, const char *slot) {
        const bool ok = bus.connect({}, {}, iface, QString::fromLatin1(name), this, slot);
        if (!ok) {
            qWarning("atlas-notepad: can't listen for KDirNotify %s: other programs' renames won't be followed", name);
        }
        all = all && ok;
    };
    join("FileRenamed", SLOT(FileRenamed(QString, QString)));
    join("FileRenamedWithLocalPath", SLOT(FileRenamedWithLocalPath(QString, QString, QString)));
    join("FileMoved", SLOT(FileMoved(QString, QString)));
    join("FilesChanged", SLOT(FilesChanged(QStringList)));
    join("FilesRemoved", SLOT(FilesRemoved(QStringList)));
    m_connected = all;
}

void DirNotifyListener::FileRenamedWithLocalPath(const QString &src, const QString &dst, const QString &)
{
    queueMove(src, dst);
}

void DirNotifyListener::FileMoved(const QString &src, const QString &dst)
{
    queueMove(src, dst);
}

void DirNotifyListener::FileRenamed(const QString &src, const QString &dst)
{
    queueMove(src, dst);
}

// A flood of notices costs a list entry each (up to the cap) and one pass.
void DirNotifyListener::queueMove(const QString &srcText, const QString &dstText)
{
    if (m_moves.size() >= maxListed) {
        return;
    }
    const QUrl src(srcText), dst(dstText);
    // Same kind of place only: no trash:/ and no move to another protocol.
    if (!src.isValid() || !dst.isValid() || src.scheme().isEmpty() || src.scheme() != dst.scheme()) {
        return;
    }
    m_moves.append({src, dst});
    if (!m_timer.isActive()) {
        m_timer.start();
    }
}

// A notice is only a claim. The tab follows when the same file is at the new
// place (same server and login, the size and time the tab last saw) and the
// old place is empty; otherwise the usual stat shows what happened to it.
// A remote file that went to another folder is only offered (a banner with
// Follow): a program on the bus must not steer a tab to a place of its choice.
void DirNotifyListener::follow(Document *doc, const QUrl &old, const QUrl &to)
{
    if (doc->d->validating || old.scheme() != to.scheme() || !Remote::unsupported(to, false).isEmpty() || !doc->d->hasStamp) {
        return;
    }
    if (!doc->d->isRemote()) {
        if (!to.isLocalFile()) {
            return;
        }
        NpStamp atNew = {}, atOld = {};
        if (np_file_stamp(to.toLocalFile().toUtf8().constData(), &atNew) != 0 || !sameStamp(atNew, doc->d->stamp)) {
            return;
        }
        const int err = np_file_stamp(old.toLocalFile().toUtf8().constData(), &atOld);
        if (err != ENOENT && err != ENOTDIR) {
            return;
        }
        apply(doc, old, to);
        return;
    }
    // The same server and login: alice's file isn't bob's.
    if (old.host() != to.host() || old.port() != to.port() || ((!to.userName().isEmpty() || !old.userName().isEmpty()) && old.userName() != to.userName())) {
        return;
    }
    // The old URL's scheme, host, port and login with the new path only.
    QUrl target = old;
    target.setPath(QDir::cleanPath(to.path()));
    target.setQuery(QString());
    target.setFragment(QString());
    const QPointer<Document> guard(doc);
    const QPointer<QWindow> window(doc->d->window());
    doc->d->validating = true;
    Remote::stat(target, this, window, [this, guard, window, old, target](const Remote::StatInfo &atNew) {
        if (!guard) {
            return;
        }
        if (atNew.error || atNew.isDir || atNew.stamp.size != guard->d->stamp.size || atNew.stamp.mtimeNs != guard->d->stamp.mtimeNs) {
            guard->d->validating = false;
            return;
        }
        Remote::stat(old, this, window, [this, guard, old, target](const Remote::StatInfo &atOld) {
            if (!guard) {
                return;
            }
            guard->d->validating = false;
            if (atOld.error != ENOENT) {
                return;
            }
            const QUrl folderOld = old.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash);
            const QUrl folderNew = target.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash);
            if (folderOld.path() == folderNew.path()) {
                apply(guard, old, target);
            } else if (locationOf(guard) == old) {
                guard->d->offerMove(old, target);
            }
        });
    });
}

void DirNotifyListener::apply(Document *doc, const QUrl &old, const QUrl &to)
{
    if (locationOf(doc) != old) {
        return; // it moved on while we checked
    }
    const bool sameFolder = trimmed(old.adjusted(QUrl::RemoveFilename).path()) == trimmed(to.adjusted(QUrl::RemoveFilename).path());
    const QString oldName = doc->title();
    if (!doc->d->relocate(to)) {
        return;
    }
    const QString name = doc->title();
    Q_EMIT m_app->notice(sameFolder ? tr("“%1” was renamed to “%2”.").arg(oldName, name) : tr("“%1” was moved.").arg(name));
}

void DirNotifyListener::FilesRemoved(const QStringList &files)
{
    m_removed += files.mid(0, qMax(0, maxListed - int(m_removed.size())));
    if (!m_timer.isActive()) {
        m_timer.start(); // not restarted: a flood can't push the pass back for ever
    }
}

void DirNotifyListener::FilesChanged(const QStringList &files)
{
    m_changed += files.mid(0, qMax(0, maxListed - int(m_changed.size())));
    if (!m_timer.isActive()) {
        m_timer.start();
    }
}

void DirNotifyListener::flush()
{
    const QStringList removed = std::exchange(m_removed, {});
    const QStringList changed = std::exchange(m_changed, {});
    const auto moves = std::exchange(m_moves, {});
    const auto docs = allDocuments(m_app);
    // Closed tabs follow a rename of local files (see renameClosed); each
    // tab, the latest notice that concerns it.
    QHash<Document *, QPair<QUrl, QUrl>> latest;
    for (const auto &move : moves) {
        for (DocumentList *list : m_app->windows()) {
            list->renameClosed(keyOf(move.first), keyOf(move.second));
        }
        const QString base = trimmed(move.second.path()) == QLatin1String("/") ? QString() : trimmed(move.second.path());
        for (Document *doc : docs) {
            const QUrl old = locationOf(doc);
            if (!old.isValid()) {
                continue;
            }
            const QString rest = below(old, move.first);
            if (rest.isNull()) {
                continue;
            }
            QUrl to = move.second;
            to.setPath(base + rest);
            latest.insert(doc, {old, to});
        }
    }
    for (auto it = latest.cbegin(); it != latest.cend(); ++it) {
        follow(it.key(), it.value().first, it.value().second);
    }
    for (const QString &text : removed) {
        const QUrl gone(text);
        for (Document *doc : docs) {
            const QUrl at = locationOf(doc);
            if (at.isValid() && !below(at, gone).isNull()) {
                // The stat says so: a notice for a file that is still there
                // (a trash that was undone) changes nothing. The usual
                // throttle applies, so a flood costs one stat a tab.
                if (doc->d->isRemote()) {
                    doc->d->checkRemote(false);
                } else {
                    doc->d->checkOnDisk();
                }
            }
        }
    }
    for (const QString &text : changed) {
        const QUrl url(text);
        for (Document *doc : docs) {
            // Local files have their watcher.
            if (doc->d->isRemote() && trimmed(doc->d->url.path()) == trimmed(url.path()) && doc->d->url.host() == url.host()) {
                doc->d->checkRemote(false);
            }
        }
    }
}
