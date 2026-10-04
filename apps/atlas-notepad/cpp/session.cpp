#include "session.h"

#include "document_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QUuid>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr int formatVersion = 1;
// What a session may hold, against a corrupt or planted file. Far past
// what Notepad writes (files open up to 10 MiB), so nothing of ours is cut.
constexpr qint64 maxJsonBytes = 64 << 20;
constexpr qint64 maxTextBytes = qint64(1) << 30;
constexpr int maxWindows = 1000;
constexpr int maxTabs = 20000;
constexpr int maxCoordinate = 1 << 20;

// Atomically and readable only by us, UTF-8 (np_file_save_private: temp
// file 0600, fsync, rename, never through a symlink).
// A lone surrogate (from a lossy UTF-16 file or a paste) has no UTF-8 form
// and would fail the write every time, so it is stored as U+FFFD.
int saveUtf8(const QString &path, QString text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            ++i;
        } else if (text.at(i).isSurrogate()) {
            text[i] = QChar::ReplacementCharacter;
        }
    }
    const QByteArray bytes = text.toUtf8();
    return np_file_save_private(QFile::encodeName(path).constData(), reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()));
}

QString errorText(int error)
{
    return QString::fromLocal8Bit(strerror(error));
}

// session.json moved aside to session.json.bak, replacing an older backup.
// Copied first, so a failed copy keeps an older backup.
void backUp(const QString &jsonPath)
{
    const QString bak = jsonPath + QStringLiteral(".bak");
    QFile::remove(bak + QStringLiteral(".new"));
    const QByteArray to = QFile::encodeName(bak);
    if (QFileInfo(jsonPath).isFile() && QFile::copy(jsonPath, bak + QStringLiteral(".new"))) {
        std::rename(QFile::encodeName(bak + QStringLiteral(".new")).constData(), to.constData());
    } else {
        // A full disk (or not a file at all): moving it aside needs no
        // room. It replaces an older backup, so this run's session can
        // be written without losing the newest unusable one.
        std::rename(QFile::encodeName(jsonPath).constData(), to.constData());
    }
}

// The unsaved text goes here: a directory of ours, 0700. (A link to one,
// as dotfile setups make, is fine: only we can put a link in our state dir.)
bool makePrivate(const QString &dir)
{
    const QByteArray path = QFile::encodeName(dir);
    struct stat st = {};
    if (stat(path.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
        return false;
    }
    return (st.st_mode & 07777) == 0700 || chmod(path.constData(), 0700) == 0;
}

// A regular file only (a FIFO would block the open), at most `max` bytes;
// nothing when it isn't one or is larger.
std::optional<QByteArray> readCapped(const QString &path, qint64 max)
{
    if (!QFileInfo(path).isFile()) {
        return std::nullopt;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > max) {
        return std::nullopt;
    }
    QByteArray bytes = file.read(qMin(max, file.size()) + 1);
    if (bytes.size() > max) {
        return std::nullopt;
    }
    return bytes;
}

void removeTemps(const QString &path)
{
    QDir dir(path);
    for (const QString &name : dir.entryList({QStringLiteral(".*.tmp")}, QDir::Files | QDir::Hidden)) {
        dir.remove(name);
    }
}

// 64-bit numbers don't survive a JSON double: strings.
QJsonObject stampToJson(const NpStamp &s)
{
    return {{QStringLiteral("mtimeNs"), QString::number(s.mtimeNs)},
            {QStringLiteral("size"), QString::number(s.size)},
            {QStringLiteral("dev"), QString::number(s.dev)},
            {QStringLiteral("ino"), QString::number(s.ino)}};
}

NpStamp stampFromJson(const QJsonObject &o)
{
    NpStamp s = {};
    s.mtimeNs = o.value(QStringLiteral("mtimeNs")).toString().toLongLong();
    s.size = o.value(QStringLiteral("size")).toString().toULongLong();
    s.dev = o.value(QStringLiteral("dev")).toString().toULongLong();
    s.ino = o.value(QStringLiteral("ino")).toString().toULongLong();
    return s;
}

QString textPath(const QString &file)
{
    return Session::directory() + QStringLiteral("/texts/") + file + QStringLiteral(".txt");
}

struct TextWrite {
    QString file;
    QString text;
};
}

