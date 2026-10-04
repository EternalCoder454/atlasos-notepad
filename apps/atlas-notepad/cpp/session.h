// The session: which windows and tabs were open, and the text of every tab
// that isn't just a file on disk (untitled or modified), so a restart, a
// logout or a crash brings them back. Stored under
// $XDG_DATA_HOME/atlas-notepad/: session.json and texts/<uuid>.txt, each
// written atomically (np_file_save). Internal to the app: App owns one.
#pragma once

#include "app.h"

#include <QHash>
#include <QMutex>
#include <QRect>
#include <QSet>
#include <QThreadPool>

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
    Document *restoreTab(DocumentList *list, const TabState &tab);

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
    // Forgets everything on disk (Settings.continueSession is off).
    void remove();

private:
    struct TextState {
        QString file;
        quint64 key;
    };
    void dropFailed();
    QHash<quint64, TextState> m_texts; // by Document id
    bool m_readFailed = false;
    QMutex m_mutex; // guards m_failed (the worker adds to it)
    QSet<QString> m_failed; // text files the worker couldn't write
    QThreadPool m_pool;
};
