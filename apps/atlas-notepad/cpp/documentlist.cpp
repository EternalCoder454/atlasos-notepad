// One window's tabs: a list model of Documents with the current one, the
// closed-tab stack and the "open" rules (dedupe, failures).
#include "document_p.h"
#include "remote.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QUrl>

#include <cerrno>
#include <cstring>
#include <memory>
#include <unistd.h>

namespace
{
constexpr int closedLimit = 10;

QString openFailure(const QString &path, int error)
{
    const QString name = QFileInfo(path).fileName();
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        return QObject::tr("“%1” doesn't exist.").arg(name);
    case EACCES:
    case EPERM:
        return QObject::tr("You don't have permission to open “%1”.").arg(name);
    default:
        return QObject::tr("Couldn't open “%1”: %2").arg(name, QString::fromLocal8Bit(strerror(error)));
    }
}
}

struct DocumentList::Private {
    DocumentList *q = nullptr;
    QList<Document *> docs;
    int current = -1;
    QStringList closed; // newest last
    bool anyModified = false;
    QPointer<QWindow> window;

    // The smallest "Untitled" number no untitled tab here has.
    int freeUntitled() const
    {
        for (int n = 1;; ++n) {
            bool used = false;
            for (const Document *doc : docs) {
                used = used || (doc->d->path.isEmpty() && doc->d->untitledNumber == n);
            }
            if (!used) {
                return n;
            }
        }
    }

    Document *make()
    {
        auto *doc = new Document(q);
        doc->d->untitledNumber = freeUntitled();
        auto changed = [this, doc] {
            const int row = int(docs.indexOf(doc));
            if (row >= 0) {
                Q_EMIT q->dataChanged(q->index(row), q->index(row), {TitleRole, ModifiedRole, ToolTipRole});
            }
        };
        QObject::connect(doc, &Document::titleChanged, q, changed);
        QObject::connect(doc, &Document::pathChanged, q, changed);
        QObject::connect(doc, &Document::modifiedChanged, q, [this, changed] {
            changed();
            updateAnyModified();
        });
        return doc;
    }

    void insert(Document *doc, int index)
    {
        q->beginInsertRows({}, index, index);
        docs.insert(index, doc);
        q->endInsertRows();
        Q_EMIT q->countChanged();
        updateAnyModified();
    }

    void updateAnyModified()
    {
        bool any = false;
        for (const Document *doc : docs) {
            any = any || doc->isModified();
        }
        if (any != anyModified) {
            anyModified = any;
            Q_EMIT q->anyModifiedChanged();
        }
    }

    Document *find(const QString &path) const
    {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        for (Document *doc : docs) {
            if (doc->d->path.isEmpty()) {
                continue;
            }
            if (doc->d->path == path || (!canonical.isEmpty() && QFileInfo(doc->d->path).canonicalFilePath() == canonical)) {
                return doc;
            }
        }
        return nullptr;
    }
};

DocumentList::DocumentList(QObject *parent)
    : QAbstractListModel(parent)
    , d(std::make_unique<Private>())
{
    d->q = this;
}

DocumentList::~DocumentList() = default;

int DocumentList::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(d->docs.size());
}

QVariant DocumentList::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= d->docs.size()) {
        return {};
    }
    Document *doc = d->docs.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole:
        return doc->title();
    case ModifiedRole:
        return doc->isModified();
    case ToolTipRole:
        return doc->toolTip();
    case DocumentRole:
        return QVariant::fromValue(doc);
    default:
        return {};
    }
}

QHash<int, QByteArray> DocumentList::roleNames() const
{
    return {{TitleRole, "title"}, {ModifiedRole, "modified"}, {ToolTipRole, "toolTip"}, {DocumentRole, "document"}};
}

int DocumentList::currentIndex() const
{
    return d->current;
}

void DocumentList::setCurrentIndex(int index)
{
    if (index < 0 || index >= d->docs.size() || index == d->current) {
        return;
    }
    d->current = index;
    Q_EMIT currentIndexChanged();
}

Document *DocumentList::current() const
{
    return d->current >= 0 && d->current < d->docs.size() ? d->docs.at(d->current) : nullptr;
}

bool DocumentList::canReopenClosed() const
{
    return !d->closed.isEmpty();
}

bool DocumentList::anyModified() const
{
    return d->anyModified;
}

QList<Document *> DocumentList::documents() const
{
    return d->docs;
}

