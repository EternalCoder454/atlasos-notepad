// Follows what other programs do to files (Dolphin renaming or moving one,
// another app removing it), as org.kde.KDirNotify announces it on the session
// bus, for local and remote documents alike. With no session bus nothing is
// connected and this does nothing. Any program on the bus can send these, so
// a move is followed only when the file is really there (see follow()).
#pragma once

#include <QList>
#include <QObject>
#include <QPair>
#include <QStringList>
#include <QTimer>
#include <QUrl>

class App;
class Document;

class DirNotifyListener : public QObject
{
    Q_OBJECT
public:
    explicit DirNotifyListener(App *app);
    bool isConnected() const { return m_connected; }

public Q_SLOTS:
    void FileRenamed(const QString &src, const QString &dst);
    void FileRenamedWithLocalPath(const QString &src, const QString &dst, const QString &dstPath);
    void FileMoved(const QString &src, const QString &dst);
    void FilesChanged(const QStringList &files);
    void FilesRemoved(const QStringList &files);

private:
    static QUrl locationOf(Document *doc);
    void queueMove(const QString &src, const QString &dst);
    void follow(Document *doc, const QUrl &old, const QUrl &to);
    void finishValidation(Document *doc);
    void apply(Document *doc, const QUrl &old, const QUrl &to);
    void flush();

    App *m_app;
    bool m_connected = false;
    QTimer m_timer; // one pass for a flood of notices
    QStringList m_removed, m_changed;
    QList<QPair<QUrl, QUrl>> m_moves; // src -> dst, in the order they came, capped
};