Session::Session()
{
    m_pool.setMaxThreadCount(1);
}

Session::~Session()
{
    m_pool.waitForDone();
    if (m_lockFd >= 0) {
        ::close(m_lockFd);
    }
}

QString Session::directory()
{
    // XDG state: kept across restarts, but not documents or settings.
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/atlas-notepad/session");
}

bool Session::lock()
{
    if (m_lockFd >= 0) {
        return true;
    }
    // Beside the session folder, not in it: remove() deletes that.
    const QString parent = QFileInfo(directory()).path();
    QDir().mkpath(parent);
    const QByteArray path = QFile::encodeName(parent + QStringLiteral("/session.lock"));
    const int fd = ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) {
        qWarning("atlas-notepad: can't open %s: %s", path.constData(), strerror(errno));
        return true;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const bool held = errno == EWOULDBLOCK;
        ::close(fd);
        return !held;
    }
    m_lockFd = fd;
    return true;
}

int Session::restoreDeaths()
{
    moveOld();
    if (const std::optional<QByteArray> bytes = readCapped(directory() + QStringLiteral("/restoring"), 16)) {
        return qBound(0, bytes->trimmed().toInt(), 1000);
    }
    return 0;
}

void Session::beginRestore(int deaths)
{
    const QString dir = directory();
    const QString path = dir + QStringLiteral("/restoring");
    // No fsync: it only has to outlive the process, not the machine, and
    // this is on the way to the first frame.
    QFile file(path);
    if (!QDir().mkpath(dir) || !makePrivate(dir) || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(QByteArray::number(deaths + 1)) <= 0) {
        qWarning("atlas-notepad: can't write %s; a crash while restoring won't be noticed", qPrintable(path));
    }
}

void Session::endRestore()
{
    QFile::remove(directory() + QStringLiteral("/restoring"));
}

void Session::setAside()
{
    // Moved, not copied: the session that kills Notepad mustn't stay.
    const QString jsonPath = directory() + QStringLiteral("/session.json");
    if (QFileInfo::exists(jsonPath)) {
        std::rename(QFile::encodeName(jsonPath).constData(), QFile::encodeName(jsonPath + QStringLiteral(".bak")).constData());
    }
    m_readFailed = true; // its texts stay
}

// Before 0.1 the session lived in $XDG_DATA_HOME/atlas-notepad; move it once,
// before anything makes the new folder.
void Session::moveOld()
{
    const QString old = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/atlas-notepad");
    if (!QFileInfo::exists(directory()) && QFileInfo::exists(old + QStringLiteral("/session.json"))) {
        QDir().mkpath(QFileInfo(directory()).path());
        if (!QDir().rename(old, directory())) {
            qWarning("atlas-notepad: couldn't move the old session from %s", qUtf8Printable(old));
        }
    }
}

