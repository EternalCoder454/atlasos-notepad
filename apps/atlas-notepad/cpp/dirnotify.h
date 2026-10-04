// Follows what other programs do to files (Dolphin renaming or moving one,
// another app removing it), as org.kde.KDirNotify announces it on the session
// bus, for local and remote documents alike. With no session bus nothing is
// connected and this does nothing.
#pragma once

#include <QObject>
#include <QStringList>
#include <QUrl>

class App;
class Document;
class QUrl;

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
    App *m_app;
    bool m_connected = false;
};
