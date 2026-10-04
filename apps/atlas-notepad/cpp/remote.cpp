#include "remote.h"

#include <KIO/Job>
#include <KIO/JobUiDelegateFactory>
#include <KIO/StatJob>
#include <KJobWindows>
#include <KProtocolInfo>
#include <KProtocolManager>

#include <QGuiApplication>
#include <QWindow>

#include <QSet>

#include <cerrno>

using namespace Qt::StringLiterals;

namespace Remote
{
#ifdef NP_KIO_TEST_HOOK
namespace
{
bool forced = false;
}
void setForceKio(bool on)
{
    forced = on;
}
#endif

bool useKio(const QUrl &url)
{
    if (!url.isValid() || url.scheme().isEmpty()) {
        return false;
    }
#ifdef NP_KIO_TEST_HOOK
    if (forced) {
        return true;
    }
#endif
    return !url.isLocalFile();
}

QString display(const QUrl &url)
{
    return url.toString(QUrl::RemovePassword);
}

bool isStoredUrl(const QString &stored)
{
    if (stored.isEmpty() || stored.startsWith(QLatin1Char('/'))) {
        return false;
    }
    const QUrl url(stored);
    return url.isValid() && url.scheme().size() > 1; // not a drive letter
}

QUrl fromStored(const QString &stored)
{
    return isStoredUrl(stored) ? QUrl(stored) : QUrl::fromLocalFile(stored);
}

// What Notepad opens, by scheme, whatever else KIO has workers for. Others
// reach us from the command line, D-Bus, the session and the dialogs alike.
QString unsupported(const QUrl &url, bool forWriting)
{
    static const QSet<QString> readWrite = {u"file"_s, u"sftp"_s, u"fish"_s, u"ftp"_s, u"ftps"_s, u"smb"_s, u"webdav"_s, u"webdavs"_s, u"nfs"_s, u"mtp"_s, u"gdrive"_s, u"kdeconnect"_s};
    static const QSet<QString> readOnly = {u"http"_s, u"https"_s, u"zip"_s, u"tar"_s, u"ar"_s, u"archive"_s};
    const QString scheme = url.scheme();
    const bool writable = readWrite.contains(scheme);
    if ((!writable && !readOnly.contains(scheme)) || !KProtocolInfo::isKnownProtocol(url)) {
        return QObject::tr("Notepad can't open “%1:” locations.").arg(scheme);
    }
    if (!KProtocolManager::supportsReading(url)) {
        return QObject::tr("“%1:” locations don't hold files Notepad can read.").arg(scheme);
    }
    if (forWriting && (!writable || !KProtocolManager::supportsWriting(url))) {
        return QObject::tr("“%1:” locations can't be written to.").arg(scheme);
    }
    return {};
}

QString nameAndHost(const QUrl &url)
{
    const QString name = url.fileName();
    if (url.host().isEmpty() || name.isEmpty()) {
        return display(url);
    }
    return QObject::tr("%1 — %2").arg(name, url.host());
}

void setup(KJob *job, QWindow *window)
{
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingDisabled, nullptr));
    KJobWindows::setWindow(job, window ? window : QGuiApplication::focusWindow());
}

KJob *stat(const QUrl &url, QObject *ctx, QWindow *window, std::function<void(const StatInfo &)> done)
{
    KIO::StatJob *job = KIO::stat(url, KIO::StatJob::SourceSide, KIO::StatDefaultDetails, KIO::HideProgressInfo);
    if (!job) {
        StatInfo info;
        info.error = EIO;
        info.errorText = QObject::tr("The location can't be reached.");
        done(info);
        return nullptr;
    }
    setup(job, window);
    QObject::connect(job, &KJob::result, ctx, [job, done = std::move(done)] {
        StatInfo info;
        if (job->error()) {
            info.error = job->error() == KIO::ERR_DOES_NOT_EXIST ? ENOENT : job->error() == KIO::ERR_USER_CANCELED ? ECANCELED : EIO;
            info.errorText = job->errorString();
        } else {
            const KIO::UDSEntry entry = job->statResult();
            info.isDir = entry.isDir();
            const qint64 size = entry.numberValue(KIO::UDSEntry::UDS_SIZE, -1);
            const qint64 mtime = entry.numberValue(KIO::UDSEntry::UDS_MODIFICATION_TIME, -1);
            const qint64 access = entry.numberValue(KIO::UDSEntry::UDS_ACCESS, -1);
            info.stamp.size = size < 0 ? 0 : quint64(size);
            // A server's date is its own: clamped so the product can't overflow.
            info.stamp.mtimeNs = mtime < 0 ? -1 : qMin<qint64>(mtime, 9000000000) * 1000000000;
            info.writable = access < 0 || (access & 0222) != 0;
        }
        done(info);
    });
    return job;
}
} // namespace Remote