Document *DocumentList::append()
{
    Document *doc = d->make();
    d->insert(doc, int(d->docs.size()));
    if (d->current < 0) {
        d->current = 0;
        Q_EMIT currentIndexChanged();
    }
    return doc;
}

Document *DocumentList::newTab()
{
    Document *doc = d->make();
    const int index = d->current < 0 ? 0 : d->current + 1;
    d->insert(doc, index);
    d->current = -1; // so setCurrentIndex always announces
    setCurrentIndex(index);
    return doc;
}

void DocumentList::open(const QList<QUrl> &urls)
{
    Document *last = nullptr;
    int at = d->current < 0 ? 0 : d->current + 1;
    for (const QUrl &url : urls) {
        if (Remote::useKio(url)) {
            // Read through KIO in the background: the tab is here at once.
            const QString shown = Remote::display(url);
            if (Document *existing = d->find(shown)) {
                last = existing;
                continue;
            }
            const QString why = Remote::unsupported(url, false);
            if (!why.isEmpty() || url.fileName().isEmpty()) {
                Q_EMIT openFailed(why.isEmpty() ? tr("“%1” isn't a file.").arg(shown) : why);
                continue;
            }
            Document *doc = d->make();
            doc->d->setRemote(url);
            doc->d->path = shown;
            doc->d->untitledNumber = 0;
            doc->d->announceOpen = true;
            doc->d->userOpened = true;
            doc->d->applyMarkdown(0);
            d->insert(doc, at++);
            doc->d->startLoad(Document::Private::Initial);
            last = doc;
            continue;
        }
        const QString given = url.isLocalFile() ? url.toLocalFile() : url.toString();
        if (given.isEmpty()) {
            continue;
        }
        const QString path = QFileInfo(given).absoluteFilePath();
        if (Document *existing = d->find(path)) {
            last = existing;
            continue;
        }
        NpStamp stamp = {};
        int err = np_file_stamp(path.toUtf8().constData(), &stamp);
        if (err == 0 && QFileInfo(path).isDir()) {
            Q_EMIT openFailed(tr("“%1” is a folder.").arg(QFileInfo(path).fileName()));
            continue;
        }
        // A FIFO would hang the read, a device (/dev/zero) never end.
        if (err == 0 && !QFileInfo(path).isFile()) {
            Q_EMIT openFailed(tr("“%1” isn't a regular file.").arg(QFileInfo(path).fileName()));
            continue;
        }
        if (err == 0 && ::access(path.toUtf8().constData(), R_OK) != 0) {
            err = EACCES;
        }
        if (err) {
            Q_EMIT openFailed(openFailure(path, err));
            continue;
        }
        if (qint64(stamp.size) > Limits::fileBytes) {
            Q_EMIT openFailed(tr("“%1” is too large to open (over %2 MB).").arg(QFileInfo(path).fileName()).arg(Limits::fileBytes >> 20));
            continue;
        }
        Document *doc = d->make();
        doc->d->path = path;
        doc->d->userOpened = true;
        doc->d->untitledNumber = 0;
        doc->d->applyMarkdown(qint64(stamp.size));
        d->insert(doc, at++);
        doc->d->startLoad(Document::Private::Initial);
        last = doc;
    }
    if (last) {
        const int index = int(d->docs.indexOf(last));
        if (d->current < 0) {
            d->current = index;
            Q_EMIT currentIndexChanged();
        } else {
            setCurrentIndex(index);
        }
    }
}

void DocumentList::requestClose(int index)
{
    if (index < 0 || index >= d->docs.size()) {
        return;
    }
    Document *doc = d->docs.at(index);
    if (doc->isModified()) {
        Q_EMIT closeConfirmationNeeded(doc);
    } else {
        close(index);
    }
}

// Not modal to the whole desktop, and over Notepad's window.
static QFileDialog *newFileDialog(QWindow *parent, const QString &title, const QUrl &folder, const QStringList &nameFilters)
{
    auto *dialog = new QFileDialog(nullptr, title);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setNameFilters(nameFilters);
    if (folder.isValid() && !folder.isEmpty()) {
        dialog->setDirectoryUrl(folder);
    }
    dialog->winId();
    if (parent && dialog->windowHandle()) {
        dialog->windowHandle()->setTransientParent(parent);
    }
    return dialog;
}