QList<WindowState> Session::read()
{
    moveOld();
    // Interrupted writes leave .<name>.<hex>.tmp behind; nothing is writing yet.
    removeTemps(directory());
    removeTemps(directory() + QStringLiteral("/texts"));
    const QString jsonPath = directory() + QStringLiteral("/session.json");
    if (!QFileInfo::exists(jsonPath)) {
        return {};
    }
    const std::optional<QByteArray> bytes = readCapped(jsonPath, maxJsonBytes);
    const QJsonDocument doc = bytes ? QJsonDocument::fromJson(*bytes) : QJsonDocument();
    if (!doc.isObject() || doc.object().value(QStringLiteral("version")).toInt() != formatVersion) {
        qWarning("atlas-notepad: can't use the session file, keeping a copy as session.json.bak");
        m_readFailed = true;
        backUp(jsonPath);
        return {};
    }
    QList<WindowState> windows;
    int tabs = 0;
    for (const QJsonValue &wv : doc.object().value(QStringLiteral("windows")).toArray()) {
        if (windows.size() >= maxWindows || tabs >= maxTabs) {
            break;
        }
        const QJsonObject w = wv.toObject();
        WindowState state;
        const QJsonArray g = w.value(QStringLiteral("geometry")).toArray();
        if (g.size() == 4) {
            auto at = [&g](int i, int low) {
                return qBound(low, g[i].toInt(), maxCoordinate);
            };
            state.geometry = QRect(at(0, -maxCoordinate), at(1, -maxCoordinate), at(2, 0), at(3, 0));
        }
        state.maximized = w.value(QStringLiteral("maximized")).toBool();
        state.currentIndex = w.value(QStringLiteral("currentIndex")).toInt();
        for (const QJsonValue &tv : w.value(QStringLiteral("tabs")).toArray()) {
            if (tabs >= maxTabs) {
                break;
            }
            const QJsonObject t = tv.toObject();
            TabState tab;
            tab.path = t.value(QStringLiteral("path")).toString();
            tab.encoding = t.value(QStringLiteral("encoding")).toInt(NP_UTF8);
            tab.lineEnding = t.value(QStringLiteral("lineEnding")).toInt(NP_LF);
            tab.formatted = t.value(QStringLiteral("formatted")).toBool();
            tab.modified = t.value(QStringLiteral("modified")).toBool();
            tab.cursor = t.value(QStringLiteral("cursor")).toInt();
            tab.anchor = t.value(QStringLiteral("anchor")).toInt();
            tab.scrollY = t.value(QStringLiteral("scrollY")).toDouble();
            if (t.value(QStringLiteral("language")).isString()) {
                tab.language = t.value(QStringLiteral("language")).toString().left(100);
            }
            if (t.value(QStringLiteral("insertSpaces")).isBool()) {
                tab.insertSpaces = t.value(QStringLiteral("insertSpaces")).toBool();
            }
            if (t.value(QStringLiteral("indentWidth")).isDouble()) {
                tab.indentWidth = qBound(1, t.value(QStringLiteral("indentWidth")).toInt(4), 16);
            }
            if (t.contains(QStringLiteral("stamp"))) {
                tab.hasStamp = true;
                tab.stamp = stampFromJson(t.value(QStringLiteral("stamp")).toObject());
            }
            tab.textFile = t.value(QStringLiteral("textFile")).toString();
            if (tab.encoding < NP_UTF8 || tab.encoding > NP_WINDOWS_1252 || tab.lineEnding < NP_LF || tab.lineEnding > NP_CR) {
                continue;
            }
            state.tabs.append(tab);
            ++tabs;
        }
        if (!state.tabs.isEmpty()) {
            state.currentIndex = qBound(0, state.currentIndex, int(state.tabs.size()) - 1);
            windows.append(state);
        }
    }
    return windows;
}

std::optional<QString> Session::readText(const QString &textFile) const
{
    // Only names we made: no path separators.
    if (textFile.isEmpty() || textFile != QFileInfo(textFile).fileName()) {
        return std::nullopt;
    }
    const std::optional<QByteArray> bytes = readCapped(textPath(textFile), maxTextBytes);
    if (!bytes) {
        return std::nullopt;
    }
    return QString::fromUtf8(*bytes);
}

