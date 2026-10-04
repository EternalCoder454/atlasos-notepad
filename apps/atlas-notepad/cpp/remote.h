// Documents on non-local URLs (sftp, smb, fish, ftp, webdav...) go through
// KIO. Local files never do: they keep the Rust file layer (np_file_*).
#pragma once

#include "notepad_core.h"

#include <QString>
#include <QUrl>

#include <functional>

class KJob;
class QObject;

namespace Remote
{
// Local files use KIO only when a test asks for it (NP_KIO_TEST_HOOK is
// defined for app_test alone, so no release build has the switch).
#ifdef NP_KIO_TEST_HOOK
void setForceKio(bool on);
#endif

// True for any URL KIO should handle: not a local file (or, in tests, any).
bool useKio(const QUrl &url);
// The URL as text, user name kept and password dropped: what a tab, the
// session, recent files and messages show. Never contains a password.
QString display(const QUrl &url);
// A path or URL as stored (session, recent files): a local path or display().
bool isStoredUrl(const QString &stored);
QUrl fromStored(const QString &stored);
// Why a URL can't be opened (or saved to), or empty: unknown scheme, a
// scheme with no file access, or (forWriting) one that can't be written.
QString unsupported(const QUrl &url, bool forWriting);
// "name — host", or the display string when there is no host.
QString nameAndHost(const QUrl &url);

struct StatInfo {
    int error = 0;        // 0, ENOENT, ECANCELED or EIO
    QString errorText;    // KIO's words
    bool isDir = false;
    bool writable = true; // unknown counts as writable
    NpStamp stamp = {};   // mtime in seconds (as ns) and size; dev, ino 0
};

// KIO::stat in the background (no progress UI). done runs on the main
// thread unless ctx is destroyed first. The job is returned so it can be
// killed; null when nothing could be started (done is then called at once).
KJob *stat(const QUrl &url, QObject *ctx, std::function<void(const StatInfo &)> done);

// A UI delegate for KIO's own password and certificate prompts, parented
// to the active window. Errors never use KMessageBox: callers read
// errorString().
void setup(KJob *job);
} // namespace Remote