void DocumentList::openDialog(const QUrl &folder, const QStringList &nameFilters)
{
    QFileDialog *dialog = newFileDialog(d->window, tr("Open"), folder, nameFilters);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setFileMode(QFileDialog::ExistingFiles);
    connect(dialog, &QFileDialog::urlsSelected, this, [this](const QList<QUrl> &urls) { open(urls); });
    dialog->open();
}

void DocumentList::saveAsDialog(Document *document, const QUrl &folder, const QString &fileName, const QStringList &nameFilters)
{
    if (!document) {
        return;
    }
    QFileDialog *dialog = newFileDialog(d->window, tr("Save As"), folder, nameFilters);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->selectFile(fileName);
    const QPointer<Document> guard(document);
    auto chosen = std::make_shared<bool>(false);
    connect(dialog, &QFileDialog::urlSelected, this, [guard, chosen](const QUrl &url) {
        *chosen = true;
        if (guard && !url.isEmpty()) {
            guard->saveAs(url);
        }
    });
    connect(dialog, &QDialog::finished, this, [this, guard, chosen] {
        if (!*chosen) {
            Q_EMIT saveAsRejected(guard);
        }
    });
    dialog->open();
}

QWindow *DocumentList::window() const
{
    return d->window;
}

void DocumentList::setWindow(QWindow *window)
{
    d->window = window;
}

void DocumentList::renameClosed(const QString &from, const QString &to)
{
    // Local files only, and only where the new place exists and the old one
    // doesn't: a notice can't make a closed tab reopen somewhere else.
    if (!QDir::isAbsolutePath(from) || !QDir::isAbsolutePath(to)) {
        return;
    }
    for (QString &p : d->closed) {
        if (Remote::isStoredUrl(p) || !(p == from || p.startsWith(from + QLatin1Char('/')))) {
            continue;
        }
        const QString moved = to + p.mid(from.size());
        NpStamp atNew = {}, atOld = {};
        const int errOld = np_file_stamp(p.toUtf8().constData(), &atOld);
        if (np_file_stamp(moved.toUtf8().constData(), &atNew) == 0 && (errOld == ENOENT || errOld == ENOTDIR)) {
            p = moved;
        }
    }
}

void DocumentList::close(int index)
{
    if (index < 0 || index >= d->docs.size()) {
        return;
    }
    Document *doc = d->docs.at(index);
    doc->d->cancelRemote();
    if (!doc->d->path.isEmpty() && !doc->d->announceOpen) {
        d->closed.removeAll(doc->d->path);
        d->closed.append(doc->d->path);
        while (d->closed.size() > closedLimit) {
            d->closed.removeFirst();
        }
        Q_EMIT closedChanged();
    }
    doc->d->unwatch();
    ++doc->d->loadGeneration;
    beginRemoveRows({}, index, index);
    d->docs.removeAt(index);
    endRemoveRows();
    Q_EMIT countChanged();
    if (d->docs.isEmpty()) {
        d->current = -1;
        Q_EMIT currentIndexChanged();
    } else if (index < d->current) {
        --d->current;
        Q_EMIT currentIndexChanged();
    } else if (index == d->current) {
        d->current = qMin(index, int(d->docs.size()) - 1);
        Q_EMIT currentIndexChanged();
    }
    d->updateAnyModified();
    doc->deleteLater();
    if (d->docs.isEmpty()) {
        Q_EMIT empty();
    }
}

void DocumentList::closeDocument(Document *document)
{
    close(indexOf(document));
}

void DocumentList::move(int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= d->docs.size() || to >= d->docs.size()) {
        return;
    }
    Document *current = this->current();
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    d->docs.move(from, to);
    endMoveRows();
    const int now = int(d->docs.indexOf(current));
    if (now != d->current) {
        d->current = now;
        Q_EMIT currentIndexChanged();
    }
}

void DocumentList::reopenClosed()
{
    if (d->closed.isEmpty()) {
        return;
    }
    const QString path = d->closed.takeLast();
    Q_EMIT closedChanged();
    open({Remote::isStoredUrl(path) ? QUrl(path) : QUrl::fromLocalFile(path)});
}

int DocumentList::indexOf(Document *document) const
{
    return int(d->docs.indexOf(document));
}

void DocumentList::next()
{
    if (d->docs.size() > 1) {
        setCurrentIndex((d->current + 1) % int(d->docs.size()));
    }
}

void DocumentList::previous()
{
    if (d->docs.size() > 1) {
        setCurrentIndex((d->current + int(d->docs.size()) - 1) % int(d->docs.size()));
    }
}
