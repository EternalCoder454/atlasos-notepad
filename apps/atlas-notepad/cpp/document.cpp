// One tab: reading and saving through the Rust file layer on worker threads,
// watching the file, the banners, find and replace, and the counts. The text
// lives in the TextEdit's QTextDocument once there is one.
#include "document_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QtConcurrent>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{
quint64 s_nextId = 1;

// Counting happens on the main thread for texts up to this size.
constexpr qsizetype countInlineLimit = 256 * 1024;
constexpr int findCap = 10000;

bool isMarkdownName(const QString &path)
{
    static const QStringList extensions = {QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("mdown"), QStringLiteral("mkd")};
    return extensions.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive);
}

// Text for people, not code or config: spell check reads it.
bool isProseName(const QString &path)
{
    static const QStringList extensions = {QStringLiteral("txt"), QStringLiteral("text")};
    const QString suffix = QFileInfo(path).suffix();
    return path.isEmpty() || suffix.isEmpty() || isMarkdownName(path) || extensions.contains(suffix, Qt::CaseInsensitive);
}

Counts countText(const QString &text)
{
    Counts c;
    bool inWord = false;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\n')) {
            ++c.lines;
            inWord = false;
            continue;
        }
        if (ch.isLowSurrogate()) {
            continue;
        }
        ++c.characters;
        const bool space = ch.isSpace();
        if (!space && !inWord) {
            ++c.words;
        }
        inWord = !space;
    }
    return c;
}

bool hasLongLine(const QString &text)
{
    qsizetype start = 0;
    while (start <= text.size()) {
        qsizetype end = text.indexOf(QLatin1Char('\n'), start);
        if (end < 0) {
            end = text.size();
        }
        if (end - start > Limits::lineLength) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

// Turns CRLF and CR into "\n", noting which one the text used.
void normalizeEndings(QString &text, int &ending, bool &mixed)
{
    qsizetype crlf = 0, lf = 0, cr = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('\r')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (ch == QLatin1Char('\n')) {
            ++lf;
        }
    }
    ending = NP_LF;
    qsizetype best = lf;
    if (crlf > best) {
        ending = NP_CRLF;
        best = crlf;
    }
    if (cr > best) {
        ending = NP_CR;
    }
    mixed = (crlf > 0) + (lf > 0) + (cr > 0) > 1;
    if (crlf || cr) {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    }
}

QString decodeWindows1252(const QByteArray &bytes)
{
    static const char16_t high[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                      0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    QString out;
    out.reserve(bytes.size());
    for (const char b : bytes) {
        const auto u = uchar(b);
        out.append(u >= 0x80 && u < 0xA0 ? QChar(high[u - 0x80]) : QChar(u));
    }
    return out;
}

// Reading as an encoding the user chose (np_file_read detects its own).
LoadResult readAs(const QByteArray &path, int encoding, LoadResult r)
{
    // Non-blocking, and checked on the open file: a FIFO swapped in since
    // readFile looked mustn't hang this worker.
    const int fd = ::open(path.constData(), O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        r.error = errno;
        return r;
    }
    struct stat st = {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        r.error = EINVAL;
        return r;
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        r.error = EIO;
        return r;
    }
    if (st.st_size > Limits::fileBytes) {
        r.tooLarge = true;
        return r;
    }
    QByteArray bytes = file.read(Limits::fileBytes + 1);
    if (bytes.size() > Limits::fileBytes) {
        r.tooLarge = true;
        return r;
    }
    r.encoding = encoding;
    switch (encoding) {
    case NP_UTF8_BOM:
    case NP_UTF8:
        if (bytes.startsWith("\xEF\xBB\xBF")) {
            bytes.remove(0, 3);
        }
        r.binary = bytes.left(65536).contains('\0');
        {
            // Invalid bytes become U+FFFD; saving would lose the originals.
            QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
            r.text = decoder.decode(bytes);
            r.lossy = decoder.hasError();
        }
        break;
    case NP_UTF16_LE:
    case NP_UTF16_BE: {
        const bool big = encoding == NP_UTF16_BE;
        qsizetype from = 0;
        if (bytes.size() >= 2 && ((big && bytes.startsWith("\xFE\xFF")) || (!big && bytes.startsWith("\xFF\xFE")))) {
            from = 2;
        }
        const qsizetype units = (bytes.size() - from) / 2;
        QString text(units, Qt::Uninitialized);
        for (qsizetype i = 0; i < units; ++i) {
            const auto a = uchar(bytes[from + 2 * i]);
            const auto b = uchar(bytes[from + 2 * i + 1]);
            text[i] = QChar(big ? char16_t(a << 8 | b) : char16_t(b << 8 | a));
        }
        r.lossy = (bytes.size() - from) % 2 != 0;
        for (qsizetype i = 0; i < units && !r.lossy; ++i) {
            const QChar c = text.at(i);
            if (c.isHighSurrogate()) {
                r.lossy = i + 1 >= units || !text.at(i + 1).isLowSurrogate();
                ++i;
            } else if (c.isLowSurrogate()) {
                r.lossy = true;
            }
        }
        r.text = text;
        break;
    }
    default:
        r.binary = bytes.left(65536).contains('\0');
        r.text = decodeWindows1252(bytes);
        break;
    }
    bool mixed = false;
    normalizeEndings(r.text, r.lineEnding, mixed);
    // The editor splits lines at U+2029 too, so saving writes it as a line end.
    r.mixed = mixed || r.text.contains(QChar::ParagraphSeparator);
    r.longLines = hasLongLine(r.text);
    return r;
}

// Runs on a worker.
LoadResult readFile(const QByteArray &path, int forcedEncoding)
{
    LoadResult r;
    NpStamp stamp = {};
    r.error = np_file_stamp(path.constData(), &stamp);
    if (r.error) {
        return r;
    }
    r.stamp = stamp;
    // Regular files only: a FIFO would hang the read, /dev/zero never end.
    if (!QFileInfo(QFile::decodeName(path)).isFile()) {
        r.error = EINVAL;
        return r;
    }
    if (qint64(stamp.size) > Limits::fileBytes) {
        r.tooLarge = true;
        return r;
    }
    if (forcedEncoding >= 0) {
        return readAs(path, forcedEncoding, r);
    }
    // np_file_read checks both again on the open file.
    NpFile *file = np_file_read(path.constData(), quint64(Limits::fileBytes));
    if (file->error == EFBIG) {
        r.tooLarge = true;
        np_file_free(file);
        return r;
    }
    if (file->error || !file->text) {
        r.error = file->error ? file->error : EIO;
        np_file_free(file);
        return r;
    }
    r.text = QString::fromUtf16(reinterpret_cast<const char16_t *>(file->text), qsizetype(file->len));
    r.encoding = file->encoding;
    r.lineEnding = file->lineEnding;
    r.mixed = file->mixed || r.text.contains(QChar::ParagraphSeparator);
    r.binary = file->binary;
    r.lossy = file->lossy;
    r.stamp = file->stamp;
    np_file_free(file);
    r.longLines = hasLongLine(r.text);
    return r;
}

QString errnoText(int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
        return QObject::tr("You don't have permission to save here.");
    case ENOENT:
        return QObject::tr("The folder doesn't exist.");
    case ENOSPC:
        return QObject::tr("The disk is full.");
    case EROFS:
        return QObject::tr("The disk is read-only.");
    case EISDIR:
        return QObject::tr("That is a folder, not a file.");
    case ENAMETOOLONG:
        return QObject::tr("The file name is too long.");
    case EDQUOT:
        return QObject::tr("Your disk quota is used up.");
    default:
        return QString::fromLocal8Bit(strerror(error));
    }
}

QString readErrorText(int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
        return QObject::tr("You don't have permission to read it.");
    case EINVAL:
        return QObject::tr("It isn't a regular file.");
    case EIO:
        return QObject::tr("The disk couldn't be read.");
    default:
        return QString::fromLocal8Bit(strerror(error));
    }
}