Document *Session::restoreTab(DocumentList *list, const TabState &tab, bool safe)
{
    Document *doc = list->append();
    doc->d->safeMode = safe;
    doc->d->safeFormatted = tab.formatted;
    std::optional<QString> text;
    const bool wantsText = !tab.textFile.isEmpty() && (tab.modified || tab.path.isEmpty());
    if (wantsText) {
        text = readText(tab.textFile);
        if (!text) {
            qWarning("atlas-notepad: the session's text %s is missing; the tab's unsaved changes are lost", qPrintable(tab.textFile));
        }
    }
    doc->d->restore(tab, text);
    if (wantsText && !text) {
        if (tab.modified && tab.path.isEmpty()) {
            doc->d->modified = true; // a named tab shows its file again instead
        }
        doc->d->setBanner(Document::Unrecovered);
    }
    if (text) {
        m_texts.insert(doc->d->id, {tab.textFile, doc->d->sessionKey()});
    }
    return doc;
}

void Session::dropFailed()
{
    QSet<QString> failed;
    {
        QMutexLocker lock(&m_mutex);
        failed.swap(m_failed);
    }
    if (failed.isEmpty()) {
        return;
    }
    for (auto it = m_texts.begin(); it != m_texts.end();) {
        it = failed.contains(it->file) ? m_texts.erase(it) : std::next(it);
    }
}

