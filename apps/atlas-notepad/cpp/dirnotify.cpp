#include "dirnotify.h"

#include "app.h"
#include "document_p.h"
#include "remote.h"

#include <QDBusConnection>
#include <QFileInfo>

namespace
{
QString trimmed(const QString &path)
{
    return path.size() > 1 && path.endsWith(QLatin1Char('/')) ? path.chopped(1) : path;
}

// The part of the document's path below `dir`, "" when it is `dir` itself;
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
    if (d.startsWith(s + QLatin1Char('/')) || s == QLatin1String("/")) {
        return d.mid(s.size());
    }
    return {};
}

QList<Document *> allDocuments(App *app)
{
    QList<Document *> out;
    for (DocumentList *list : app->windows()) {
        out += list->documents();
    }
    return out;
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
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return; // no session bus: nothing to follow, quietly
    }
    const QString iface = QStringLiteral("org.kde.KDirNotify");
    m_connected = bus.connect({}, {}, iface, QStringLiteral("FileRenamed"), this, SLOT(FileRenamed(QString, QString)));
    bus.connect({}, {}, iface, QStringLiteral("FileRenamedWithLocalPath"), this, SLOT(FileRenamedWithLocalPath(QString, QString, QString)));
    bus.connect({}, {}, iface, QStringLiteral("FileMoved"), this, SLOT(FileMoved(QString, QString)));
    bus.connect({}, {}, iface, QStringLiteral("FilesChanged"), this, SLOT(FilesChanged(QStringList)));
    bus.connect({}, {}, iface, QStringLiteral("FilesRemoved"), this, SLOT(FilesRemoved(QStringList)));
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
    if (!src.isValid() || !dst.isValid() || src.scheme().isEmpty() || dst.scheme().isEmpty()) {
        return;
    }
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
        to.setPath(trimmed(dst.path()) + rest);
        if (old.scheme() == to.scheme() && old.host() == to.host() && to.userName().isEmpty()) {
            to.setUserInfo(old.userInfo()); // the same server: same login
        }
        const bool sameFolder = trimmed(QUrl(to).adjusted(QUrl::RemoveFilename).path()) == trimmed(QUrl(old).adjusted(QUrl::RemoveFilename).path());
        const QString oldName = doc->title();
        if (!doc->d->relocate(to)) {
            continue;
        }
        const QString name = doc->title();
        Q_EMIT m_app->notice(sameFolder ? tr("“%1” was renamed to “%2”.").arg(oldName, name) : tr("“%1” was moved.").arg(name));
    }
}

void DirNotifyListener::FilesRemoved(const QStringList &files)
{
    const auto docs = allDocuments(m_app);
    for (const QString &text : files) {
        const QUrl gone(text);
        for (Document *doc : docs) {
            const QUrl at = locationOf(doc);
            if (at.isValid() && !below(at, gone).isNull()) {
                // The stat says so: a notice for a file that is still there
                // (a trash that was undone) changes nothing.
                if (doc->d->isRemote()) {
                    doc->d->checkRemote(true);
                } else {
                    doc->d->checkOnDisk();
                }
            }
        }
    }
}

void DirNotifyListener::FilesChanged(const QStringList &files)
{
    const auto docs = allDocuments(m_app);
    for (const QString &text : files) {
        const QUrl changed(text);
        for (Document *doc : docs) {
            // Local files have their watcher.
            if (doc->d->isRemote() && !below(doc->d->url, changed).isNull() && trimmed(doc->d->url.path()) == trimmed(changed.path())) {
                doc->d->checkRemote(true);
            }
        }
    }
}
