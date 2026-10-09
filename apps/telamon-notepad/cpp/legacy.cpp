#include "legacy.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QVariantMap>

#include <cerrno>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Legacy
{
QString configFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/atlas-notepadrc");
}

QString stateDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/atlas-notepad");
}

QString dataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/atlas-notepad");
}

const QString &appId()
{
    static const QString id = QStringLiteral("net.eterneon.atlas.notepad");
    return id;
}

bool moveNoReplace(const QString &from, const QString &to)
{
    const QByteArray src = QFile::encodeName(from);
    const QByteArray dst = QFile::encodeName(to);
    if (renameat2(AT_FDCWD, src.constData(), AT_FDCWD, dst.constData(), RENAME_NOREPLACE) == 0) {
        return true;
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return false;
    }
    // A file system without RENAME_NOREPLACE: look first (a window of a few
    // microseconds in which another Notepad could do the same move).
    struct stat st = {};
    if (lstat(dst.constData(), &st) == 0) {
        errno = EEXIST;
        return false;
    }
    return rename(src.constData(), dst.constData()) == 0;
}

bool sessionHeldByOldNotepad()
{
    const QByteArray path = QFile::encodeName(stateDir() + QStringLiteral("/session.lock"));
    // Not O_CREAT: no old Notepad ever ran here if there is no lock file.
    const int fd = ::open(path.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(fd); // closing drops our lock, if we got it
    return held;
}

bool forwardLaunch(const QStringList &arguments)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface() || !bus.interface()->isServiceRegistered(appId())) {
        return false;
    }
    // What KDBusService::Unique's second launch sends the first.
    QVariantMap platformData;
    const QByteArray token = qgetenv("XDG_ACTIVATION_TOKEN");
    if (!token.isEmpty()) {
        platformData.insert(QStringLiteral("activation-token"), QString::fromLocal8Bit(token));
    }
    const QByteArray startupId = qgetenv("DESKTOP_STARTUP_ID");
    if (!startupId.isEmpty()) {
        platformData.insert(QStringLiteral("desktop-startup-id"), QString::fromLocal8Bit(startupId));
    }
    // KDBusService serves org.kde.KDBusService at the path of its name
    // (net.eterneon.atlas.notepad -> /net/eterneon/atlas/notepad); the
    // /MainApplication it also exports is the Qt application object, which
    // has no CommandLine.
    QString path = appId();
    path.replace(QLatin1Char('.'), QLatin1Char('/')).prepend(QLatin1Char('/'));
    QDBusMessage call = QDBusMessage::createMethodCall(appId(), path, QStringLiteral("org.kde.KDBusService"), QStringLiteral("CommandLine"));
    call << arguments << QDir::currentPath() << platformData;
    return bus.call(call, QDBus::Block, 10000).type() == QDBusMessage::ReplyMessage;
}
}
