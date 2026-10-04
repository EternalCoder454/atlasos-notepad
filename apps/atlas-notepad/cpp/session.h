// The session: which windows and tabs were open, and the text of every tab
// that isn't just a file on disk (untitled or modified), so a restart, a
// logout or a crash brings them back. Stored under
// $XDG_STATE_HOME/atlas-notepad/session/: session.json and texts/<uuid>.txt, each
// written atomically (np_file_save). Internal to the app: App owns one.
#pragma once

#include "app.h"

#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QThreadPool>

#include <functional>
#include <optional>

// One tab as stored in session.json.
struct TabState {
    QString path; // empty: untitled
    int encoding = NP_UTF8;
    int lineEnding = NP_LF;
    bool formatted = false;
    bool modified = false;
    int cursor = 0;
    int anchor = 0;
    double scrollY = 0;
    bool hasStamp = false;
    NpStamp stamp = {};
    QString textFile; // under texts/, empty: none
};

struct WindowState {
    QRect geometry;
    bool maximized = false;
    int currentIndex = 0;
    QList<TabState> tabs;
};

class Session
{
public:
    Session();
    ~Session(); // waits for writes in flight

    static QString directory();

    // Only one Notepad keeps the session: each would delete the other's
    // texts. Two can run with separate D-Bus sessions (two logins, a
    // dbus-run-session), which KDBusService::Unique doesn't see. False when
    // another process holds $XDG_STATE_HOME/atlas-notepad/session.lock;
    // true when it's ours or can't be made (then nothing is guarded).
    bool lock();

    // A launch that dies while restoring would die again on the same
    // session. restoreDeaths(): how many launches in a row died restoring
    // (0 normally). beginRestore(deaths) counts this one in a file, before
    // restoring tabs; endRestore() once they are read and drawn. Launches
    // with nothing to restore don't count.
    int restoreDeaths();
    void beginRestore(int deaths);
    void endRestore();
    // Moves session.json to session.json.bak and keeps every text, so
    // nothing of it is lost; read() then finds no session.
    void setAside();

    // The stored windows (those with no tabs dropped); empty if none or the
    // file is unreadable. A session.json that exists but can't be used
    // (corrupt, unknown version) is copied to session.json.bak, and this run
    // never deletes text files (they may belong to it). Also removes the
    // temp files an interrupted write left in texts/.
    QList<WindowState> read();
    // The text of a tab's text file.
    std::optional<QString> readText(const QString &textFile) const;
    // Makes a tab of `list` from its stored state: modified and untitled tabs
    // take their text from the text file, the others read their file again.
    // `safe`: without Markdown or spell check, after a launch died restoring.
    Document *restoreTab(DocumentList *list, const TabState &tab, bool safe = false);

    struct Live {
        DocumentList *list;
        QRect geometry;
        bool maximized;
    };
    // Writes the windows' state (most recent window first): the tabs'
    // texts that changed since last written, then session.json (only if every
    // text was written; a failed text is written again next time), then
    // deletes text files nobody refers to (only after session.json was
    // written). On a worker; wait: until it's done.
    void write(const QList<Live> &windows, bool wait);
    // Why the last write failed, empty if it worked (or none ran yet).
    QString lastError() const;
    // Called on `context`'s thread after each write finished.
    void onWritten(QObject *context, std::function<void()> callback);
    // Forgets everything on disk (Settings.continueSession is off).
    void remove();

private:
    struct TextState {
        QString file;
        quint64 key;
    };
    void dropFailed();
    void moveOld();
    QHash<quint64, TextState> m_texts; // by Document id
    bool m_readFailed = false;
    int m_lockFd = -1;
    mutable QMutex m_mutex; // guards m_failed and m_lastError (the worker sets them)
    QSet<QString> m_failed; // text files the worker couldn't write
    QString m_lastError;
    QPointer<QObject> m_context;
    std::function<void()> m_written;
    QThreadPool m_pool;
};
