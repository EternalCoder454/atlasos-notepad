// What the File menu offers for a document's file, as Dolphin does: show it
// in the file manager, open it with another app, its properties, its location.
#include "document_p.h"
#include "remote.h"

#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenFileManagerWindowJob>
#include <KFileItem>
#include <KOpenWithDialog>
#include <KService>
#include <KIO/StatJob>
#include <KJobWindows>
#include <KPropertiesDialog>

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
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
    // KDE's Open With dialog, made here (not by the job) so it is transient
    // for Notepad's window.
    const QUrl url = shownUrl(this); // no password in what another program is given
    const QPointer<QWindow> parent(d->window() ? d->window() : QGuiApplication::focusWindow());
    auto *dialog = new KOpenWithDialog({url});
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setSaveNewApplications(true);
    dialog->winId();
    if (parent && dialog->windowHandle()) {
        dialog->windowHandle()->setTransientParent(parent);
    }
    connect(dialog, &QDialog::accepted, dialog, [dialog, url, parent] {
        KService::Ptr service = dialog->service();
        if (!service) {
            return;
        }
        auto *job = new KIO::ApplicationLauncherJob(service);
        job->setUrls({url});
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, nullptr));
        KJobWindows::setWindow(job, parent.data());
        job->start();
    });
    dialog->show();
}

void Document::showProperties()
{
    if (d->path.isEmpty()) {
        return;
    }
    // The window and the document are taken before anything that can run an
    // event loop of its own.
    const QPointer<Document> guard(this);
    const QPointer<QWindow> parent(d->window() ? d->window() : QGuiApplication::focusWindow());
    const QUrl url = shownUrl(this);
    auto show = [parent](KPropertiesDialog *dialog) {
        // Not modal, and over Notepad's window.
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->winId();
        if (parent && dialog->windowHandle()) {
            dialog->windowHandle()->setTransientParent(parent);
        }
        dialog->show();
    };
    if (!d->isRemote()) {
        show(new KPropertiesDialog(url));
        return;
    }
    // KPropertiesDialog(QUrl) would stat the server in a nested event loop:
    // stat in the background, then open it for what came back.
    KIO::StatJob *job = KIO::stat(url, KIO::StatJob::SourceSide, KIO::StatDefaultDetails, KIO::HideProgressInfo);
    if (!job) {
        return;
    }
    Remote::setup(job, parent.data());
    connect(job, &KJob::result, this, [guard, job, url, show] {
        if (!guard) {
            return;
        }
        if (job->error()) {
            if (App *app = App::instance()) {
                Q_EMIT app->notice(tr("Couldn't get the properties of %1: %2").arg(guard->title(), job->errorString()));
            }
            return;
        }
        show(new KPropertiesDialog(KFileItem(job->statResult(), url)));
    });
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
