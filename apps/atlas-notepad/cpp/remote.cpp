#include "remote.h"

#include <KIO/Job>
#include <KIO/JobUiDelegateFactory>
#include <KIO/StatJob>
#include <KJobWindows>
#include <KProtocolInfo>
#include <KProtocolManager>

#include <QGuiApplication>
#include <QWindow>

#include <cerrno>

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

QString unsupported(const QUrl &url, bool forWriting)
{
    const QString scheme = url.scheme();
    if (!KProtocolInfo::isKnownProtocol(url)) {
        return QObject::tr("Notepad can't open “%1:” locations.").arg(scheme);
    }
    if (!KProtocolManager::supportsReading(url)) {
        return QObject::tr("“%1:” locations don't hold files Notepad can read.").arg(scheme);
    }
    if (forWriting && !KProtocolManager::supportsWriting(url)) {
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
            info.stamp.mtimeNs = mtime < 0 ? -1 : mtime * 1000000000;
            info.writable = access < 0 || (access & 0222) != 0;
        }
        done(info);
    });
    return job;
}
} // namespace Remote
