// What the File menu offers for a document's file, as Dolphin does: show it
// in the file manager, open it with another app, its properties, its location.
#include "document_p.h"
#include "remote.h"

#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenFileManagerWindowJob>
#include <KJobWindows>
#include <KPropertiesDialog>

#include <QClipboard>
#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

namespace
{
QUrl shownUrl(const Document *doc)
{
    return doc->url(); // a remote one without its password
}
}

void Document::showInFolder()
{
    if (d->path.isEmpty()) {
        return;
    }
    KIO::highlightInFileManager({shownUrl(this)});
}

void Document::openWith()
{
    if (d->path.isEmpty()) {
        return;
    }
    // No service: the job asks, with KDE's Open With dialog.
    auto *job = new KIO::ApplicationLauncherJob();
    job->setUrls({d->isRemote() ? d->url : shownUrl(this)});
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, nullptr));
    KJobWindows::setWindow(job, d->window() ? d->window() : QGuiApplication::focusWindow());
    job->start();
}

void Document::showProperties()
{
    if (d->path.isEmpty()) {
        return;
    }
    // Not modal, and over Notepad's window.
    auto *dialog = new KPropertiesDialog(shownUrl(this));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->winId();
    QWindow *parent = d->window() ? d->window() : QGuiApplication::focusWindow();
    if (parent && dialog->windowHandle()) {
        dialog->windowHandle()->setTransientParent(parent);
    }
    dialog->show();
}

void Document::copyLocation()
{
    if (d->path.isEmpty()) {
        return;
    }
    // d->path is the local path, or the URL without its password.
    QGuiApplication::clipboard()->setText(d->path);
    if (App *app = App::instance()) {
        Q_EMIT app->notice(tr("Location copied."));
    }
}