void Session::write(const QList<Live> &windows, bool wait)
{
    dropFailed(); // a text that failed last time is written again
    QJsonArray windowsJson;
    QList<TextWrite> writes;
    QSet<QString> referenced;
    QSet<quint64> alive;

    for (const Live &live : windows) {
        QJsonArray tabs;
        for (Document *doc : live.list->documents()) {
            Document::Private *p = doc->d.get();
            alive.insert(p->id);
            QJsonObject t;
            t[QStringLiteral("path")] = p->path;
            t[QStringLiteral("encoding")] = int(p->encoding);
            t[QStringLiteral("lineEnding")] = int(p->lineEnding);
            // A tab opened plain after a crash keeps its stored view for next
            // time (a switch made in this plain run isn't kept).
            t[QStringLiteral("formatted")] = p->safeMode ? p->safeFormatted : p->formatted;
            t[QStringLiteral("modified")] = p->isModified();
            t[QStringLiteral("cursor")] = doc->cursorPosition();
            t[QStringLiteral("anchor")] = doc->selectionAnchor();
            t[QStringLiteral("scrollY")] = double(doc->scrollY());
            if (p->userLanguage) {
                t[QStringLiteral("language")] = p->language;
            }
            if (p->userIndent) {
                t[QStringLiteral("insertSpaces")] = p->insertSpaces;
                t[QStringLiteral("indentWidth")] = p->indentWidth;
            }
            if (p->hasStamp) {
                t[QStringLiteral("stamp")] = stampToJson(p->stamp);
            }

            const bool needsText = !p->loading && p->loaded && (p->path.isEmpty() || p->isModified());
            if (needsText) {
                const quint64 key = p->sessionKey();
                auto it = m_texts.find(p->id);
                if (it == m_texts.end() || it->key != key) {
                    const QString text = doc->text();
                    if (text.isEmpty() && p->path.isEmpty()) {
                        // Nothing to keep of an empty untitled tab.
                        m_texts.remove(p->id);
                    } else {
                        const QString file = it == m_texts.end() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : it->file;
                        writes.append({file, text});
                        m_texts.insert(p->id, {file, key});
                    }
                }
                const auto now = m_texts.constFind(p->id);
                if (now != m_texts.constEnd()) {
                    t[QStringLiteral("textFile")] = now->file;
                    referenced.insert(now->file);
                }
            } else {
                m_texts.remove(p->id);
            }
            tabs.append(t);
        }
        QJsonObject w;
        w[QStringLiteral("geometry")] = QJsonArray{live.geometry.x(), live.geometry.y(), live.geometry.width(), live.geometry.height()};
        w[QStringLiteral("maximized")] = live.maximized;
        w[QStringLiteral("currentIndex")] = live.list->currentIndex();
        w[QStringLiteral("tabs")] = tabs;
        windowsJson.append(w);
    }
    for (auto it = m_texts.begin(); it != m_texts.end();) {
        it = alive.contains(it.key()) ? std::next(it) : m_texts.erase(it);
    }

    const QJsonObject root{{QStringLiteral("version"), formatVersion}, {QStringLiteral("windows"), windowsJson}};
    const QString json = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
    const QString dir = directory();
    const bool cleanup = !m_readFailed;
    QObject *context = m_context.data();
    const std::function<void()> written = m_written;
    m_pool.start([this, dir, json, writes, referenced, cleanup, context, written] {
        auto fail = [this](const QString &file) {
            QMutexLocker lock(&m_mutex);
            m_failed.insert(file);
        };
        // Reported once the write is over, however it ends.
        QString error;
        const auto report = qScopeGuard([&] {
            {
                QMutexLocker lock(&m_mutex);
                m_lastError = error;
            }
            if (context && written) {
                QMetaObject::invokeMethod(context, written, Qt::QueuedConnection);
            }
        });
        errno = 0;
        if (!QDir().mkpath(dir + QStringLiteral("/texts"))) {
            // mkpath doesn't promise errno; without one, the folder message.
            error = errno ? errorText(errno) : QObject::tr("%1 isn't a folder Notepad can use.").arg(dir);
        } else if (!makePrivate(dir) || !makePrivate(dir + QStringLiteral("/texts"))) {
            error = QObject::tr("%1 isn't a folder Notepad can use.").arg(dir);
        }
        if (!error.isEmpty()) {
            qWarning("atlas-notepad: can't keep the session private in %s (not a folder of ours, or chmod failed)", qPrintable(dir));
            for (const TextWrite &w : writes) {
                fail(w.file);
            }
            return;
        }
        for (const TextWrite &w : writes) {
            if (const int rc = saveUtf8(textPath(w.file), w.text)) {
                qWarning("atlas-notepad: can't write the session's text %s", qPrintable(w.file));
                fail(w.file);
                error = errorText(rc);
            }
        }
        if (!error.isEmpty()) {
            return; // session.json would point at missing texts
        }
        if (const int rc = saveUtf8(dir + QStringLiteral("/session.json"), json)) {
            qWarning("atlas-notepad: can't write the session");
            error = errorText(rc);
            return;
        }
        if (!cleanup) {
            return;
        }
        // Texts the backup names stay, for whoever recovers it.
        QSet<QString> keep = referenced;
        const QString bakPath = dir + QStringLiteral("/session.json.bak");
        if (QFileInfo::exists(bakPath)) {
            const std::optional<QByteArray> bak = readCapped(bakPath, maxJsonBytes);
            if (!bak) {
                return; // can't tell which texts it names
            }
            const QJsonDocument doc = QJsonDocument::fromJson(*bak);
            if (doc.object().value(QStringLiteral("version")).toInt() > formatVersion) {
                return; // a newer Notepad's; its texts can't be told apart here
            }
            for (const QJsonValue &w : doc.object().value(QStringLiteral("windows")).toArray()) {
                for (const QJsonValue &t : w.toObject().value(QStringLiteral("tabs")).toArray()) {
                    keep.insert(t.toObject().value(QStringLiteral("textFile")).toString());
                }
            }
        }
        QDir texts(dir + QStringLiteral("/texts"));
        for (const QString &name : texts.entryList({QStringLiteral("*.txt")}, QDir::Files)) {
            if (!keep.contains(QFileInfo(name).completeBaseName())) {
                texts.remove(name);
            }
        }
    });
    if (wait) {
        m_pool.waitForDone();
        dropFailed();
    }
}

QString Session::lastError() const
{
    QMutexLocker lock(&m_mutex);
    return m_lastError;
}

void Session::onWritten(QObject *context, std::function<void()> callback)
{
    m_context = context;
    m_written = std::move(callback);
}

void Session::remove()
{
    m_pool.waitForDone();
    m_texts.clear();
    QDir(directory()).removeRecursively();
    QMutexLocker lock(&m_mutex);
    m_lastError.clear();
}
