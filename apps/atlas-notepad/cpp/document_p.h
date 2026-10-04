// Document's private data, shared by Document, DocumentList, Session and App
// (the friends in app.h).
#pragma once

#include "app.h"
#include "session.h"

#include <QFutureWatcher>
#include <QElapsedTimer>
#include <QPointer>
#include <QUrl>
#include <QTimer>

#include <map>
#include <optional>

class QQuickTextDocument;
class QWindow;
class KJob;

struct LoadResult {
    int error = 0; // errno
    QString errorText; // KIO's words, when it has some
    bool isDir = false; // a remote folder
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
    QString errorText; // KIO's words
    bool noStamp = false; // a remote save whose new mtime couldn't be read
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
    // Local: the absolute path. Remote (url set): url without its password.
    QString path;
    QUrl url; // empty for a local file or an untitled tab; else KIO handles it
    bool remoteWritable = true; // what the last stat said (the scheme may still refuse)
    bool userOpened = false; // opened by the user (not restored): the first read is a recent file
    bool announceOpen = false; // a failed first read closes the tab and tells openFailed
    QPointer<KJob> remoteJob; // the load or save in flight
    QPointer<KJob> checkJob; // the stat of the change check
    QElapsedTimer lastRemoteCheck;
    int untitledNumber = 0;
    NpStamp stamp = {};
    bool hasStamp = false;
    Encoding encoding = Utf8;
    LineEnding lineEnding = Lf;
    bool markdown = true;
    bool prose = true;
    bool formatted = false;
    bool readOnly = false;
    bool loading = false;
    bool modified = false; // while there is no QTextDocument
    bool loaded = true; // false: the text isn't the file's (never saved over it)
    bool keepMine = false;
    bool saving = false;
    LoadMode lastMode = Initial; // of the load that last started, for a restart after a rename
    int lastForced = -1;
    int percent = -1; // of a remote read, -1 when unknown
    bool resave = false; // save() asked for during a save
    bool recheck = false; // the file changed during a save or load: look again after
    bool safeMode = false; // no Markdown or spell check: a launch died restoring it
    bool safeFormatted = false; // the view stored for it then, written back to the session
    bool cleanBeforeDelete = false; // unmodified when the Deleted banner came
    quint64 deleteRevision = 0; // sessionKey() then
    bool settingText = false;
    // A big text goes into the TextEdit in pieces (fillEdit), because setting
    // it at once lays out all of it on the main thread. While that runs the
    // document counts as loading and fillText is the whole text.
    bool filling = false;
    bool appending = false; // inside a piece: not a change of the text
    bool shownLoading = false; // what loadingChanged last told
    bool fillModified = false; // what isModified() is, until the fill ends
    QString fillText;
    qsizetype fillPos = 0;
    qsizetype fillPiece = 0;
    quint64 fillGeneration = 0;
    // The edit's caret, anchor and scroll once the first piece is in (the
    // view may have clamped the scroll to it): a change from them by the end
    // is the user's.
    int fillBaseCursor = 0;
    int fillBaseAnchor = 0;
    qreal fillBaseScroll = 0;
    QTimer fillTimer;
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
    bool isRemote() const { return !url.isEmpty(); }
    QWindow *window() const; // the one showing this document, or null
    void setPercent(int value);
    void setSaving(bool on);
    // Makes this a remote document (registers it for the focus check).
    void setRemote(const QUrl &remote);
    void startRemoteLoad(LoadMode mode, int forcedEncoding, int generation);
    void saveRemote();
    void checkRemote(bool force);
    void applyDiskStat(int err, const NpStamp &now);
    void cancelRemote();
    // The file was renamed or moved by someone else: follow it. False when
    // another tab has the new place.
    bool relocate(const QUrl &to);
    qint64 currentBytes() const;

    void startLoad(LoadMode mode, int forcedEncoding = -1);
    void finishLoad(const LoadResult &result, LoadMode mode);
    void putText(const QString &text, bool keepView);
    // Puts text into the TextEdit: all at once when small, else the first
    // piece now and the rest from the event loop. True when it's all in.
    bool fillEdit(const QString &text, bool modifiedAfter);
    void fillStep();
    void cancelFill(); // no signals: the caller calls syncLoading()
    void completeFill();
    void setLoading(bool on);
    void syncLoading();
    void emitKeepingView(void (Document::*changed)());
    int editAnchor() const; // the TextEdit's selection anchor, also mid-fill
    bool isLoading() const { return loading || filling; }
    void applyView();
    QQuickItem *flickable() const;

    struct SaveSnapshot {
        QString text;
        quint64 revision;
        quint64 editGeneration;
        QString path;
        QUrl url;
    };
    void startSave();
    void startSaveRemote(const SaveSnapshot &snapshot);
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
