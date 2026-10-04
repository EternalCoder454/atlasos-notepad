#include "dirnotify.h"

#include "app.h"
#include "document_p.h"
#include "remote.h"

#include <QDBusConnection>
#include <QFileInfo>
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
    FileRenamed(src, dst);
}

void DirNotifyListener::FileMoved(const QString &src, const QString &dst)
{
    FileRenamed(src, dst);
}

void DirNotifyListener::FileRenamed(const QString &srcText, const QString &dstText)
{
    const QUrl src(srcText), dst(dstText);
    // Same kind of place only: no trash:/ and no move to another protocol.
    if (!src.isValid() || !dst.isValid() || src.scheme().isEmpty() || src.scheme() != dst.scheme()) {
        return;
    }
    // The tabs closed from there are offered from the new place.
    for (DocumentList *list : m_app->windows()) {
        list->renameClosed(keyOf(src), keyOf(dst));
    }
    const QString base = trimmed(dst.path()) == QLatin1String("/") ? QString() : trimmed(dst.path());
    const auto docs = allDocuments(m_app);
    for (Document *doc : docs) {
        const QUrl old = locationOf(doc);
        if (!old.isValid()) {
            continue;
        }
        const QString rest = below(old, src);
        if (rest.isNull()) {
            continue;
        }
        QUrl to = dst;
        to.setPath(base + rest);
        follow(doc, old, to);
    }
}

// A notice is only a claim. The tab follows when the same file is at the new
// place (same server and user, the size and time the tab last saw) and the
// old place is empty; otherwise the usual stat shows what happened to it.
void DirNotifyListener::follow(Document *doc, const QUrl &old, const QUrl &to)
{
    if (old.scheme() != to.scheme() || !Remote::unsupported(to, false).isEmpty() || !doc->d->hasStamp) {
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
    QUrl target = to;
    target.setUserInfo(old.userInfo()); // the notice has no password
    const QPointer<Document> guard(doc);
    QWindow *window = doc->d->window();
    Remote::stat(target, this, window, [this, guard, old, target, window](const Remote::StatInfo &atNew) {
        if (!guard || atNew.error || atNew.isDir || atNew.stamp.size != guard->d->stamp.size || atNew.stamp.mtimeNs != guard->d->stamp.mtimeNs) {
            return;
        }
        Remote::stat(old, this, window, [this, guard, old, target](const Remote::StatInfo &atOld) {
            if (guard && atOld.error == ENOENT) {
                apply(guard, old, target);
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
    m_timer.start();
}

void DirNotifyListener::FilesChanged(const QStringList &files)
{
    m_changed += files.mid(0, qMax(0, maxListed - int(m_changed.size())));
    m_timer.start();
}

void DirNotifyListener::flush()
{
    const QStringList removed = std::exchange(m_removed, {});
    const QStringList changed = std::exchange(m_changed, {});
    const auto docs = allDocuments(m_app);
    for (const QString &text : removed) {
        const QUrl gone(text);
        for (Document *doc : docs) {
            const QUrl at = locationOf(doc);
            if (at.isValid() && !below(at, gone).isNull()) {
                // The stat says so: a notice for a file that is still there
                // (a trash that was undone) changes nothing. One stat for each
                // tab, so it doesn't wait for the usual interval.
                if (doc->d->isRemote()) {
                    doc->d->checkRemote(true);
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
