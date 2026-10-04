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
#include <QUuid>

namespace
{
constexpr int formatVersion = 1;

// Atomically, UTF-8 with LF (np_file_save: temp file, fsync, rename).
bool saveUtf8(const QString &path, const QString &text)
{
    NpStamp stamp = {};
    size_t bad = 0;
    return np_file_save(path.toUtf8().constData(), reinterpret_cast<const uint16_t *>(text.utf16()), size_t(text.size()), NP_UTF8, NP_LF, &stamp, &bad) == 0;
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
}

QString Session::directory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/atlas-notepad");
}

QList<WindowState> Session::read() const
{
    QFile file(directory() + QStringLiteral("/session.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject() || doc.object().value(QStringLiteral("version")).toInt() != formatVersion) {
        return {};
    }
    QList<WindowState> windows;
    for (const QJsonValue &wv : doc.object().value(QStringLiteral("windows")).toArray()) {
        const QJsonObject w = wv.toObject();
        WindowState state;
        const QJsonArray g = w.value(QStringLiteral("geometry")).toArray();
        if (g.size() == 4) {
            state.geometry = QRect(g[0].toInt(), g[1].toInt(), g[2].toInt(), g[3].toInt());
        }
        state.maximized = w.value(QStringLiteral("maximized")).toBool();
        state.currentIndex = w.value(QStringLiteral("currentIndex")).toInt();
        for (const QJsonValue &tv : w.value(QStringLiteral("tabs")).toArray()) {
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
            if (t.contains(QStringLiteral("stamp"))) {
                tab.hasStamp = true;
                tab.stamp = stampFromJson(t.value(QStringLiteral("stamp")).toObject());
            }
            tab.textFile = t.value(QStringLiteral("textFile")).toString();
            if (tab.encoding < NP_UTF8 || tab.encoding > NP_WINDOWS_1252 || tab.lineEnding < NP_LF || tab.lineEnding > NP_CR) {
                continue;
            }
            state.tabs.append(tab);
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
    QFile file(textPath(textFile));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return QString::fromUtf8(file.readAll());
}

Document *Session::restoreTab(DocumentList *list, const TabState &tab)
{
    Document *doc = list->append();
    std::optional<QString> text;
    if (!tab.textFile.isEmpty() && (tab.modified || tab.path.isEmpty())) {
        text = readText(tab.textFile);
    }
    doc->d->restore(tab, text);
    if (text) {
        m_texts.insert(doc->d->id, {tab.textFile, doc->d->sessionKey()});
    }
    return doc;
}

void Session::write(const QList<Live> &windows, bool wait)
{
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
            t[QStringLiteral("formatted")] = p->formatted;
            t[QStringLiteral("modified")] = p->isModified();
            t[QStringLiteral("cursor")] = doc->cursorPosition();
            t[QStringLiteral("anchor")] = doc->selectionAnchor();
            t[QStringLiteral("scrollY")] = double(doc->scrollY());
            if (p->hasStamp) {
                t[QStringLiteral("stamp")] = stampToJson(p->stamp);
            }

            const bool needsText = !p->loading && (p->path.isEmpty() || p->isModified());
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
    m_pool.start([dir, json, writes, referenced] {
        if (!QDir().mkpath(dir + QStringLiteral("/texts"))) {
            qWarning("atlas-notepad: can't create %s", qPrintable(dir));
            return;
        }
        for (const TextWrite &w : writes) {
            if (!saveUtf8(textPath(w.file), w.text)) {
                qWarning("atlas-notepad: can't write the session's text %s", qPrintable(w.file));
            }
        }
        if (!saveUtf8(dir + QStringLiteral("/session.json"), json)) {
            qWarning("atlas-notepad: can't write the session");
        }
        QDir texts(dir + QStringLiteral("/texts"));
        for (const QString &name : texts.entryList({QStringLiteral("*.txt")}, QDir::Files)) {
            if (!referenced.contains(QFileInfo(name).completeBaseName())) {
                texts.remove(name);
            }
        }
    });
    if (wait) {
        m_pool.waitForDone();
    }
}

void Session::remove()
{
    m_pool.waitForDone();
    m_texts.clear();
    QDir(directory()).removeRecursively();
}