// Find: a plain search, or a regular expression for the other flags.
struct Matcher {
    QRegularExpression re;
    QString plain;
    Qt::CaseSensitivity cs = Qt::CaseInsensitive;
    bool useRegex = false;
    bool expand = false; // \0..\9 in replacements
    QString error;
};

Matcher makeMatcher(const QString &text, int flags)
{
    Matcher m;
    m.cs = (flags & Document::MatchCase) ? Qt::CaseSensitive : Qt::CaseInsensitive;
    m.expand = flags & Document::RegularExpression;
    m.useRegex = (flags & Document::RegularExpression) || (flags & Document::WholeWords);
    if (!m.useRegex) {
        m.plain = text;
        return m;
    }
    QString pattern = (flags & Document::RegularExpression) ? text : QRegularExpression::escape(text);
    if (flags & Document::WholeWords) {
        // Not \b: a term that starts or ends with a symbol ("c++") would
        // then never match before a space.
        pattern = QStringLiteral("(?<!\\w)(?:") + pattern + QStringLiteral(")(?!\\w)");
    }
    QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
    if (!(flags & Document::MatchCase)) {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    if (flags & Document::RegularExpression) {
        options |= QRegularExpression::MultilineOption;
    }
    m.re = QRegularExpression(pattern, options);
    if (!m.re.isValid()) {
        m.error = m.re.errorString();
    }
    return m;
}

using Span = std::pair<int, int>;

// Matches in order from `from`, at most `cap`; empty matches are skipped.
void collect(const QString &t, const Matcher &m, qsizetype from, int cap, std::vector<Span> &out)
{
    if (m.useRegex) {
        auto it = m.re.globalMatch(t, from);
        while (it.hasNext() && int(out.size()) < cap) {
            const QRegularExpressionMatch match = it.next();
            if (match.capturedLength() > 0) {
                out.emplace_back(int(match.capturedStart()), int(match.capturedEnd()));
            }
        }
        return;
    }
    qsizetype pos = from;
    while (int(out.size()) < cap) {
        pos = t.indexOf(m.plain, pos, m.cs);
        if (pos < 0) {
            break;
        }
        out.emplace_back(int(pos), int(pos + m.plain.size()));
        pos += m.plain.size();
    }
}

QString replacementFor(const Matcher &m, const QString &t, Span span, const QString &replacement)
{
    if (!m.expand || !replacement.contains(QLatin1Char('\\'))) {
        return replacement;
    }
    const QRegularExpressionMatch match = m.re.match(t, span.first, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    QString out;
    for (qsizetype i = 0; i < replacement.size(); ++i) {
        const QChar ch = replacement.at(i);
        if (ch == QLatin1Char('\\') && i + 1 < replacement.size()) {
            const QChar next = replacement.at(i + 1);
            if (next.isDigit()) {
                out += match.captured(next.digitValue());
                ++i;
                continue;
            }
            if (next == QLatin1Char('\\')) {
                out += QLatin1Char('\\');
                ++i;
                continue;
            }
        }
        out += ch;
    }
    return out;
}

QVariantMap error(const QString &message)
{
    return {{QStringLiteral("error"), message}};
}

QVariantMap noMatch()
{
    return {{QStringLiteral("count"), 0}};
}
}

bool sameStamp(const NpStamp &a, const NpStamp &b)
{
    return a.mtimeNs == b.mtimeNs && a.size == b.size && a.dev == b.dev && a.ino == b.ino;
}

// ---------------------------------------------------------------- Private

Document::Private::Private(Document *document, DocumentList *owner)
    : q(document)
    , list(owner)
    , id(s_nextId++)
{
    markdown = settings().formatting();
    formatted = markdown && settings().openMarkdownFormatted();
    countTimer.setSingleShot(true);
    countTimer.setInterval(300);
    QObject::connect(&countTimer, &QTimer::timeout, q, [this] { updateCounts(); });
    diskTimer.setSingleShot(true);
    diskTimer.setInterval(100);
    QObject::connect(&diskTimer, &QTimer::timeout, q, [this] { checkOnDisk(); });
}

Settings &Document::Private::settings() const
{
    if (App::instance()) {
        return *App::instance()->settings();
    }
    static Settings fallback;
    return fallback;
}

bool Document::Private::isModified() const
{
    return qdoc ? qdoc->isModified() : modified;
}

void Document::Private::setModified(bool on)
{
    if (qdoc) {
        qdoc->setModified(on);
    } else if (modified != on) {
        modified = on;
        Q_EMIT q->modifiedChanged();
    }
}

void Document::Private::setBanner(Banner banner, const QString &text)
{
    const Banner before = q->banner();
    const QString textBefore = q->bannerText();
    banners[banner] = text;
    if (before != q->banner() || textBefore != q->bannerText()) {
        Q_EMIT q->bannerChanged();
    }
}

void Document::Private::clearBanner(Banner banner)
{
    if (banners.erase(banner)) {
        Q_EMIT q->bannerChanged();
    }
}

qint64 Document::Private::currentBytes() const
{
    if (hasStamp) {
        return qint64(stamp.size);
    }
    return qdoc ? qdoc->characterCount() : pending.size();
}

void Document::Private::applyMarkdown(qint64 bytes)
{
    const bool nameOk = !safeMode && (path.isEmpty() || isMarkdownName(path));
    const bool formattingOn = settings().formatting();
    const bool tooBig = bytes > Limits::formattedBytes;
    const bool now = nameOk && formattingOn && !tooBig;
    if (nameOk && !path.isEmpty() && formattingOn && tooBig) {
        setBanner(FormattingOff);
    } else {
        clearBanner(FormattingOff);
    }
    if (now != markdown) {
        markdown = now;
        Q_EMIT q->markdownChanged();
    }
    const bool isProse = !safeMode && isProseName(path) && !tooBig;
    if (isProse != prose) {
        prose = isProse;
        Q_EMIT q->proseChanged();
    }
    if (!markdown && formatted) {
        formatted = false;
        Q_EMIT q->formattedChanged();
    }
}

void Document::Private::checkWritable()
{
    if (!path.isEmpty() && QFileInfo::exists(path) && ::access(path.toUtf8().constData(), W_OK) != 0) {
        setBanner(ReadOnlyFile);
    } else {
        clearBanner(ReadOnlyFile);
    }
}

quint64 Document::Private::revision() const
{
    // Not QTextDocument::revision(): undo takes that back, so new text
    // could carry an old number.
    return contentVersion;
}

quint64 Document::Private::sessionKey() const
{
    return contentVersion;
}

void Document::Private::connectTextDocument()
{
    QObject::connect(qdoc, &QTextDocument::modificationChanged, q, [this] { Q_EMIT q->modifiedChanged(); });
    QObject::connect(qdoc, &QTextDocument::contentsChanged, q, [this] {
        ++contentVersion;
        if (settingText) {
            return;
        }
        scheduleCounts();
        Q_EMIT q->edited();
    });
}

QQuickItem *Document::Private::flickable() const
{
    for (QQuickItem *item = textEdit ? textEdit->parentItem() : nullptr; item; item = item->parentItem()) {
        const QMetaObject *mo = item->metaObject();
        if (mo->indexOfProperty("contentY") >= 0 && mo->indexOfProperty("contentHeight") >= 0) {
            return item;
        }
    }
    return nullptr;
}

void Document::Private::applyView()
{
    if (!textEdit || !qdoc) {
        return;
    }
    const int length = qMax(0, qdoc->characterCount() - 1);
    const int c = qBound(0, cursor, length);
    const int a = qBound(0, anchor, length);
    if (a != c) {
        QMetaObject::invokeMethod(textEdit, "select", Q_ARG(int, a), Q_ARG(int, c));
    } else if (c != 0 || textEdit->property("cursorPosition").toInt() != 0) {
        textEdit->setProperty("cursorPosition", c);
    }
    if (scrollY > 0) {
        QPointer<QQuickItem> flick = flickable();
        const qreal y = scrollY;
        if (flick) {
            QTimer::singleShot(0, flick, [flick, y] {
                // Never past the end: a stale or bad value would show nothing.
                const qreal end = qMax(0.0, flick->property("contentHeight").toReal() - flick->height());
                flick->setProperty("contentY", qBound(0.0, y, end));
            });
        }
    }
}

void Document::Private::putText(const QString &text, bool keepView)
{
    if (textEdit && qdoc) {
        // Setting the text moves the caret to 0, and the view saves that
        // back through setCursorPosition; restore from copies.
        const int c = keepView ? q->cursorPosition() : cursor;
        const int an = keepView ? q->selectionAnchor() : anchor;
        const qreal y = keepView ? q->scrollY() : scrollY;
        settingText = true;
        textEdit->setProperty("text", text);
        qdoc->setModified(false);
        settingText = false;
        cursor = c;
        anchor = an;
        scrollY = y;
        applyView();
    } else {
        pending = text;
        hasPending = true;
        ++contentVersion;
        setModified(false);
    }
}

void Document::Private::scheduleCounts()
{
    countTimer.start();
}

void Document::Private::updateCounts()
{
    const qsizetype size = qdoc ? qdoc->characterCount() : pending.size();
    auto apply = [this](const Counts &c) {
        if (c.characters != counts.characters || c.words != counts.words || c.lines != counts.lines) {
            counts = c;
            Q_EMIT q->countsChanged();
        }
    };
    if (size <= countInlineLimit) {
        ++countGeneration;
        apply(countText(q->text()));
        return;
    }
    const int gen = ++countGeneration;
    auto *watcher = new QFutureWatcher<Counts>(q);
    QObject::connect(watcher, &QFutureWatcherBase::finished, q, [this, watcher, gen, apply] {
        const Counts c = watcher->result();
        watcher->deleteLater();
        if (gen == countGeneration) {
            apply(c);
        }
    });
    watcher->setFuture(QtConcurrent::run([text = q->text()] { return countText(text); }));
}

// ------------------------------------------------------------- File watch

namespace
{
QMultiHash<QString, Document *> &watchedDocuments()
{
    static QMultiHash<QString, Document *> docs;
    return docs;
}

// Tabs whose file is missing, by its folder: inotify can't watch a file
// that isn't there, so the folder says when it comes back.
QMultiHash<QString, Document *> &waitingDocuments()
{
    static QMultiHash<QString, Document *> docs;
    return docs;
}

// One watcher for all tabs (each QFileSystemWatcher holds an inotify instance).
QPointer<QFileSystemWatcher> &sharedWatcher()
{
    static QPointer<QFileSystemWatcher> watcher;
    return watcher;
}

void stopWaiting(Document *doc)
{
    for (auto it = waitingDocuments().begin(); it != waitingDocuments().end();) {
        if (it.value() == doc) {
            const QString dir = it.key();
            it = waitingDocuments().erase(it);
            if (!waitingDocuments().contains(dir) && sharedWatcher()) {
                sharedWatcher()->removePath(dir);
            }
        } else {
            ++it;
        }
    }
}
}

void Document::Private::watch()
{
    if (path.isEmpty()) {
        return;
    }
    if (!sharedWatcher()) {
        auto *w = new QFileSystemWatcher(QCoreApplication::instance());
        QObject::connect(w, &QFileSystemWatcher::fileChanged, w, [w](const QString &changed) {
            // An atomic rename (ours or another editor's) drops the watch.
            if (QFileInfo::exists(changed) && !w->files().contains(changed)) {
                w->addPath(changed);
            }
            const auto docs = watchedDocuments().values(changed);
            for (Document *doc : docs) {
                doc->d->diskTimer.start();
            }
        });
        QObject::connect(w, &QFileSystemWatcher::directoryChanged, w, [](const QString &dir) {
            const auto docs = waitingDocuments().values(dir);
            for (Document *doc : docs) {
                doc->d->diskTimer.start();
            }
        });
        sharedWatcher() = w;
    }
    QFileSystemWatcher *watcher = sharedWatcher();
    if (!watchedDocuments().contains(path, q)) {
        watchedDocuments().insert(path, q);
    }
    if (QFileInfo::exists(path)) {
        stopWaiting(q);
        if (!watcher->files().contains(path)) {
            watcher->addPath(path);
        }
        return;
    }
    const QString dir = QFileInfo(path).absolutePath();
    if (!waitingDocuments().contains(dir, q)) {
        stopWaiting(q); // another folder's, before a Save As
        waitingDocuments().insert(dir, q);
    }
    if (QFileInfo(dir).isDir() && !watcher->directories().contains(dir)) {
        watcher->addPath(dir);
    }
}

void Document::Private::unwatch()
{
    stopWaiting(q);
    for (auto it = watchedDocuments().begin(); it != watchedDocuments().end();) {
        if (it.value() == q) {
            const QString p = it.key();
            it = watchedDocuments().erase(it);
            if (!watchedDocuments().contains(p) && sharedWatcher()) {
                sharedWatcher()->removePath(p);
            }
        } else {
            ++it;
        }
    }
}

void Document::Private::checkOnDisk()
{
    if (path.isEmpty()) {
        return;
    }
    if (saving || loading) {
        recheck = true;
        return;
    }
    if (!loaded) {
        // Nothing of the file's is shown. One that couldn't be read is read
        // again when it changes (chmod, a remounted drive); one too large
        // waits for the user.
        if (banners.count(ReadFailed) && !isModified()) {
            startLoad(Initial);
        }
        return;
    }
    watch(); // re-add after an atomic rename
    NpStamp now = {};
    const int err = np_file_stamp(path.toUtf8().constData(), &now);
    if (err == ENOENT || err == ENOTDIR) {
        banners.erase(ReadFailed); // gone now, which Deleted says
        if (!banners.count(Deleted)) {
            cleanBeforeDelete = !isModified();
            deleteRevision = sessionKey();
            setBanner(Deleted);
            setModified(true); // the session must keep the text
        }
        return;
    }
    if (err) {
        return;
    }
    if (banners.count(Deleted)) {
        clearBanner(Deleted);
        // Gone only for a moment (a save by another program, a checkout):
        // untouched since, it is clean again and may reload silently.
        if (cleanBeforeDelete && sessionKey() == deleteRevision) {
            setModified(false);
        }
        cleanBeforeDelete = false;
        if (!hasStamp || !sameStamp(now, stamp)) {
            // Back, but not as we knew it: treated as changed below.
            hasStamp = true;
            stamp.mtimeNs = now.mtimeNs - 1;
        }
    }
    if (hasStamp && sameStamp(now, stamp)) {
        return;
    }
    if (!isModified()) {
        startLoad(SilentReload);
    } else if (!keepMine) {
        setBanner(ChangedOnDisk);
    }
}

// ------------------------------------------------------------------ Load

void Document::Private::startLoad(LoadMode mode, int forcedEncoding)
{
    const int gen = ++loadGeneration;
    watch();
    if (mode == Initial) {
        loaded = false;
    }
    if (mode != SilentReload && !loading) {
        loading = true;
        Q_EMIT q->loadingChanged();
    }
    auto *watcher = new QFutureWatcher<LoadResult>(q);
    QObject::connect(watcher, &QFutureWatcherBase::finished, q, [this, watcher, gen, mode] {
        const LoadResult result = watcher->result();
        watcher->deleteLater();
        if (gen == loadGeneration) {
            finishLoad(result, mode);
            if (recheck && !saving && !loading) {
                recheck = false;
                checkOnDisk();
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([p = path.toUtf8(), forcedEncoding] { return readFile(p, forcedEncoding); }));
}

void Document::Private::finishLoad(const LoadResult &r, LoadMode mode)
{
    if (loading) {
        loading = false;
        Q_EMIT q->loadingChanged();
    }
    if (mode == SilentReload && isModified()) {
        // Typed while the file was being read: it is a conflict now.
        if (!keepMine) {
            setBanner(ChangedOnDisk);
        }
        return;
    }
    if (r.error) {
        if (r.error == ENOENT || r.error == ENOTDIR) {
            banners.erase(ReadFailed); // gone now, which Deleted says
            if (mode == Initial) {
                loaded = true; // nothing read to protect: Save recreates it
            }
            setBanner(Deleted);
            setModified(true);
        } else {
            setBanner(ReadFailed, readErrorText(r.error));
        }
        return;
    }
    stamp = r.stamp;
    hasStamp = true;
    keepMine = false;
    const bool unrecovered = banners.count(Unrecovered);
    banners.clear();
    if (unrecovered) {
        banners[Unrecovered]; // the file is back, the unsaved changes aren't
    }
    const bool wasReadOnly = readOnly;
    if (r.tooLarge) {
        loaded = false;
        readOnly = true;
        banners[TooLarge] = QObject::tr("This file is too large to open (over %1 MB).").arg(Limits::fileBytes >> 20);
        putText(QString(), false);
        Q_EMIT q->bannerChanged();
        if (!wasReadOnly) {
            Q_EMIT q->readOnlyChanged();
        }
        return;
    }
    loaded = true;
    if (encoding != Encoding(r.encoding)) {
        encoding = Encoding(r.encoding);
        Q_EMIT q->encodingChanged();
    }
    if (lineEnding != LineEnding(r.lineEnding)) {
        lineEnding = LineEnding(r.lineEnding);
        Q_EMIT q->lineEndingChanged();
    }
    readOnly = r.binary || r.longLines;
    if (r.binary) {
        banners[Binary];
    }
    if (r.longLines) {
        banners[LongLines];
    }
    if (r.lossy) {
        banners[Lossy];
    }
    if (r.mixed) {
        banners[MixedLineEndings];
    }
    applyMarkdown(qint64(r.stamp.size));
    if (mode == Initial) {
        formatted = markdown && restoredFormatted.value_or(settings().openMarkdownFormatted());
        restoredFormatted.reset();
        Q_EMIT q->formattedChanged();
    }
    checkWritable();
    putText(r.text, mode != Initial);
    scheduleCounts();
    if (readOnly != wasReadOnly) {
        Q_EMIT q->readOnlyChanged();
    }
    Q_EMIT q->bannerChanged();
}

// ------------------------------------------------------------------ Save

void Document::Private::startSave()
{
    saving = true;
    SaveSnapshot snapshot{q->text(), revision(), editGeneration, path};
    const Encoding enc = encoding;
    const LineEnding le = lineEnding;
    auto *watcher = new QFutureWatcher<SaveResult>(q);
    QObject::connect(watcher, &QFutureWatcherBase::finished, q, [this, watcher, snapshot] {
        const SaveResult result = watcher->result();
        watcher->deleteLater();
        finishSave(result, snapshot);
    });
    watcher->setFuture(QtConcurrent::run([snapshot, enc, le] {
        SaveResult r;
        size_t bad = 0;
        r.rc = np_file_save(snapshot.path.toUtf8().constData(), reinterpret_cast<const uint16_t *>(snapshot.text.utf16()), size_t(snapshot.text.size()),
                            uint8_t(enc), uint8_t(le), &r.stamp, &bad);
        return r;
    }));
}

void Document::Private::finishSave(const SaveResult &r, const SaveSnapshot &snapshot)
{
    saving = false;
    if (r.rc == 0) {
        stamp = r.stamp;
        hasStamp = true;
        keepMine = false;
        loaded = true;
        for (const Banner b : {ChangedOnDisk, SaveFailed, ReadFailed, Unrecovered, Unencodable, Deleted, Lossy, MixedLineEndings}) {
            banners.erase(b);
        }
        Q_EMIT q->bannerChanged();
        checkWritable();
        watch();
        if (revision() == snapshot.revision && editGeneration == snapshot.editGeneration) {
            setModified(false);
        }
        Q_EMIT q->saved();
        if (App::instance()) {
            App::instance()->addRecentFile(snapshot.path);
        }
    } else if (r.rc == -1) {
        const QString message = QObject::tr("Some characters can't be saved as %1.").arg(q->encodingName());
        setBanner(Unencodable, message);
        Q_EMIT q->saveFailed(message);
    } else {
        const QString message = errnoText(r.rc);
        setBanner(SaveFailed, message);
        Q_EMIT q->saveFailed(message);
    }
    const bool again = resave && r.rc == 0;
    resave = false;
    if (again) {
        q->save();
    } else if (recheck) {
        recheck = false;
        checkOnDisk();
    }
}

// --------------------------------------------------------------- Restore

void Document::Private::restore(const TabState &t, const std::optional<QString> &text)
{
    path = t.path;
    untitledNumber = path.isEmpty() ? untitledNumber : 0;
    encoding = Encoding(t.encoding);
    lineEnding = LineEnding(t.lineEnding);
    cursor = t.cursor;
    anchor = t.anchor;
    scrollY = t.scrollY;
    if (!path.isEmpty() && !text) {
        restoredFormatted = t.formatted;
        startLoad(Initial);
    } else {
        pending = text.value_or(QString());
        hasPending = true;
        modified = t.modified;
        if (!path.isEmpty()) {
            hasStamp = t.hasStamp;
            stamp = t.stamp;
            applyMarkdown(t.hasStamp ? qint64(t.stamp.size) : qint64(pending.size()));
            checkWritable();
            watch();
            NpStamp now = {};
            const int err = np_file_stamp(path.toUtf8().constData(), &now);
            if (err == ENOENT || err == ENOTDIR) {
                setBanner(Deleted);
                modified = true;
            } else if (err == 0 && t.hasStamp && !sameStamp(now, t.stamp)) {
                setBanner(ChangedOnDisk);
            }
        } else {
            applyMarkdown(0);
        }
        formatted = markdown && t.formatted;
        scheduleCounts();
    }
    Q_EMIT q->pathChanged();
    Q_EMIT q->titleChanged();
    Q_EMIT q->encodingChanged();
    Q_EMIT q->lineEndingChanged();
    Q_EMIT q->formattedChanged();
    Q_EMIT q->modifiedChanged();
    Q_EMIT q->bannerChanged();
}

// --------------------------------------------------------------- Document

Document::Document(DocumentList *list)
    : QObject(list)
    , d(std::make_unique<Private>(this, list))
{
}

Document::~Document()
{
    d->unwatch();
}

namespace
{
// A file name as shown: invisible format characters dropped (with U+202E,
// "evil\u202Etxt.exe" reads "evilexe.txt") and line breaks and other control
// characters as U+FFFD.
QString shown(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        const QChar::Category cat = c.category();
        // Invisible format characters (bidi controls, zero-width spaces, BOM,
        // soft hyphen) go; joiners stay for emoji and scripts that need them.
        if (cat == QChar::Other_Format && u != 0x200C && u != 0x200D) {
            continue;
        }
        const bool breaks = cat == QChar::Other_Control || cat == QChar::Separator_Line || cat == QChar::Separator_Paragraph;
        out.append(breaks ? QChar(QChar::ReplacementCharacter) : c);
    }
    return out.isEmpty() && !name.isEmpty() ? QString(QChar::ReplacementCharacter) : out;
}
}

QString Document::title() const
{
    if (!d->path.isEmpty()) {
        return shown(QFileInfo(d->path).fileName());
    }
    return d->untitledNumber > 1 ? tr("Untitled %1").arg(d->untitledNumber) : tr("Untitled");
}

QString Document::path() const
{
    return d->path;
}

QUrl Document::folder() const
{
    if (d->path.isEmpty()) {
        return QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    }
    return QUrl::fromLocalFile(QFileInfo(d->path).absolutePath());
}

QString Document::toolTip() const
{
    // The desktop style's ToolTip takes text with a tag in it as rich text
    // (a folder named <img src=...> would load an image): a word joiner
    // after each '<' keeps it plain and looks the same.
    return d->path.isEmpty() ? title() : shown(d->path).replace(u'<', QStringLiteral("<\u2060"));
}

bool Document::isModified() const
{
    return d->isModified();
}

bool Document::isLoading() const
{
    return d->loading;
}

bool Document::isMarkdown() const
{
    return d->markdown;
}

bool Document::isProse() const
{
    return d->prose;
}

bool Document::isFormatted() const
{
    return d->formatted;
}

void Document::setFormatted(bool on)
{
    on = on && d->markdown;
    if (on != d->formatted) {
        d->formatted = on;
        Q_EMIT formattedChanged();
    }
}

bool Document::isReadOnly() const
{
    return d->readOnly;
}

Document::Encoding Document::encoding() const
{
    return d->encoding;
}

void Document::setEncoding(Encoding encoding)
{
    if (encoding == d->encoding) {
        return;
    }
    d->encoding = encoding;
    Q_EMIT encodingChanged();
    d->clearBanner(Unencodable);
    d->setModified(true);
}

QString Document::encodingName() const
{
    switch (d->encoding) {
    case Utf8Bom:
        return QStringLiteral("UTF-8 with BOM");
    case Utf16Le:
        return QStringLiteral("UTF-16 LE");
    case Utf16Be:
        return QStringLiteral("UTF-16 BE");
    case Windows1252:
        return QStringLiteral("Windows-1252");
    default:
        return QStringLiteral("UTF-8");
    }
}

Document::LineEnding Document::lineEnding() const
{
    return d->lineEnding;
}

void Document::setLineEnding(LineEnding ending)
{
    if (ending == d->lineEnding) {
        return;
    }
    d->lineEnding = ending;
    d->clearBanner(MixedLineEndings);
    Q_EMIT lineEndingChanged();
    d->setModified(true);
}

QString Document::lineEndingName() const
{
    switch (d->lineEnding) {
    case CrLf:
        return tr("Windows (CRLF)");
    case Cr:
        return tr("Macintosh (CR)");
    default:
        return tr("Unix (LF)");
    }
}

Document::Banner Document::banner() const
{
    return d->banners.empty() ? NoBanner : d->banners.begin()->first;
}

QString Document::bannerText() const
{
    return d->banners.empty() ? QString() : d->banners.begin()->second;
}

QQuickItem *Document::textEdit() const
{
    return d->textEdit;
}

void Document::setTextEdit(QQuickItem *edit)
{
    if (edit == d->textEdit) {
        return;
    }
    if (d->qdoc) {
        // Keep what is in the old TextEdit.
        d->cursor = cursorPosition();
        d->anchor = selectionAnchor();
        d->scrollY = scrollY();
        d->modified = d->qdoc->isModified();
        d->pending = text();
        d->hasPending = true;
        disconnect(d->qdoc, nullptr, this, nullptr);
        d->qdoc = nullptr;
    }
    d->textEdit = edit;
    ++d->editGeneration;
    ++d->contentVersion;
    if (edit) {
        // The text is set as plain text, whatever it looks like (TextEdit.PlainText is Qt::PlainText, 0).
        edit->setProperty("textFormat", int(Qt::PlainText));
        auto *quickDocument = qobject_cast<QQuickTextDocument *>(edit->property("textDocument").value<QObject *>());
        d->qdoc = quickDocument ? quickDocument->textDocument() : nullptr;
        if (d->qdoc) {
            d->connectTextDocument();
            const bool wasModified = d->modified;
            if (d->hasPending) {
                d->settingText = true;
                edit->setProperty("text", d->pending);
                d->settingText = false;
                d->pending.clear();
                d->hasPending = false;
                d->applyView();
            }
            d->qdoc->setModified(wasModified);
            d->scheduleCounts();
        }
    }
    Q_EMIT textEditChanged();
}

int Document::cursorPosition() const
{
    return d->textEdit ? d->textEdit->property("cursorPosition").toInt() : d->cursor;
}

void Document::setCursorPosition(int position)
{
    d->cursor = position;
    Q_EMIT viewStateChanged();
}

int Document::selectionAnchor() const
{
    if (!d->textEdit) {
        return d->anchor;
    }
    const int start = d->textEdit->property("selectionStart").toInt();
    const int end = d->textEdit->property("selectionEnd").toInt();
    const int caret = d->textEdit->property("cursorPosition").toInt();
    return caret == start ? end : start;
}

void Document::setSelectionAnchor(int position)
{
    d->anchor = position;
    Q_EMIT viewStateChanged();
}

qreal Document::scrollY() const
{
    if (QQuickItem *flick = d->flickable()) {
        return flick->property("contentY").toReal();
    }
    return d->scrollY;
}

void Document::setScrollY(qreal y)
{
    d->scrollY = y;
    Q_EMIT viewStateChanged();
}

int Document::characterCount() const
{
    return d->counts.characters;
}

int Document::wordCount() const
{
    return d->counts.words;
}

int Document::lineCount() const
{
    return d->counts.lines;
}

QString Document::text() const
{
    if (!d->qdoc) {
        return d->pending;
    }
    QString out;
    out.reserve(d->qdoc->characterCount());
    bool first = true;
    for (QTextBlock block = d->qdoc->begin(); block.isValid(); block = block.next()) {
        if (!first) {
            out += QLatin1Char('\n');
        }
        first = false;
        out += block.text();
    }
    return out;
}

void Document::save()
{
    if (d->path.isEmpty()) {
        Q_EMIT saveAsRequested();
        return;
    }
    if (d->loading || !d->loaded) {
        // Nothing of the file's is here to write.
        Q_EMIT saveFailed(tr("The file is still being read."));
        return;
    }
    if (d->saving) {
        d->resave = true;
        return;
    }
    NpStamp now = {};
    const int err = np_file_stamp(d->path.toUtf8().constData(), &now);
    if (err == 0 && d->hasStamp && !sameStamp(now, d->stamp) && !d->keepMine) {
        d->setBanner(ChangedOnDisk);
        Q_EMIT saveFailed(tr("The file changed on disk."));
        return;
    }
    d->startSave();
}

void Document::saveAs(const QUrl &url)
{
    // A remote URL's path (sftp://host/etc/x) isn't the local /etc/x.
    const QString newPath = url.isLocalFile() ? url.toLocalFile() : QString();
    if (newPath.isEmpty() || d->saving) {
        Q_EMIT saveFailed(newPath.isEmpty() ? tr("That isn't a file on this computer.") : tr("A save is already running."));
        return;
    }
    QList<Document *> others;
    if (App *app = App::instance()) {
        for (DocumentList *list : app->windows()) {
            others += list->documents();
        }
    } else if (d->list) {
        others = d->list->documents();
    }
    {
        // Two tabs (in any window) on one file would overwrite each other's saves.
        const QString canonical = QFileInfo(newPath).canonicalFilePath();
        const QString absolute = QFileInfo(newPath).absoluteFilePath();
        for (Document *other : std::as_const(others)) {
            if (other == this || other->d->path.isEmpty()) {
                continue;
            }
            if (other->d->path == absolute || (!canonical.isEmpty() && QFileInfo(other->d->path).canonicalFilePath() == canonical)) {
                Q_EMIT saveFailed(tr("%1 is open in another tab. Close it first, or save there.").arg(other->title()));
                return;
            }
        }
    }
    if (d->loading || !d->loaded) {
        // Writing now would put an empty or partial text under the new name.
        Q_EMIT saveFailed(d->loading ? tr("The file is still being read.") : tr("The file couldn't be read, so there is nothing to save."));
        return;
    }
    d->unwatch();
    ++d->loadGeneration; // a silent reload of the old file must not land here
    d->path = QFileInfo(newPath).absoluteFilePath();
    d->untitledNumber = 0;
    d->hasStamp = false;
    d->keepMine = false;
    d->banners.erase(ChangedOnDisk);
    d->banners.erase(Deleted);
    d->banners.erase(ReadOnlyFile);
    d->banners.erase(TooLarge);
    d->readOnly = false;
    d->applyMarkdown(qint64(text().toUtf8().size()));
    d->watch();
    Q_EMIT pathChanged();
    Q_EMIT titleChanged();
    Q_EMIT readOnlyChanged();
    Q_EMIT bannerChanged();
    d->startSave();
}

void Document::reload()
{
    if (d->path.isEmpty() || d->loading || d->saving) {
        return;
    }
    d->keepMine = false;
    d->startLoad(Private::Reload);
}

void Document::reopenWithEncoding(int encoding)
{
    if (d->path.isEmpty() || d->loading || d->saving || d->isModified() || encoding < NP_UTF8 || encoding > NP_WINDOWS_1252) {
        return;
    }
    d->startLoad(Private::AsEncoding, encoding);
}

void Document::keepMine()
{
    // The disk version seen now is the one overruled: a later change shows
    // the banner again, and Save checks against this one.
    NpStamp now = {};
    if (np_file_stamp(d->path.toUtf8().constData(), &now) == 0) {
        d->stamp = now;
        d->hasStamp = true;
    }
    d->keepMine = false;
    d->clearBanner(ChangedOnDisk);
    d->setModified(true);
}

void Document::dismissBanner()
{
    if (!d->banners.empty()) {
        d->banners.erase(d->banners.begin());
        Q_EMIT bannerChanged();
    }
}

QPoint Document::lineColumn(int position) const
{
    if (!d->qdoc) {
        const int p = qBound(0, position, int(d->pending.size()));
        const int line = int(d->pending.left(p).count(QLatin1Char('\n'))) + 1;
        const int start = p == 0 ? 0 : int(d->pending.lastIndexOf(QLatin1Char('\n'), p - 1)) + 1;
        return QPoint(line, p - start + 1);
    }
    const QTextBlock block = d->qdoc->findBlock(qBound(0, position, qMax(0, d->qdoc->characterCount() - 1)));
    const int inBlock = qBound(0, position - block.position(), int(block.length()) - 1);
    int column = 1;
    const QString prefix = block.text().left(inBlock);
    for (const QChar ch : prefix) {
        if (!ch.isLowSurrogate()) {
            ++column;
        }
    }
    return QPoint(block.blockNumber() + 1, column);
}

int Document::positionOfLine(int line) const
{
    if (!d->qdoc) {
        int pos = 0;
        for (int i = 1; i < line; ++i) {
            const int next = int(d->pending.indexOf(QLatin1Char('\n'), pos));
            if (next < 0) {
                break;
            }
            pos = next + 1;
        }
        return pos;
    }
    const QTextBlock block = d->qdoc->findBlockByNumber(qBound(1, line, d->qdoc->blockCount()) - 1);
    return block.position();
}

QVariantMap Document::find(const QString &text, int flags, int from, bool backward) const
{
    if (text.isEmpty()) {
        return noMatch();
    }
    const QString t = this->text();
    if (t.isEmpty()) {
        return noMatch();
    }
    const Matcher m = makeMatcher(text, flags);
    if (!m.error.isEmpty()) {
        return error(m.error);
    }
    std::vector<Span> spans;
    collect(t, m, 0, findCap, spans);
    if (spans.empty()) {
        return noMatch();
    }
    const bool truncated = int(spans.size()) >= findCap;
    from = qBound(0, from, int(t.size()));
    int index = -1; // 0-based into spans
    Span target;
    if (!backward) {
        auto it = std::lower_bound(spans.begin(), spans.end(), from, [](const Span &s, int pos) { return s.first < pos; });
        if (it != spans.end()) {
            index = int(it - spans.begin());
            target = *it;
        } else if (truncated) {
            // Past the matches counted: look on from here.
            std::vector<Span> more;
            collect(t, m, from, 1, more);
            if (!more.empty()) {
                index = findCap - 1;
                target = more.front();
            }
        }
        if (index < 0) {
            index = 0;
            target = spans.front();
        }
    } else {
        auto it = std::lower_bound(spans.begin(), spans.end(), from, [](const Span &s, int pos) { return s.first < pos; });
        if (it != spans.begin()) {
            index = int(it - spans.begin()) - 1;
        } else {
            index = int(spans.size()) - 1; // wrap
        }
        target = spans[size_t(index)];
    }
    return {{QStringLiteral("start"), target.first},
            {QStringLiteral("end"), target.second},
            {QStringLiteral("index"), index + 1},
            {QStringLiteral("count"), int(spans.size())}};
}

void Document::insertText(const QString &text)
{
    if (!d->qdoc || !d->textEdit || d->readOnly || d->loading) {
        return;
    }
    QTextCursor c(d->qdoc);
    c.setPosition(d->textEdit->property("selectionStart").toInt());
    c.setPosition(d->textEdit->property("selectionEnd").toInt(), QTextCursor::KeepAnchor);
    c.beginEditBlock();
    c.insertText(text);
    c.endEditBlock();
    d->textEdit->setProperty("cursorPosition", c.position());
}

int Document::replaceAll(const QString &text, const QString &replacement, int flags)
{
    if (text.isEmpty() || !d->qdoc || d->readOnly) {
        return 0;
    }
    const QString t = this->text();
    const Matcher m = makeMatcher(text, flags);
    if (!m.error.isEmpty()) {
        return 0;
    }
    std::vector<Span> spans;
    collect(t, m, 0, INT_MAX, spans);
    if (spans.empty()) {
        return 0;
    }
    QTextCursor cursor(d->qdoc);
    cursor.beginEditBlock();
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        cursor.setPosition(it->first);
        cursor.setPosition(it->second, QTextCursor::KeepAnchor);
        cursor.insertText(replacementFor(m, t, *it, replacement));
    }
    cursor.endEditBlock();
    return int(spans.size());
}

QVariantMap Document::replaceOne(const QString &text, const QString &replacement, int flags, int start, int end)
{
    if (text.isEmpty() || !d->qdoc || d->readOnly) {
        return find(text, flags, start, false);
    }
    const QString t = this->text();
    const Matcher m = makeMatcher(text, flags);
    if (!m.error.isEmpty()) {
        return error(m.error);
    }
    if (start < 0 || end <= start || end > t.size()) {
        return find(text, flags, qMax(0, start), false);
    }
    // Still the same match?
    bool matches = false;
    if (m.useRegex) {
        const QRegularExpressionMatch match = m.re.match(t, start, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        matches = match.hasMatch() && match.capturedStart() == start && match.capturedEnd() == end;
    } else {
        matches = t.mid(start, end - start).compare(m.plain, m.cs) == 0;
    }
    if (!matches) {
        return find(text, flags, start, false);
    }
    const QString with = replacementFor(m, t, {start, end}, replacement);
    QTextCursor cursor(d->qdoc);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.insertText(with);
    return find(text, flags, start + int(with.size()), false);
}

QString Document::suggestedFileName() const
{
    if (!d->path.isEmpty()) {
        return QFileInfo(d->path).fileName();
    }
    // Formatting was used if the text has Markdown block or inline syntax.
    static const QRegularExpression syntax(QStringLiteral(R"((^(#{1,6} |[-*+] |\d+[.)] |> |```|- \[[ xX]\] ))|\*\*\S|\[[^\]\n]+\]\([^)\n]+\))"),
                                           QRegularExpression::MultilineOption);
    const bool markdownUsed = d->markdown && d->formatted && syntax.match(text()).hasMatch();
    return title() + (markdownUsed ? QStringLiteral(".md") : QStringLiteral(".txt"));
}
