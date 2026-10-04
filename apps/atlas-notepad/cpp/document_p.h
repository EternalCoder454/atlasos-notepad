// Document's private data, shared by Document, DocumentList, Session and App
// (the friends in app.h).
#pragma once

#include "app.h"
#include "session.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>

#include <map>
#include <optional>

class QQuickTextDocument;

struct LoadResult {
    int error = 0; // errno
    bool tooLarge = false;
    QString text;
    int encoding = 0;
    int lineEnding = 0;
    bool mixed = false;
    bool binary = false;
    bool lossy = false;
    bool longLines = false;
    NpStamp stamp = {};
};

struct SaveResult {
    int rc = 0; // 0, an errno, or -1 (unencodable)
    NpStamp stamp = {};
};

struct Counts {
    int characters = 0;
    int words = 0;
    int lines = 1;
};

struct Document::Private {
    enum LoadMode {
        Initial,      // first read of a tab
        Reload,       // the user's: changes here are dropped
        SilentReload, // the file changed outside and this tab has no changes
        AsEncoding,   // read as another encoding
    };

    Document *q = nullptr;
    DocumentList *list = nullptr;
    quint64 id = 0;
    QString path;
    int untitledNumber = 0;
    NpStamp stamp = {};
    bool hasStamp = false;
    Encoding encoding = Utf8;
    LineEnding lineEnding = Lf;
    bool markdown = true;
    bool formatted = false;
    bool readOnly = false;
    bool loading = false;
    bool modified = false; // while there is no QTextDocument
    bool loaded = true; // false: the text isn't the file's (never saved over it)
    bool keepMine = false;
    bool saving = false;
    bool resave = false; // save() asked for during a save
    bool recheck = false; // the file changed during a save or load: look again after
    bool cleanBeforeDelete = false; // unmodified when the Deleted banner came
    quint64 deleteRevision = 0; // sessionKey() then
    bool settingText = false;
    std::map<Banner, QString> banners;

    QPointer<QQuickItem> textEdit;
    QPointer<QTextDocument> qdoc;
    QString pending; // text read, not yet in a TextEdit
    bool hasPending = false;
    quint64 editGeneration = 0; // counts the TextEdits this document had
    quint64 contentVersion = 0; // bumped by every change of the text, never taken back
    int cursor = 0;
    int anchor = 0;
    qreal scrollY = 0;
    std::optional<bool> restoredFormatted;

    Counts counts;
    int countGeneration = 0;
    int loadGeneration = 0;
    QTimer countTimer;
    QTimer diskTimer;

    explicit Private(Document *document, DocumentList *list);

    Settings &settings() const;
    bool isModified() const;
    void setModified(bool on);
    void setBanner(Banner banner, const QString &text = {});
    void clearBanner(Banner banner);
    // Markdown-ness and the FormattingOff banner, from the extension, the
    // setting and the size in bytes.
    void applyMarkdown(qint64 bytes);
    void checkWritable();
    qint64 currentBytes() const;

    void startLoad(LoadMode mode, int forcedEncoding = -1);
    void finishLoad(const LoadResult &result, LoadMode mode);
    void putText(const QString &text, bool keepView);
    void applyView();
    QQuickItem *flickable() const;

    struct SaveSnapshot {
        QString text;
        quint64 revision;
        quint64 editGeneration;
        QString path;
    };
    void startSave();
    void finishSave(const SaveResult &result, const SaveSnapshot &snapshot);

    void watch();
    void unwatch();
    void checkOnDisk();

    void scheduleCounts();
    void updateCounts();

    void connectTextDocument();
    // What Session compares to know whether the text changed since it wrote it.
    quint64 sessionKey() const;
    quint64 revision() const;
    void restore(const TabState &tab, const std::optional<QString> &text);
};

bool sameStamp(const NpStamp &a, const NpStamp &b);
