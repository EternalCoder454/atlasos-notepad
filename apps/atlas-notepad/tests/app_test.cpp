// Documents, tabs, the session and settings. XDG_* point into a temp dir
// (main below), App runs without QML (a null engine), and the TextEdits are
// real ones made from QML, as in editor_test.
#include "app.h"
#include "document_p.h"
#include "remote.h"
#include "session.h"

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDirIterator>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTest>

#include <KDirNotify>
#include <KSharedConfig>

#include <memory>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
QTemporaryDir *s_tmp = nullptr;

QByteArray utf16(const QString &s, bool big, bool bom = true)
{
    QByteArray out;
    if (bom) {
        out += big ? "\xFE\xFF" : "\xFF\xFE";
    }
    for (const QChar c : s) {
        const char hi = char(c.unicode() >> 8), lo = char(c.unicode() & 0xff);
        out.append(big ? hi : lo);
        out.append(big ? lo : hi);
    }
    return out;
}
}

class AppTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;
    App *m_app = nullptr;
    std::vector<std::unique_ptr<QQuickItem>> m_edits;
    QString m_dir;
    int m_files = 0;

    QString stateDir() const
    {
        return QString::fromUtf8(qgetenv("XDG_STATE_HOME"));
    }

    QString write(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir + QLatin1Char('/') + name;
        QFile f(path);
        [&] { QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); }();
        f.write(bytes);
        return path;
    }
    static QByteArray read(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
    }
    DocumentList *newList()
    {
        return m_app->windows().isEmpty() ? (m_app->newWindow(), m_app->windows().first()) : m_app->windows().first();
    }
    Document *openFile(DocumentList *list, const QString &path)
    {
        list->open({QUrl::fromLocalFile(path)});
        Document *doc = list->current();
        if (!doc) {
            return nullptr;
        }
        const bool ok = QTest::qWaitFor([&] { return !doc->isLoading(); });
        return ok ? doc : nullptr;
    }
    QString sessionDir() const
    {
        return stateDir() + QStringLiteral("/atlas-notepad/session");
    }
    QJsonObject sessionJson() const
    {
        return QJsonDocument::fromJson(read(sessionDir() + QStringLiteral("/session.json"))).object();
    }
    QQuickItem *attach(Document *doc)
    {
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nTextEdit { textFormat: TextEdit.PlainText }", QUrl());
        auto *edit = qobject_cast<QQuickItem *>(component.create());
        m_edits.emplace_back(edit);
        doc->setTextEdit(edit);
        return edit;
    }
    static void insert(Document *doc, int pos, const QString &s)
    {
        QMetaObject::invokeMethod(doc->textEdit(), "insert", Q_ARG(int, pos), Q_ARG(QString, s));
    }
    void restart()
    {
        delete m_app;
        m_edits.clear();
        m_app = new App(nullptr);
    }
    bool saveAndWait(Document *doc)
    {
        QSignalSpy spy(doc, &Document::saved);
        doc->save();
        return spy.wait(5000);
    }

    // The session folder as a plain file: nothing can be written there.
    void breakSession()
    {
        QDir(sessionDir()).removeRecursively();
        QVERIFY(QDir().mkpath(QFileInfo(sessionDir()).path()));
        QFile blocker(sessionDir());
        QVERIFY(blocker.open(QIODevice::WriteOnly));
    }
    void fixSession()
    {
        QVERIFY(QFile::remove(sessionDir()));
    }

    // A session of one Markdown tab, for the restore guard.
    QString markdownSession()
    {
        const QString path = write(QStringLiteral("notes.md"), "# Notes\n");
        m_app->start({path});
        Document *doc = m_app->windows().first()->current();
        [&] {
            QVERIFY(QTest::qWaitFor([doc] { return !doc->isLoading(); }));
            QVERIFY(doc->isMarkdown());
        }();
        m_app->saveSession();
        restart();
        return path;
    }
    void setRestoring(int count)
    {
        QFile marker(sessionDir() + QStringLiteral("/restoring"));
        QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        marker.write(QByteArray::number(count));
    }

private Q_SLOTS:
    void init()
    {
        QDir(m_dir = s_tmp->path() + QStringLiteral("/work%1").arg(++m_files)).mkpath(QStringLiteral("."));
        m_app = new App(nullptr);
    }
    void cleanup()
    {
        delete m_app;
        m_app = nullptr;
        m_edits.clear();
        QDir(stateDir()).removeRecursively();
        QFile::remove(Settings::filePath());
        Remote::setForceKio(false);
    }

    void roundTrip_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("encoding");
        QTest::addColumn<int>("lineEnding");
        QTest::addColumn<QString>("text");
        const QString t = QStringLiteral("héllo\nwörld\n");
        QTest::newRow("utf8") << t.toUtf8() << int(Document::Utf8) << int(Document::Lf) << t;
        QTest::newRow("utf8 bom") << "\xEF\xBB\xBF" + t.toUtf8() << int(Document::Utf8Bom) << int(Document::Lf) << t;
        QTest::newRow("utf16 le") << utf16(t, false) << int(Document::Utf16Le) << int(Document::Lf) << t;
        QTest::newRow("utf16 be") << utf16(t, true) << int(Document::Utf16Be) << int(Document::Lf) << t;
        QTest::newRow("1252") << QByteArray("caf\xE9 \x93q\x94\n") << int(Document::Windows1252) << int(Document::Lf)
                              << QStringLiteral("café “q”\n");
        QTest::newRow("crlf") << QByteArray("a\r\nb\r\n") << int(Document::Utf8) << int(Document::CrLf) << QStringLiteral("a\nb\n");
    }
    void roundTrip()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(int, encoding);
        QFETCH(int, lineEnding);
        QFETCH(QString, text);
        const QString path = write(QStringLiteral("f.txt"), bytes);
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        QCOMPARE(int(doc->encoding()), encoding);
        QCOMPARE(int(doc->lineEnding()), lineEnding);
        QCOMPARE(doc->text(), text);
        QVERIFY(!doc->isModified());
        QVERIFY(saveAndWait(doc));
        QCOMPARE(read(path), bytes);
        QVERIFY(!doc->isModified());
    }

    void nbspSurvivesSave()
    {
        const QByteArray bytes = "a\xC2\xA0" "b\n";
        const QString path = write(QStringLiteral("n.txt"), bytes);
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(doc->text().contains(QChar(0x00A0)));
        QVERIFY(saveAndWait(doc));
        QCOMPARE(read(path), bytes);
    }

    void tooLargeBanner()
    {
        const QString path = write(QStringLiteral("big.txt"), "small\n");
        DocumentList *list = newList();
        Document *doc = openFile(list, path);
        QVERIFY(doc);
        attach(doc);
        // The file grows past the limit: reload refuses and reads nothing.
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadWrite));
        QVERIFY(f.resize(Limits::fileBytes + 1));
        f.close();
        doc->reload();
        QTRY_COMPARE(doc->banner(), Document::TooLarge);
        QVERIFY(doc->isReadOnly());
        QCOMPARE(doc->text(), QString());
        // Opening a file that large fails with a message and makes no tab.
        QSignalSpy failed(list, &DocumentList::openFailed);
        const int before = list->rowCount();
        QFile g(write(QStringLiteral("big2.txt"), QByteArray()));
        QVERIFY(g.open(QIODevice::ReadWrite));
        QVERIFY(g.resize(Limits::fileBytes + 1));
        g.close();
        list->open({QUrl::fromLocalFile(g.fileName())});
        QCOMPARE(failed.size(), 1);
        list->open({QUrl::fromLocalFile(m_dir + QStringLiteral("/missing.txt"))});
        QCOMPARE(failed.size(), 2);
        QCOMPARE(list->rowCount(), before);
    }

    void specialFilesRefused()
    {
        const QString fifo = m_dir + QStringLiteral("/fifo.txt");
        QCOMPARE(mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        const QString zero = m_dir + QStringLiteral("/zero.txt");
        QVERIFY(QFile::link(QStringLiteral("/dev/zero"), zero));
        DocumentList *list = newList();
        QSignalSpy failed(list, &DocumentList::openFailed);
        const int before = list->rowCount();
        list->open({QUrl::fromLocalFile(fifo), QUrl::fromLocalFile(zero)});
        QCOMPARE(failed.size(), 2);
        QVERIFY(failed.at(0).at(0).toString().contains(QStringLiteral("regular file")));
        QCOMPARE(list->rowCount(), before);
        // A file that turns into one after it was opened: reload refuses too.
        const QString path = write(QStringLiteral("swap.txt"), "text\n");
        Document *doc = openFile(list, path);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::link(QStringLiteral("/dev/zero"), path));
        doc->reload();
        QTRY_COMPARE(doc->banner(), Document::ReadFailed);
        QVERIFY(doc->bannerText().contains(QStringLiteral("regular file")));
    }

    void longLinesReadOnly()
    {
        const QString path = write(QStringLiteral("long.txt"), QByteArray(Limits::lineLength + 1, 'a') + "\nshort\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        QCOMPARE(doc->banner(), Document::LongLines);
        QVERIFY(doc->isReadOnly());
    }

    void binaryReadOnly()
    {
        const QString path = write(QStringLiteral("b.bin"), QByteArray("ab\0cd\n", 6));
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        QCOMPARE(doc->banner(), Document::Binary);
        QVERIFY(doc->isReadOnly());
    }

    void lossyAndMixedBanners()
    {
        Document *doc = openFile(newList(), write(QStringLiteral("m.txt"), "a\r\nb\nc\n"));
        QVERIFY(doc);
        QCOMPARE(doc->banner(), Document::MixedLineEndings);
        attach(doc);
        QVERIFY(saveAndWait(doc));
        QCOMPARE(doc->banner(), Document::NoBanner);
        QCOMPARE(read(m_dir + QStringLiteral("/m.txt")), QByteArray("a\nb\nc\n"));

        // U+2029 splits a line in the editor, so it is warned about the same way.
        doc = openFile(newList(), write(QStringLiteral("p.txt"), "a\xE2\x80\xA9" "b\n"));
        QVERIFY(doc);
        QCOMPARE(doc->banner(), Document::MixedLineEndings);
        attach(doc);
        QVERIFY(saveAndWait(doc));
        QCOMPARE(read(m_dir + QStringLiteral("/p.txt")), QByteArray("a\nb\n"));
    }

    void displayPathShortensHome()
    {
        const QString home = QDir::homePath();
        QCOMPARE(m_app->displayPath(home + QStringLiteral("/a.txt")), QStringLiteral("~/a.txt"));
        QCOMPARE(m_app->displayPath(home + QStringLiteral("x/a.txt")), home + QStringLiteral("x/a.txt"));
        QCOMPARE(m_app->displayPath(QStringLiteral("/etc/hosts")), QStringLiteral("/etc/hosts"));
    }

    void insertTextIsOneUndo()
    {
        Document *doc = openFile(newList(), write(QStringLiteral("i.txt"), "ab cd"));
        QVERIFY(doc);
        QQuickItem *edit = attach(doc);
        QMetaObject::invokeMethod(edit, "select", Q_ARG(int, 2), Q_ARG(int, 5));
        doc->insertText(QStringLiteral("\n12:00\n"));
        QCOMPARE(doc->text(), QStringLiteral("ab\n12:00\n"));
        QCOMPARE(edit->property("cursorPosition").toInt(), 9);
        QMetaObject::invokeMethod(edit, "undo");
        QCOMPARE(doc->text(), QStringLiteral("ab cd"));
    }

    // A text with no UTF-8 form must not stop the session from being written.
    void sessionSurvivesLoneSurrogate()
    {
        Document *doc = newList()->newTab();
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("x") + QChar(0xD800) + QStringLiteral("y"));
        m_app->saveSession();
        const QJsonArray tabs = sessionJson().value(QStringLiteral("windows")).toArray().first().toObject().value(QStringLiteral("tabs")).toArray();
        QVERIFY(!tabs.isEmpty());
        const QString file = tabs.last().toObject().value(QStringLiteral("textFile")).toString();
        QCOMPARE(QString::fromUtf8(read(sessionDir() + QStringLiteral("/texts/") + file + QStringLiteral(".txt"))), QStringLiteral("x�y"));
    }

    void saveWhileTypingStaysModified()
    {
        const QString path = write(QStringLiteral("t.txt"), "abc\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("x"));
        QVERIFY(doc->isModified());
        QSignalSpy saved(doc, &Document::saved);
        doc->save();
        insert(doc, 0, QStringLiteral("y")); // before the save finishes
        QVERIFY(saved.wait(5000));
        QVERIFY(doc->isModified());
        QCOMPARE(read(path), QByteArray("xabc\n"));
        QVERIFY(saveAndWait(doc));
        QVERIFY(!doc->isModified());
        QCOMPARE(read(path), QByteArray("yxabc\n"));
    }

    void unencodable()
    {
        const QString path = write(QStringLiteral("e.txt"), "caf\xE9\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        QCOMPARE(doc->encoding(), Document::Windows1252);
        insert(doc, 0, QString::fromUtf8("\xF0\x9F\x98\x80"));
        doc->save();
        QTRY_COMPARE(doc->banner(), Document::Unencodable);
        QVERIFY(doc->isModified());
        QCOMPARE(read(path), QByteArray("caf\xE9\n"));
        doc->setEncoding(Document::Utf8);
        QVERIFY(saveAndWait(doc));
    }

    void saveFailedMessage()
    {
        Document *doc = newList()->newTab();
        attach(doc);
        insert(doc, 0, QStringLiteral("x"));
        QSignalSpy asked(doc, &Document::saveAsRequested);
        doc->save();
        QCOMPARE(asked.size(), 1);
        QSignalSpy failed(doc, &Document::saveFailed);
        doc->saveAs(QUrl::fromLocalFile(m_dir + QStringLiteral("/nope/x.txt")));
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(doc->banner(), Document::SaveFailed);
        QCOMPARE(doc->bannerText(), QStringLiteral("The folder doesn't exist."));
        QVERIFY(doc->isModified());
    }

    // ---- KIO. The test build sends local files through the KIO path, so the
    // real file worker stands in for sftp: no network.
    Document *openKio(DocumentList *list, const QString &path)
    {
        Remote::setForceKio(true);
        list->open({QUrl::fromLocalFile(path)});
        Document *doc = list->current();
        if (!doc || !doc->isRemote()) {
            return nullptr;
        }
        // A refused open closes the tab: don't touch it once it's gone.
        QPointer<Document> guard = doc;
        const bool done = QTest::qWaitFor([&] { return !guard || !guard->isLoading(); }, 20000);
        return done && guard ? doc : nullptr;
    }

    void kioRoundTrip()
    {
        const QString text = QStringLiteral("h\u00e9llo\r\nworld\r\n");
        const QString path = write(QStringLiteral("k.txt"), utf16(text, false));
        Document *doc = openKio(newList(), path);
        QVERIFY(doc);
        QCOMPARE(doc->text(), QStringLiteral("h\u00e9llo\nworld\n"));
        QCOMPARE(int(doc->encoding()), int(NP_UTF16_LE));
        QCOMPARE(int(doc->lineEnding()), int(NP_CRLF));
        QCOMPARE(doc->title(), QStringLiteral("k.txt"));
        attach(doc);
        insert(doc, 0, QStringLiteral("X"));
        QVERIFY(doc->isModified());
        QVERIFY(saveAndWait(doc));
        QVERIFY(!doc->isModified());
        QCOMPARE(read(path), utf16(QStringLiteral("Xh\u00e9llo\r\nworld\r\n"), false));
        // Saved again with no change, then reopened: the same bytes.
        QTRY_VERIFY(doc->d->hasStamp);
        const QByteArray before = read(path);
        newList()->closeDocument(doc);
        Document *again = openKio(newList(), path);
        QVERIFY(again);
        QCOMPARE(again->text(), QStringLiteral("Xh\u00e9llo\nworld\n"));
        attach(again);
        insert(again, 0, QStringLiteral("a"));
        insert(again, 0, QString());
        QVERIFY(saveAndWait(again));
        QCOMPARE(read(path), utf16(QStringLiteral("aXh\u00e9llo\r\nworld\r\n"), false));
        QVERIFY(before != read(path));
    }

    void kioRefusals()
    {
        Remote::setForceKio(true);
        DocumentList *list = newList();
        QSignalSpy failed(list, &DocumentList::openFailed);
        const int tabs = list->rowCount();
        QFile big(write(QStringLiteral("big.txt"), QByteArray()));
        QVERIFY(big.open(QIODevice::WriteOnly));
        QVERIFY(big.resize(Limits::fileBytes + 1));
        big.close();
        list->open({QUrl::fromLocalFile(big.fileName())});
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 20000);
        QVERIFY(failed.last().at(0).toString().contains(QStringLiteral("too large")));
        QVERIFY(QDir().mkpath(m_dir + QStringLiteral("/folder")));
        list->open({QUrl::fromLocalFile(m_dir + QStringLiteral("/folder"))});
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 2, 20000);
        QVERIFY(failed.last().at(0).toString().contains(QStringLiteral("is a folder")));
        list->open({QUrl::fromLocalFile(m_dir + QStringLiteral("/missing.txt"))});
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 3, 20000);
        QVERIFY(failed.last().at(0).toString().contains(QStringLiteral("doesn't exist")));
        list->open({QUrl(QStringLiteral("nosuchscheme://host/x.txt"))});
        QCOMPARE(failed.size(), 4);
        // Only the schemes Notepad knows (admin: is out, trash: isn't a file).
        list->open({QUrl(QStringLiteral("admin:///etc/hosts"))});
        list->open({QUrl(QStringLiteral("trash:/x.txt"))});
        QCOMPARE(failed.size(), 6);
        QVERIFY(Remote::unsupported(QUrl(QStringLiteral("sftp://h/x")), true).isEmpty());
        QVERIFY(Remote::unsupported(QUrl(QStringLiteral("smb://h/s/x")), true).isEmpty());
        QVERIFY(Remote::unsupported(QUrl(QStringLiteral("https://h/x")), false).isEmpty());
        QVERIFY(!Remote::unsupported(QUrl(QStringLiteral("https://h/x")), true).isEmpty()); // read-only: Save As
        QVERIFY(!Remote::unsupported(QUrl(QStringLiteral("zip:/a.zip/x")), true).isEmpty());
        QVERIFY(!Remote::unsupported(QUrl(QStringLiteral("admin:///x")), false).isEmpty());
        QVERIFY(!Remote::unsupported(QUrl(QStringLiteral("fish://h/x")), false).isEmpty());
        QTRY_COMPARE(list->rowCount(), tabs); // refused tabs are gone again
        QVERIFY(!list->canReopenClosed()); // and aren't offered back
    }

    void kioConflictAndFailure()
    {
        const QString path = write(QStringLiteral("c.txt"), "one\n");
        Document *doc = openKio(newList(), path);
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("mine "));
        // Someone else writes it meanwhile (a later mtime).
        QFile other(path);
        QVERIFY(other.open(QIODevice::WriteOnly | QIODevice::Truncate));
        other.write("theirs\n");
        QVERIFY(other.setFileTime(QDateTime::currentDateTimeUtc().addSecs(60), QFileDevice::FileModificationTime));
        other.close();
        QSignalSpy failed(doc, &Document::saveFailed);
        doc->save();
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 20000);
        QCOMPARE(doc->banner(), Document::ChangedOnDisk);
        QCOMPARE(read(path), QByteArray("theirs\n"));
        QVERIFY(doc->isModified());
        doc->keepMine();
        QVERIFY(saveAndWait(doc));
        QCOMPARE(read(path), QByteArray("mine one\n"));

        // A failed write keeps the text and the modified state.
        insert(doc, 0, QStringLiteral("more "));
        QSignalSpy failed2(doc, &Document::saveFailed);
        doc->saveAs(QUrl::fromLocalFile(m_dir + QStringLiteral("/nope/x.txt")));
        QTRY_COMPARE_WITH_TIMEOUT(failed2.size(), 1, 20000);
        QCOMPARE(doc->banner(), Document::SaveFailed);
        QVERIFY(doc->isModified());
        QCOMPARE(doc->text(), QStringLiteral("more mine one\n"));
    }

    void kioCancelFirstOpenLeavesNoClosedTab()
    {
        Remote::setForceKio(true);
        DocumentList *list = newList();
        QSignalSpy emptied(list, &DocumentList::empty);
        const int tabs = list->rowCount();
        const QString path = write(QStringLiteral("cancel.txt"), "x\n");
        list->open({QUrl::fromLocalFile(path)});
        Document *doc = list->current();
        QVERIFY(doc);
        QVERIFY(doc->isFetching()); // the stat hasn't come back yet
        QPointer<Document> guard = doc;
        doc->cancelLoad();
        // The only tab: an empty untitled one stays (the window doesn't close).
        QCOMPARE(list->rowCount(), qMax(tabs, 1));
        QVERIFY(list->current());
        QVERIFY(list->current() != doc);
        QVERIFY(tabs > 0 || list->current()->path().isEmpty());
        QVERIFY(!list->canReopenClosed());
        QCOMPARE(emptied.count(), 0);
        QTRY_VERIFY(!guard);
    }

    // ---- sftp, against a real sshd: scripts/kio-sftp-test.sh sets
    // NP_KIO_TEST_BASE=sftp://user@127.0.0.1:2222/<folder the test may use>.
    static QUrl sftpUrl(const QString &name)
    {
        QUrl url(qEnvironmentVariable("NP_KIO_TEST_BASE"));
        url.setPath(url.path() + QLatin1Char('/') + name);
        return url;
    }
    // The same folder as the server sees it, to set files up and check them.
    static QString sftpLocal(const QString &name)
    {
        return QUrl(qEnvironmentVariable("NP_KIO_TEST_BASE")).path() + QLatin1Char('/') + name;
    }
    Document *openSftp(DocumentList *list, const QString &name)
    {
        list->open({sftpUrl(name)});
        Document *doc = list->current();
        if (!doc || !doc->isRemote()) {
            return nullptr;
        }
        QPointer<Document> guard = doc;
        const bool done = QTest::qWaitFor([&] { return !guard || !guard->isLoading(); }, 30000);
        return done && guard ? doc : nullptr;
    }
    static void putFile(const QString &path, const QByteArray &bytes)
    {
        QDir().mkpath(QFileInfo(path).path());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
    }

    void sftpRoundTrip()
    {
        if (qEnvironmentVariableIsEmpty("NP_KIO_TEST_BASE")) {
            QSKIP("needs a sftp server: scripts/kio-sftp-test.sh");
        }
        const QByteArray bytes = utf16(QStringLiteral("h\u00e9llo\r\nsftp\r\n"), true);
        putFile(sftpLocal(QStringLiteral("rt.txt")), bytes);
        Document *doc = openSftp(newList(), QStringLiteral("rt.txt"));
        QVERIFY(doc);
        QCOMPARE(doc->text(), QStringLiteral("h\u00e9llo\nsftp\n"));
        QCOMPARE(int(doc->encoding()), int(NP_UTF16_BE));
        attach(doc);
        insert(doc, 0, QStringLiteral("X"));
        QVERIFY(saveAndWait(doc));
        QVERIFY(!doc->isModified());
        QCOMPARE(read(sftpLocal(QStringLiteral("rt.txt"))), utf16(QStringLiteral("Xh\u00e9llo\r\nsftp\r\n"), true));
        newList()->closeDocument(doc);
        Document *again = openSftp(newList(), QStringLiteral("rt.txt"));
        QVERIFY(again);
        QCOMPARE(again->text(), QStringLiteral("Xh\u00e9llo\nsftp\n"));
        QVERIFY(!again->isModified());
        // No edit, a save: the bytes are the ones that were read.
        attach(again);
        insert(again, 0, QStringLiteral("Y"));
        QVERIFY(saveAndWait(again));
        QCOMPARE(read(sftpLocal(QStringLiteral("rt.txt"))), utf16(QStringLiteral("YXh\u00e9llo\r\nsftp\r\n"), true));
    }

    void sftpConflictMissingAndReadOnly()
    {
        if (qEnvironmentVariableIsEmpty("NP_KIO_TEST_BASE")) {
            QSKIP("needs a sftp server: scripts/kio-sftp-test.sh");
        }
        DocumentList *list = newList();
        // Changed on the server after it was read.
        putFile(sftpLocal(QStringLiteral("cf.txt")), "one\n");
        Document *doc = openSftp(list, QStringLiteral("cf.txt"));
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("mine "));
        QFile other(sftpLocal(QStringLiteral("cf.txt")));
        QVERIFY(other.open(QIODevice::WriteOnly | QIODevice::Truncate));
        other.write("theirs\n");
        QVERIFY(other.setFileTime(QDateTime::currentDateTimeUtc().addSecs(60), QFileDevice::FileModificationTime));
        other.close();
        QSignalSpy failed(doc, &Document::saveFailed);
        doc->save();
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 30000);
        QCOMPARE(doc->banner(), Document::ChangedOnDisk);
        QCOMPARE(read(sftpLocal(QStringLiteral("cf.txt"))), QByteArray("theirs\n"));
        doc->keepMine();
        QVERIFY(saveAndWait(doc));
        QCOMPARE(read(sftpLocal(QStringLiteral("cf.txt"))), QByteArray("mine one\n"));

        // A file that isn't there.
        QSignalSpy openFailed(list, &DocumentList::openFailed);
        list->open({sftpUrl(QStringLiteral("nothing-here.txt"))});
        QTRY_COMPARE_WITH_TIMEOUT(openFailed.size(), 1, 30000);
        QVERIFY(openFailed.last().at(0).toString().contains(QStringLiteral("doesn't exist")));

        // A read-only file: the banner that offers Save As, and no write.
        putFile(sftpLocal(QStringLiteral("ro.txt")), "fixed\n");
        QVERIFY(QFile::setPermissions(sftpLocal(QStringLiteral("ro.txt")), QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther));
        Document *ro = openSftp(list, QStringLiteral("ro.txt"));
        QVERIFY(ro);
        QCOMPARE(ro->banner(), Document::ReadOnlyFile);
    }

    // ---- KDirNotify and KDE integration. These need a session bus
    // (ctest makes one with dbus-run-session); without it they skip.
    bool haveBus()
    {
        // NP_TEST_NO_BUS: ctest had no dbus-run-session; never use an ambient bus.
        const bool bus = !qEnvironmentVariableIsSet("NP_TEST_NO_BUS") && QDBusConnection::sessionBus().isConnected();
        if (bus) {
            m_app->startDirNotify(); // the app starts it a moment after the first frames
        }
        return bus;
    }

    void dirNotifyRename()
    {
        if (!haveBus()) {
            QSKIP("no session bus");
        }
        const QString from = write(QStringLiteral("a.md"), "# a\n");
        Document *doc = openFile(newList(), from);
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("typed "));
        QSignalSpy notice(m_app, &App::notice);
        const QString to = m_dir + QStringLiteral("/b.md");
        QVERIFY(QFile::rename(from, to));
        OrgKdeKDirNotifyInterface::emitFileRenamed(QUrl::fromLocalFile(from), QUrl::fromLocalFile(to));
        QTRY_COMPARE_WITH_TIMEOUT(doc->path(), to, 5000);
        QCOMPARE(doc->title(), QStringLiteral("b.md"));
        QVERIFY(doc->isModified()); // the text and its state stay
        QCOMPARE(doc->text(), QStringLiteral("typed # a\n"));
        QVERIFY(doc->banner() != Document::Deleted);
        QCOMPARE(notice.size(), 1);
        QVERIFY(saveAndWait(doc)); // saves to the new place
        QCOMPARE(read(to), QByteArray("typed # a\n"));
        QVERIFY(!QFileInfo::exists(from));
        QCOMPARE(m_app->recentFiles().value(0), to);
    }

    void dirNotifyFolderMove()
    {
        if (!haveBus()) {
            QSKIP("no session bus");
        }
        QVERIFY(QDir().mkpath(m_dir + QStringLiteral("/old/sub")));
        const QString from = write(QStringLiteral("old/sub/n.txt"), "x\n");
        Document *doc = openFile(newList(), from);
        QVERIFY(doc);
        QVERIFY(QDir().rename(m_dir + QStringLiteral("/old"), m_dir + QStringLiteral("/new")));
        OrgKdeKDirNotifyInterface::emitFileMoved(QUrl::fromLocalFile(m_dir + QStringLiteral("/old")), QUrl::fromLocalFile(m_dir + QStringLiteral("/new")));
        QTRY_COMPARE_WITH_TIMEOUT(doc->path(), m_dir + QStringLiteral("/new/sub/n.txt"), 5000);
        QVERIFY(doc->banner() != Document::Deleted);
    }

    void dirNotifyRemoved()
    {
        if (!haveBus()) {
            QSKIP("no session bus");
        }
        const QString path = write(QStringLiteral("gone.txt"), "x\n");
        Document *doc = openKio(newList(), path); // no file watcher: only the notice tells
        QVERIFY(doc);
        QVERIFY(QFile::remove(path));
        OrgKdeKDirNotifyInterface::emitFilesRemoved({QUrl::fromLocalFile(path)});
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::Deleted, 20000);
    }

    void recentDocumentsHaveNoPassword()
    {
        m_app->addRecentFile(QStringLiteral("sftp://user:secret@host/dir/kde-recent.txt"));
        const QString local = write(QStringLiteral("kde-local.txt"), "x\n");
        QVERIFY(openFile(newList(), local)); // opening counts too
        QTest::qWait(1500); // KDE's file is written a moment after the open
        QString all;
        QDirIterator it(QString::fromUtf8(qgetenv("XDG_DATA_HOME")), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            all += QString::fromUtf8(read(it.next()));
        }
        QVERIFY(all.contains(QStringLiteral("host/dir/kde-recent.txt")));
        // (KRecentDocument leaves out files under /tmp, where these live: the
        // local file is only in Notepad's own list.)
        QVERIFY(m_app->recentFiles().contains(local));
        QVERIFY(!all.contains(QStringLiteral("secret")));
        QVERIFY(all.contains(QStringLiteral("net.eterneon.atlas.notepad")));
    }
    void recentDocumentsWrittenOnQuit()
    {
        // Quitting before the deferred write still records the file.
        m_app->addRecentFile(QStringLiteral("sftp://host/dir/opened-then-quit.txt"));
        delete m_app;
        m_app = new App(nullptr);
        QString all;
        QDirIterator it(QString::fromUtf8(qgetenv("XDG_DATA_HOME")), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            all += QString::fromUtf8(read(it.next()));
        }
        QVERIFY(all.contains(QStringLiteral("host/dir/opened-then-quit.txt")));
    }

    void recentDocumentsFollowKdeSetting()
    {
        // Plasma's "remember recent documents" switched off ([RecentDocuments]
        // UseRecent in kdeglobals): KRecentDocument records nothing.
        const QString config = QString::fromUtf8(qgetenv("XDG_CONFIG_HOME"));
        QVERIFY(QDir().mkpath(config));
        const QString globals = config + QStringLiteral("/kdeglobals");
        auto restore = qScopeGuard([&globals] {
            QFile::remove(globals);
            KSharedConfig::openConfig()->reparseConfiguration();
        });
        QFile file(globals);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[RecentDocuments]\nUseRecent=false\n");
        file.close();
        KSharedConfig::openConfig()->reparseConfiguration();

        m_app->addRecentFile(QStringLiteral("sftp://host/dir/not-remembered.txt"));
        QTest::qWait(1500); // KDE's file is written a moment after the open
        QString all;
        QDirIterator it(QString::fromUtf8(qgetenv("XDG_DATA_HOME")), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            all += QString::fromUtf8(read(it.next()));
        }
        QVERIFY(!all.contains(QStringLiteral("not-remembered.txt")));
    }

    void copyLocationAndActions()
    {
        QApplication::clipboard()->setText(QStringLiteral("before"));
        Document *untitled = newList()->newTab();
        untitled->copyLocation();
        untitled->showInFolder();
        untitled->openWith();
        untitled->showProperties(); // nothing for a tab with no file
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("before"));

        const QString path = write(QStringLiteral("loc.txt"), "x\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        QSignalSpy notice(m_app, &App::notice);
        doc->copyLocation();
        QCOMPARE(QApplication::clipboard()->text(), path);
        QCOMPARE(notice.size(), 1);
        // Open Containing Folder, Open With and Properties need a desktop: they
        // start their job or dialog, and nothing crashes.
        if (haveBus()) {
            doc->showInFolder();
            doc->showProperties();
            doc->openWith();
        }
        QTest::qWait(300);
        const auto widgets = QApplication::topLevelWidgets();
        for (QWidget *w : widgets) {
            w->close();
        }
        QTest::qWait(100);

        Remote::setForceKio(false);
        Document *remote = newList()->newTab();
        remote->d->setRemote(QUrl(QStringLiteral("sftp://user:secret@127.0.0.1:1/r.txt")));
        remote->d->path = Remote::display(remote->d->url);
        remote->copyLocation();
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("sftp://user@127.0.0.1:1/r.txt"));
    }

    void kioSaveRaisesNoBanner()
    {
        const QString path = write(QStringLiteral("quiet.txt"), "one\n");
        Document *doc = openKio(newList(), path);
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("two "));
        QVERIFY(saveAndWait(doc));
        QTest::qWait(700); // KIO's own FilesChanged and the stat after it
        QCOMPARE(doc->banner(), Document::NoBanner);
        QCOMPARE(doc->path(), Remote::display(QUrl::fromLocalFile(path)));
        QVERIFY(!doc->isModified());
    }

    void relocateDuringRemoteSave()
    {
        const QString a = write(QStringLiteral("ra.txt"), "one\n");
        const QString b = m_dir + QStringLiteral("/rb.txt");
        Document *doc = openKio(newList(), a);
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("X "));
        doc->d->keepMine = true; // no conflict check: the race below may leave a newer time on rb.txt
        doc->save();
        QVERIFY(doc->d->saving); // the stat and write are in flight; the event loop hasn't run
        QVERIFY(QFile::rename(a, b));
        QVERIFY(doc->d->relocate(QUrl::fromLocalFile(b)));
        // The first save went to the old name; the text is saved again to the new one.
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isModified() && !doc->d->saving, 20000);
        QCOMPARE(read(b), QByteArray("X one\n"));
        QCOMPARE(doc->path(), Remote::display(QUrl::fromLocalFile(b)));
        QCOMPARE(doc->text(), QStringLiteral("X one\n"));
    }

    void renameDuringRemoteLoad()
    {
        const QString a = write(QStringLiteral("la.txt"), "one\n");
        const QString b = m_dir + QStringLiteral("/lb.txt");
        Remote::setForceKio(true);
        DocumentList *list = newList();
        list->open({QUrl::fromLocalFile(a)});
        Document *doc = list->current();
        QVERIFY(doc && doc->isFetching());
        QVERIFY(QFile::rename(a, b));
        QVERIFY(doc->d->relocate(QUrl::fromLocalFile(b)));
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isLoading(), 20000);
        QCOMPARE(doc->text(), QStringLiteral("one\n"));
        QCOMPARE(doc->banner(), Document::NoBanner);
        QCOMPARE(doc->title(), QStringLiteral("lb.txt"));
    }

    void dirNotifyDeletedThenMovedRemote()
    {
        if (!haveBus()) {
            QSKIP("no session bus");
        }
        const QString a = write(QStringLiteral("da.txt"), "one\n");
        const QString b = m_dir + QStringLiteral("/db.txt");
        Document *doc = openKio(newList(), a);
        QVERIFY(doc);
        QVERIFY(QFile::rename(a, b));
        OrgKdeKDirNotifyInterface::emitFilesRemoved({QUrl::fromLocalFile(a)});
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::Deleted, 20000);
        OrgKdeKDirNotifyInterface::emitFileMoved(QUrl::fromLocalFile(a), QUrl::fromLocalFile(b));
        QTRY_COMPARE_WITH_TIMEOUT(doc->path(), Remote::display(QUrl::fromLocalFile(b)), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(doc->banner() != Document::Deleted, 20000);
    }

    void dirNotifyRemoteMoveToOtherFolderIsOffered()
    {
        if (!haveBus()) {
            QSKIP("no session bus");
        }
        const QString a = write(QStringLiteral("oa.txt"), "one\n");
        QVERIFY(QDir().mkpath(m_dir + QStringLiteral("/sub")));
        const QString b = m_dir + QStringLiteral("/sub/ob.txt");
        Document *doc = openKio(newList(), a);
        QVERIFY(doc);
        QVERIFY(QFile::rename(a, b));
        OrgKdeKDirNotifyInterface::emitFileMoved(QUrl::fromLocalFile(a), QUrl::fromLocalFile(b));
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::Moved, 20000);
        QCOMPARE(doc->path(), Remote::display(QUrl::fromLocalFile(a))); // not followed by itself
        doc->followMove(); // checks the new place first, then goes
        QTRY_COMPARE_WITH_TIMEOUT(doc->path(), Remote::display(QUrl::fromLocalFile(b)), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(doc->banner() != Document::Moved && doc->banner() != Document::Deleted, 20000);
    }

    void dirNotifyChainedRenames()
    {
        if (!haveBus()) {
            QSKIP("no session bus");
        }
        for (const bool kio : {false, true}) {
            const QString a = write(QStringLiteral("xa%1.txt").arg(kio), "one\n");
            const QString b = m_dir + QStringLiteral("/xb%1.txt").arg(kio);
            const QString c = m_dir + QStringLiteral("/xc%1.txt").arg(kio);
            DocumentList *list = newList();
            Document *doc = kio ? openKio(list, a) : openFile(list, a);
            QVERIFY(doc);
            // a to b to c in one batch: the tab ends at c.
            QVERIFY(QFile::rename(a, b));
            OrgKdeKDirNotifyInterface::emitFileRenamed(QUrl::fromLocalFile(a), QUrl::fromLocalFile(b));
            QVERIFY(QFile::rename(b, c));
            OrgKdeKDirNotifyInterface::emitFileRenamed(QUrl::fromLocalFile(b), QUrl::fromLocalFile(c));
            const QString shown = kio ? Remote::display(QUrl::fromLocalFile(c)) : c;
            QTRY_COMPARE_WITH_TIMEOUT(doc->path(), shown, 20000);
            QTRY_VERIFY_WITH_TIMEOUT(doc->banner() != Document::Deleted, 20000);
        }
        // While a check is running a notice for where it is heading is kept.
        const QString a = write(QStringLiteral("ya.txt"), "one\n");
        Document *doc = openKio(newList(), a);
        QVERIFY(doc);
        const QUrl b = QUrl::fromLocalFile(m_dir + QStringLiteral("/yb.txt"));
        const QUrl c = QUrl::fromLocalFile(m_dir + QStringLiteral("/yc.txt"));
        doc->d->validating = true;
        doc->d->validateTarget = b;
        QVERIFY(QFile::rename(a, c.toLocalFile()));
        OrgKdeKDirNotifyInterface::emitFileRenamed(b, c);
        QTRY_VERIFY_WITH_TIMEOUT(doc->d->hasPendingMove, 5000);
        QCOMPARE(doc->d->pendingTo, c);
        // A later batch carries on from the notice already waiting.
        const QUrl d = QUrl::fromLocalFile(m_dir + QStringLiteral("/yd.txt"));
        QVERIFY(QFile::rename(c.toLocalFile(), d.toLocalFile()));
        OrgKdeKDirNotifyInterface::emitFileRenamed(c, d);
        QTRY_COMPARE_WITH_TIMEOUT(doc->d->pendingTo, d, 5000);
    }

    void movedBannerClearsOnReload()
    {
        const QString a = write(QStringLiteral("mb.txt"), "one\n");
        Document *doc = openFile(newList(), a);
        QVERIFY(doc);
        doc->d->offerMove(QUrl::fromLocalFile(a), QUrl::fromLocalFile(a + QStringLiteral(".x")));
        QCOMPARE(doc->banner(), Document::Moved);
        // Control and bidi characters never reach the text; long ones are cut.
        QVERIFY(!doc->bannerText().contains(QChar(0x202E)));
        doc->d->offerMove(QUrl::fromLocalFile(a), QUrl::fromLocalFile(QStringLiteral("/tmp/") + QString(200, QLatin1Char('x')) + QChar(0x202E) + QStringLiteral(".txt")));
        QVERIFY(!doc->bannerText().contains(QChar(0x202E)));
        QVERIFY(doc->bannerText().size() < 200);
        // Nor other invisible ones, and the cut keeps surrogate pairs whole.
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        QString odd = QStringLiteral("/tmp/a") + QChar(0x061C) + QChar(0x200B) + QChar(0x2028) + QChar(0x2029) + QStringLiteral("b/");
        for (int i = 0; i < 60; ++i) {
            odd += emoji;
        }
        doc->d->offerMove(QUrl::fromLocalFile(a), QUrl::fromLocalFile(odd + QStringLiteral(".txt")));
        const QString text = doc->bannerText();
        for (const char16_t u : {u'\u061C', u'\u200B', u'\u2028', u'\u2029'}) {
            QVERIFY(!text.contains(QChar(u)));
        }
        for (qsizetype i = 0; i < text.size(); ++i) {
            QVERIFY(!text.at(i).isHighSurrogate() || (i + 1 < text.size() && text.at(i + 1).isLowSurrogate()));
            QVERIFY(!text.at(i).isLowSurrogate() || (i > 0 && text.at(i - 1).isHighSurrogate()));
        }
        doc->reload();
        QTRY_VERIFY(doc->banner() != Document::Moved);
    }

    void dirNotifyMovesAreChecked()
    {
        if (!haveBus()) {
            QSKIP("no session bus");
        }
        const QString a = write(QStringLiteral("ma.txt"), "one\n");
        Document *doc = openFile(newList(), a);
        QVERIFY(doc);
        // Nothing at the new place: a claim, not a move.
        OrgKdeKDirNotifyInterface::emitFileMoved(QUrl::fromLocalFile(a), QUrl::fromLocalFile(m_dir + QStringLiteral("/nowhere.txt")));
        // A copy (the old place is still there).
        const QString copy = write(QStringLiteral("mcopy.txt"), "one\n");
        OrgKdeKDirNotifyInterface::emitFileMoved(QUrl::fromLocalFile(a), QUrl::fromLocalFile(copy));
        // Another kind of place: the trash.
        OrgKdeKDirNotifyInterface::emitFileMoved(QUrl::fromLocalFile(a), QUrl(QStringLiteral("trash:/ma.txt")));
        QTest::qWait(600);
        QCOMPARE(doc->path(), a);
        QVERIFY(doc->banner() != Document::Deleted);
    }

    void dirNotifyRenamesClosedTabs()
    {
        if (!haveBus()) {
            QSKIP("no session bus");
        }
        const QString a = write(QStringLiteral("ca.txt"), "one\n");
        const QString b = m_dir + QStringLiteral("/cb.txt");
        DocumentList *list = newList();
        Document *doc = openFile(list, a);
        QVERIFY(doc);
        list->closeDocument(doc);
        QVERIFY(QFile::rename(a, b));
        OrgKdeKDirNotifyInterface::emitFileMoved(QUrl::fromLocalFile(a), QUrl::fromLocalFile(b));
        QTest::qWait(300);
        list->reopenClosed();
        QVERIFY(list->current());
        QCOMPARE(list->current()->path(), b);
    }

    void kioSessionStripsPassword()
    {
        const QString path = markdownSession();
        QFile f(sessionDir() + QStringLiteral("/session.json"));
        QVERIFY(f.open(QIODevice::ReadWrite));
        QByteArray json = f.readAll();
        const QByteArray old = QJsonDocument(QJsonArray{path}).toJson(QJsonDocument::Compact).mid(1);
        const QByteArray quoted = old.left(old.size() - 1);
        QVERIFY(json.contains(quoted));
        json.replace(quoted, "\"sftp://user:secret@127.0.0.1:1/notes.md\"");
        f.resize(0);
        f.write(json);
        f.close();
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        QVERIFY(doc);
        QVERIFY(doc->isRemote()); // the tab is there at once, loading in the background
        QCOMPARE(doc->path(), QStringLiteral("sftp://user@127.0.0.1:1/notes.md"));
        QVERIFY(!doc->toolTip().contains(QStringLiteral("secret")));
        QCOMPARE(doc->host(), QStringLiteral("127.0.0.1"));
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::ReadFailed, 30000); // nothing listens there
        m_app->saveSession();
        const QByteArray saved = read(sessionDir() + QStringLiteral("/session.json"));
        QVERIFY(!saved.contains("secret"));
        QVERIFY(saved.contains("sftp://user@127.0.0.1:1/notes.md"));
        m_app->addRecentFile(QStringLiteral("sftp://user:secret@host/a.txt"));
        QCOMPARE(m_app->recentFiles().value(0), QStringLiteral("sftp://user@host/a.txt"));
    }

    void argumentUrls()
    {
        QCOMPARE(App::urlFromArgument(QStringLiteral("a.txt"), QStringLiteral("/w")), QUrl::fromLocalFile(QStringLiteral("/w/a.txt")));
        QCOMPARE(App::urlFromArgument(QStringLiteral("/x/a.txt"), QString()), QUrl::fromLocalFile(QStringLiteral("/x/a.txt")));
        QVERIFY(!App::urlFromArgument(QStringLiteral("a.txt"), QString()).isValid());
        QCOMPARE(App::urlFromArgument(QStringLiteral("sftp://h/a.txt"), QString()), QUrl(QStringLiteral("sftp://h/a.txt")));
        QCOMPARE(App::urlFromArgument(QStringLiteral("file:///x/a%20b.txt"), QString()), QUrl::fromLocalFile(QStringLiteral("/x/a b.txt")));
        QCOMPARE(App::urlFromArgument(QStringLiteral("smb://h/s/a.txt"), QStringLiteral("/w")), QUrl(QStringLiteral("smb://h/s/a.txt")));
    }


    void saveAsTakesName()
    {
        Document *doc = newList()->newTab();
        attach(doc);
        insert(doc, 0, QStringLiteral("hi"));
        QSignalSpy saved(doc, &Document::saved);
        doc->saveAs(QUrl::fromLocalFile(m_dir + QStringLiteral("/new.md")));
        QVERIFY(saved.wait(5000));
        QCOMPARE(doc->title(), QStringLiteral("new.md"));
        QVERIFY(doc->isMarkdown());
        QCOMPARE(read(m_dir + QStringLiteral("/new.md")), QByteArray("hi"));
        QVERIFY(!doc->isModified());
        QCOMPARE(m_app->recentFiles().value(0), m_dir + QStringLiteral("/new.md"));
        QSignalSpy again(doc, &Document::saved);
        doc->saveAs(QUrl::fromLocalFile(m_dir + QStringLiteral("/new.txt")));
        QVERIFY(again.wait(5000));
        QVERIFY(!doc->isMarkdown());
    }

    void saveAsRefusesRemoteAndOpenFiles()
    {
        const QString p = write(QStringLiteral("taken.txt"), "theirs\n");
        DocumentList *list = newList();
        QVERIFY(openFile(list, p));
        Document *doc = list->newTab();
        attach(doc);
        insert(doc, 0, QStringLiteral("mine"));
        QSignalSpy failed(doc, &Document::saveFailed);
        // An unknown scheme's URL is never the local <dir>/remote.txt.
        QUrl remote;
        remote.setScheme(QStringLiteral("nosuchscheme")); // KIO would take a real one: see kioConflictAndFailure
        remote.setHost(QStringLiteral("host"));
        remote.setPath(m_dir + QStringLiteral("/remote.txt"));
        doc->saveAs(remote);
        QCOMPARE(failed.size(), 1);
        QVERIFY(!QFileInfo::exists(m_dir + QStringLiteral("/remote.txt")));
        // Another tab has it open: refused, the file untouched.
        doc->saveAs(QUrl::fromLocalFile(p));
        QCOMPARE(failed.size(), 2);
        QVERIFY(failed.at(1).at(0).toString().contains(QStringLiteral("another tab")));
        QCOMPARE(read(p), QByteArray("theirs\n"));
        QVERIFY(doc->path().isEmpty());
        // Or in another window.
        m_app->newWindow();
        QCOMPARE(m_app->windows().size(), 2);
        DocumentList *second = m_app->windows().first() == list ? m_app->windows().last() : m_app->windows().first();
        Document *elsewhere = second->current();
        QVERIFY(elsewhere && elsewhere != doc);
        attach(elsewhere);
        insert(elsewhere, 0, QStringLiteral("mine too"));
        QSignalSpy failedThere(elsewhere, &Document::saveFailed);
        elsewhere->saveAs(QUrl::fromLocalFile(p));
        QCOMPARE(failedThere.size(), 1);
        QCOMPARE(read(p), QByteArray("theirs\n"));
    }

    void namesShownSafely()
    {
        // U+202E would show "a<RLO>txt.exe" as "aexe.txt".
        const QString p = write(QStringLiteral("a\u202Etxt.exe"), "x\n");
        Document *doc = openFile(newList(), p);
        QVERIFY(doc);
        QCOMPARE(doc->title(), QStringLiteral("atxt.exe"));
        QVERIFY(QDir(m_dir).mkdir(QStringLiteral("<b>tag")));
        const QString q = write(QStringLiteral("<b>tag/f\nl.txt"), "y\n");
        Document *other = openFile(newList(), q);
        QVERIFY(other);
        QCOMPARE(other->title(), QStringLiteral("f\uFFFDl.txt"));
        // Zero-width spaces go, line separators show.
        const QString r = write(QStringLiteral("z\u200Bw\u2028x.txt"), "z\n");
        Document *third = openFile(newList(), r);
        QVERIFY(third);
        QCOMPARE(third->title(), QStringLiteral("zw\uFFFDx.txt"));
        QVERIFY(!other->toolTip().contains(QStringLiteral("<b>")));
        QVERIFY(other->toolTip().contains(QStringLiteral("<\u2060b>tag")));
    }

    void linksThatOpen()
    {
        QCOMPARE(m_app->linkUrl(QStringLiteral("https://example.com/a?b=1&c=2")), QStringLiteral("https://example.com/a?b=1&c=2"));
        QCOMPARE(m_app->linkUrl(QStringLiteral("HTTP://Example.com")), QStringLiteral("http://example.com"));
        QCOMPARE(m_app->linkUrl(QStringLiteral("www.example.com")), QStringLiteral("https://www.example.com"));
        QCOMPARE(m_app->linkTarget(QStringLiteral("https://evil.example/login")), QStringLiteral("evil.example"));
        for (const char *refused : {"file:///etc/passwd", "javascript:alert(1)", "data:text/html,hi", "smb://srv/share", "https://", "relative/path",
                                    " https://example.com", "mailto:"}) {
            QVERIFY2(m_app->linkUrl(QString::fromUtf8(refused)).isEmpty(), refused);
            QVERIFY(!m_app->openLink(QString::fromUtf8(refused)));
        }
        // A mailto keeps its address, subject and body, not its attachments.
        const QString mail = m_app->linkUrl(QStringLiteral("mailto:a@b.example?subject=Hi&attach=/home/u/.ssh/id_rsa&Attachment=x&body=yo"));
        QCOMPARE(mail, QStringLiteral("mailto:a@b.example?subject=Hi&body=yo"));
        QCOMPARE(m_app->linkUrl(QStringLiteral("mailto:a@b.example?attach=%2Fetc%2Fpasswd")), QStringLiteral("mailto:a@b.example"));
        QCOMPARE(m_app->linkTarget(QStringLiteral("mailto:a@b.example?subject=Hi")), QStringLiteral("a@b.example"));
        // Encoded keys don't slip past; the other fields stay byte for byte.
        QCOMPARE(m_app->linkUrl(QStringLiteral("mailto:a@b.example?%61ttach=x&subject=a+b&body=x%20y%2Bz")),
                 QStringLiteral("mailto:a@b.example?subject=a+b&body=x%20y%2Bz"));
        QCOMPARE(m_app->linkUrl(QStringLiteral("mailto:a@b.example?subject=a+b&body=x%20y%2Bz")), QStringLiteral("mailto:a@b.example?subject=a+b&body=x%20y%2Bz"));
        // A lookalike host shows as punycode, as the tooltip does.
        QCOMPARE(m_app->linkTarget(QStringLiteral("https://\u0430pple.com/")), QStringLiteral("xn--pple-43d.com"));
    }

    void proseIsWhatSpellCheckReads()
    {
        DocumentList *list = newList();
        QVERIFY(list->newTab()->isProse()); // untitled
        for (const char *name : {"a.md", "b.txt", "README", "c.TXT"}) {
            Document *doc = openFile(list, write(QLatin1String(name), "words\n"));
            QVERIFY2(doc && doc->isProse(), name);
        }
        for (const char *name : {"d.json", "e.js", "f.conf", "g.py"}) {
            Document *doc = openFile(list, write(QLatin1String(name), "{}\n"));
            QVERIFY2(doc && !doc->isProse(), name);
        }
        // Renamed to code by Save As: no longer prose.
        Document *doc = openFile(list, write(QStringLiteral("h.txt"), "x\n"));
        QSignalSpy saved(doc, &Document::saved);
        doc->saveAs(QUrl::fromLocalFile(m_dir + QStringLiteral("/h.rs")));
        QVERIFY(saved.wait(5000));
        QVERIFY(!doc->isProse());
    }

    void externalChangeWhileUnmodifiedReloads()
    {
        const QString path = write(QStringLiteral("x.txt"), "one\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        write(QStringLiteral("x.txt"), "two two\n");
        doc->d->checkOnDisk();
        QTRY_COMPARE(doc->text(), QStringLiteral("two two\n"));
        QVERIFY(!doc->isModified());
        QCOMPARE(doc->banner(), Document::NoBanner);
    }

    void externalChangeSeenByWatcher()
    {
        const QString path = write(QStringLiteral("w.txt"), "one\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        write(QStringLiteral("w.txt"), "three three\n");
        QTRY_COMPARE_WITH_TIMEOUT(doc->text(), QStringLiteral("three three\n"), 10000);
        // An atomic rename drops the watch; it comes back.
        QVERIFY(QFile::remove(path));
        write(QStringLiteral("w2.txt"), "four four four\n");
        QVERIFY(QFile::rename(m_dir + QStringLiteral("/w2.txt"), path));
        QTRY_COMPARE_WITH_TIMEOUT(doc->text(), QStringLiteral("four four four\n"), 10000);
    }

    void ownSaveIsNotAnExternalChange()
    {
        const QString path = write(QStringLiteral("o.txt"), "one\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("z"));
        QVERIFY(saveAndWait(doc));
        QTest::qWait(400); // the watcher fires for our own rename
        QCOMPARE(doc->banner(), Document::NoBanner);
        QCOMPARE(doc->text(), QStringLiteral("zone\n"));
    }

    void externalChangeWhileModified()
    {
        const QString path = write(QStringLiteral("c.txt"), "one\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("mine "));
        write(QStringLiteral("c.txt"), "theirs!!\n");
        doc->d->checkOnDisk();
        QCOMPARE(doc->banner(), Document::ChangedOnDisk);
        doc->save();
        QCOMPARE(read(path), QByteArray("theirs!!\n")); // refused
        QCOMPARE(doc->banner(), Document::ChangedOnDisk);
        doc->keepMine();
        QVERIFY(saveAndWait(doc));
        QCOMPARE(read(path), QByteArray("mine one\n"));
        QCOMPARE(doc->banner(), Document::NoBanner);
    }

    void deletedFile()
    {
        const QString path = write(QStringLiteral("d.txt"), "one\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(QFile::remove(path));
        doc->d->checkOnDisk();
        QCOMPARE(doc->banner(), Document::Deleted);
        QVERIFY(doc->isModified());
        QVERIFY(saveAndWait(doc)); // recreates it
        QCOMPARE(read(path), QByteArray("one\n"));
        QCOMPARE(doc->banner(), Document::NoBanner);
    }

    void untitledNumbering()
    {
        DocumentList *list = newList();
        list->close(0); // the window's first Untitled
        Document *a = list->newTab();
        Document *b = list->newTab();
        Document *c = list->newTab();
        QCOMPARE(a->title(), QStringLiteral("Untitled"));
        QCOMPARE(b->title(), QStringLiteral("Untitled 2"));
        QCOMPARE(c->title(), QStringLiteral("Untitled 3"));
        list->closeDocument(b);
        QCOMPARE(list->newTab()->title(), QStringLiteral("Untitled 2"));
    }

    void openDedupeAndCurrent()
    {
        DocumentList *list = newList();
        const QString a = write(QStringLiteral("a.txt"), "a");
        const QString b = write(QStringLiteral("b.txt"), "b");
        Document *da = openFile(list, a);
        Document *db = openFile(list, b);
        QVERIFY(da && db);
        QCOMPARE(list->current(), db);
        const int count = list->rowCount();
        list->open({QUrl::fromLocalFile(a)});
        QCOMPARE(list->rowCount(), count);
        QCOMPARE(list->current(), da);
        QCOMPARE(list->data(list->index(list->indexOf(da)), DocumentList::TitleRole).toString(), QStringLiteral("a.txt"));
    }

    void reopenClosed()
    {
        DocumentList *list = newList();
        std::vector<QString> paths;
        for (int i = 0; i < 12; ++i) {
            paths.push_back(write(QStringLiteral("r%1.txt").arg(i), "x"));
            QVERIFY(openFile(list, paths.back()));
        }
        QVERIFY(!list->canReopenClosed());
        // Closed from the end: r11 first, r0 last; the ten newest are kept.
        while (list->rowCount() > 1) {
            list->close(list->rowCount() - 1);
        }
        QVERIFY(list->canReopenClosed());
        for (int i = 0; i < 10; ++i) {
            list->reopenClosed();
            QCOMPARE(list->current()->path(), paths[size_t(i)]);
        }
        QVERIFY(!list->canReopenClosed());
        QSignalSpy emptied(list, &DocumentList::empty);
        while (list->rowCount() > 0) {
            list->close(0);
        }
        QCOMPARE(emptied.size(), 1);
    }

    void moveKeepsCurrent()
    {
        DocumentList *list = newList();
        list->close(0);
        Document *a = openFile(list, write(QStringLiteral("ma.txt"), "a"));
        Document *b = openFile(list, write(QStringLiteral("mb.txt"), "b"));
        Document *c = openFile(list, write(QStringLiteral("mc.txt"), "c"));
        QVERIFY(a && b && c);
        list->setCurrentIndex(0);
        list->move(0, 2);
        QCOMPARE(list->documents(), (QList<Document *>{b, c, a}));
        QCOMPARE(list->current(), a);
        list->move(2, 0);
        QCOMPARE(list->documents(), (QList<Document *>{a, b, c}));
    }

    void sessionRoundTrip()
    {
        const QString pathA = write(QStringLiteral("sa.txt"), "alpha\n");
        const QString pathB = write(QStringLiteral("sb.txt"), "beta\n");
        const QString pathC = write(QStringLiteral("sc.txt"), "gamma\n");
        m_app->start({});
        DocumentList *list = m_app->windows().first();
        Document *untitled = list->current();
        attach(untitled);
        insert(untitled, 0, QStringLiteral("typed"));
        Document *a = openFile(list, pathA);
        Document *b = openFile(list, pathB);
        Document *c = openFile(list, pathC);
        QVERIFY(a && b && c);
        attach(a);
        insert(a, 0, QStringLiteral("edited "));
        a->setEncoding(Document::Utf8Bom);
        list->setCurrentIndex(list->indexOf(a));
        m_app->saveSession();
        QVERIFY(QFile::exists(sessionDir() + QStringLiteral("/session.json")));

        restart();
        m_app->start({});
        list = m_app->windows().first();
        QCOMPARE(list->rowCount(), 4);
        const auto docs = list->documents();
        QCOMPARE(docs[0]->path(), QString());
        QCOMPARE(docs[0]->text(), QStringLiteral("typed"));
        QVERIFY(docs[0]->isModified());
        QCOMPARE(docs[1]->path(), pathA);
        QCOMPARE(docs[1]->text(), QStringLiteral("edited alpha\n"));
        QVERIFY(docs[1]->isModified());
        QCOMPARE(docs[1]->encoding(), Document::Utf8Bom);
        QCOMPARE(list->current(), docs[1]);
        QVERIFY(QTest::qWaitFor([&] { return !docs[2]->isLoading() && !docs[3]->isLoading(); }));
        QCOMPARE(docs[2]->text(), QStringLiteral("beta\n"));
        QVERIFY(!docs[2]->isModified());
        QCOMPARE(docs[3]->text(), QStringLiteral("gamma\n"));

        // Closing a tab drops its text file; a changed file is a conflict.
        const QDir texts(sessionDir() + QStringLiteral("/texts"));
        QCOMPARE(texts.entryList(QDir::Files).size(), 2);
        list->close(0);
        m_app->saveSession();
        QCOMPARE(texts.entryList(QDir::Files).size(), 1);
        write(QStringLiteral("sa.txt"), "changed elsewhere\n");
        QVERIFY(QFile::remove(pathB));
        m_app->saveSession();

        restart();
        m_app->start({});
        list = m_app->windows().first();
        QCOMPARE(list->rowCount(), 3);
        QCOMPARE(list->documents()[0]->banner(), Document::ChangedOnDisk);
        QCOMPARE(list->documents()[0]->text(), QStringLiteral("edited alpha\n"));
        QTRY_COMPARE(list->documents()[1]->banner(), Document::Deleted);
    }

    // ---- Light coding

    void codeLanguageFromName()
    {
        DocumentList *list = newList();
        struct Case { const char *name; const char *language; bool code; };
        const Case cases[] = {
            {"a.py", "Python", true},
            {"a.rs", "Rust", true},
            {"a.c", "C", true},
            {"CMakeLists.txt", "CMake", true},
            {"notes.txt", "", false},
            {"readme.md", "", false},
            {"plain.text", "", false},
        };
        for (const Case &c : cases) {
            Document *doc = openFile(list, write(QString::fromLatin1(c.name), "x\n"));
            QVERIFY2(doc, c.name);
            QCOMPARE(doc->language(), QString::fromLatin1(c.language));
            QCOMPARE(doc->property("code").toBool(), c.code);
        }
        // Untitled: not code.
        list->newTab();
        QCOMPARE(list->current()->language(), QString());
        QVERIFY(!list->current()->property("code").toBool());
        // Save As renames the language with the file.
        Document *doc = openFile(list, write(QStringLiteral("r.txt"), "fn main() {}\n"));
        QVERIFY(!doc->property("code").toBool());
        QSignalSpy codeSpy(doc, &Document::codeChanged);
        doc->saveAs(QUrl::fromLocalFile(m_dir + QStringLiteral("/r.rs")));
        QTRY_COMPARE(doc->language(), QStringLiteral("Rust"));
        QVERIFY(doc->property("code").toBool());
        QVERIFY(codeSpy.count() >= 1);
    }

    void codeIndentDetection()
    {
        DocumentList *list = newList();
        struct Case { const char *name; QByteArray text; bool spaces; int width; };
        const Case cases[] = {
            {"i1.c", "int f() {\n\tif (x) {\n\t\ty();\n\t}\n}\n", false, 4},
            {"i2.c", "int f() {\n  if (x) {\n    y();\n  }\n}\n", true, 2},
            {"i3.c", "int f() {\n    if (x) {\n        y();\n    }\n}\n", true, 4},
            {"i4.c", "int f() {\n        y();\n}\n", true, 8},
            // More tab lines than space lines: tabs; the reverse: spaces.
            {"i5.c", "a\n\tb\n\tc\n  d\n", false, 4},
            {"i6.c", "a\n\tb\n    c\n    d\n", true, 4},
            {"i7.c", "", true, 4},
            {"i8.c", "no indent\nat all\n", true, 4},
        };
        for (const Case &c : cases) {
            Document *doc = openFile(list, write(QString::fromLatin1(c.name), c.text));
            QVERIFY2(doc, c.name);
            QVERIFY2(doc->insertSpaces() == c.spaces, c.name);
            QVERIFY2(doc->indentWidth() == c.width, c.name);
        }
        // Only the first 64 KB are read: a file that turns to tabs later is spaces.
        QByteArray big;
        while (big.size() < 70 * 1024) {
            big += "    x\n";
        }
        for (int i = 0; i < 20000; ++i) {
            big += "\tx\n";
        }
        Document *doc = openFile(list, write(QStringLiteral("big.c"), big));
        QVERIFY(doc->insertSpaces());
    }

    void codeUserSettingsPersist()
    {
        const QString path = write(QStringLiteral("p.txt"), "\tx\n");
        m_app->start({});
        DocumentList *list = m_app->windows().first();
        Document *doc = openFile(list, path);
        QVERIFY(doc);
        QVERIFY(!doc->insertSpaces() && doc->language().isEmpty());
        doc->setLanguage(QStringLiteral("Python"));
        doc->setInsertSpaces(true);
        doc->setIndentWidth(3);
        QVERIFY(doc->property("code").toBool());
        Document *other = openFile(list, write(QStringLiteral("q.py"), "def f():\n  pass\n"));
        other->setIndentWidth(99); // clamped
        QCOMPARE(other->indentWidth(), 16);
        const QString detectedPath = write(QStringLiteral("r.py"), "x\n");
        openFile(list, detectedPath);
        m_app->saveSession();

        restart();
        m_app->start({});
        list = m_app->windows().first();
        const auto docs = list->documents();
        auto byPath = [&](const QString &p) -> Document * {
            for (Document *d : docs) {
                if (d->path() == p) {
                    return d;
                }
            }
            return nullptr;
        };
        Document *a = byPath(path);
        Document *b = byPath(m_dir + QStringLiteral("/q.py"));
        QVERIFY(a && b);
        QVERIFY(QTest::qWaitFor([&] { return !a->isLoading(); }));
        QCOMPARE(a->path(), path);
        QCOMPARE(a->language(), QStringLiteral("Python")); // wins over detection (none)
        QVERIFY(a->property("code").toBool());
        QVERIFY(a->insertSpaces()); // wins over the file's tab
        QCOMPARE(a->indentWidth(), 3);
        QVERIFY(QTest::qWaitFor([&] { return !b->isLoading(); }));
        QCOMPARE(b->language(), QStringLiteral("Python"));
        QCOMPARE(b->indentWidth(), 16);
        // Not set by the user: not in the session file.
        const QJsonArray tabs = sessionJson().value(QStringLiteral("windows")).toArray().at(0).toObject().value(QStringLiteral("tabs")).toArray();
        bool seen = false;
        for (const QJsonValue &t : tabs) {
            if (t.toObject().value(QStringLiteral("path")).toString() == detectedPath) {
                seen = true;
                QVERIFY(!t.toObject().contains(QStringLiteral("language")));
                QVERIFY(!t.toObject().contains(QStringLiteral("indentWidth")));
            }
        }
        QVERIFY(seen);
    }

    void codeLineNumbersSetting()
    {
        QVERIFY(m_app->settings()->codeLineNumbers());
        QSignalSpy spy(m_app->settings(), &Settings::codeLineNumbersChanged);
        m_app->settings()->setCodeLineNumbers(false);
        QVERIFY(!m_app->settings()->codeLineNumbers());
        QCOMPARE(spy.count(), 1);
        m_app->settings()->setCodeLineNumbers(true);
    }

    void sessionOffStartsEmpty()
    {
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        insert(doc, 0, QStringLiteral("typed"));
        m_app->saveSession();
        m_app->settings()->setContinueSession(false);
        m_app->saveSession();
        restart();
        m_app->start({});
        QCOMPARE(m_app->windows().first()->rowCount(), 1);
        QCOMPARE(m_app->windows().first()->current()->text(), QString());
    }

    void recentFiles()
    {
        QStringList paths;
        for (int i = 0; i < 12; ++i) {
            paths << write(QStringLiteral("rf%1.txt").arg(i), "x");
            m_app->addRecentFile(paths.last());
        }
        QCOMPARE(m_app->recentFiles().size(), 10);
        QCOMPARE(m_app->recentFiles().first(), paths[11]);
        QCOMPARE(m_app->recentFiles().last(), paths[2]);
        m_app->addRecentFile(paths[5]);
        QCOMPARE(m_app->recentFiles().first(), paths[5]);
        QCOMPARE(m_app->recentFiles().size(), 10);
        QCOMPARE(m_app->recentFiles().count(paths[5]), 1);
        QVERIFY(QFile::remove(paths[11]));
        QVERIFY(!m_app->recentFiles().contains(paths[11]));
        m_app->clearRecentFiles();
        QVERIFY(m_app->recentFiles().isEmpty());
    }

    void settingsDefaultsAndZoom()
    {
        Settings *s = m_app->settings();
        QVERIFY(s->wordWrap());
        QVERIFY(!s->lineNumbers());
        QVERIFY(s->statusBar() && s->formattingToolbar() && s->formatting() && s->openMarkdownFormatted() && s->continueSession());
        QVERIFY(!s->openInNewWindow() && !s->gpuRendering());
        QCOMPARE(s->zoom(), 100);
        s->setZoom(47);
        QCOMPARE(s->zoom(), 50);
        s->setZoom(123);
        QCOMPARE(s->zoom(), 120);
        s->setZoom(999);
        QCOMPARE(s->zoom(), 400);
        s->setGpuRendering(true);
        QVERIFY(Settings::readGpuRendering());
    }

    void findCountWrapRegexWords()
    {
        Document *doc = newList()->newTab();
        attach(doc);
        insert(doc, 0, QStringLiteral("Foo foo\nbar foo foobar"));
        QVariantMap r = doc->find(QStringLiteral("foo"), 0, 0, false);
        QCOMPARE(r.value("start").toInt(), 0);
        QCOMPARE(r.value("end").toInt(), 3);
        QCOMPARE(r.value("index").toInt(), 1);
        QCOMPARE(r.value("count").toInt(), 4);
        r = doc->find(QStringLiteral("foo"), 0, 1, false);
        QCOMPARE(r.value("start").toInt(), 4);
        QCOMPARE(r.value("index").toInt(), 2);
        r = doc->find(QStringLiteral("foo"), 0, 100, false); // wraps
        QCOMPARE(r.value("index").toInt(), 1);
        r = doc->find(QStringLiteral("foo"), 0, 0, true); // backwards wraps
        QCOMPARE(r.value("index").toInt(), 4);
        r = doc->find(QStringLiteral("foo"), 0, 12, true);
        QCOMPARE(r.value("index").toInt(), 2);
        QCOMPARE(doc->find(QStringLiteral("foo"), Document::MatchCase, 0, false).value("count").toInt(), 3);
        QCOMPARE(doc->find(QStringLiteral("foo"), Document::WholeWords, 0, false).value("count").toInt(), 3);
        QCOMPARE(doc->find(QStringLiteral("nothing"), 0, 0, false).value("count").toInt(), 0);
        QCOMPARE(doc->find(QString(), 0, 0, false).value("count").toInt(), 0);
        r = doc->find(QStringLiteral("(foo"), Document::RegularExpression, 0, false);
        QVERIFY(r.contains("error") && !r.value("error").toString().isEmpty());
        r = doc->find(QStringLiteral("^bar"), Document::RegularExpression, 0, false);
        QCOMPARE(r.value("start").toInt(), 8);
        r = doc->find(QStringLiteral("fo+"), Document::RegularExpression, 0, false);
        QCOMPARE(r.value("count").toInt(), 4);
        // The count stops at 10000.
        Document *many = newList()->newTab();
        attach(many);
        insert(many, 0, QString(12000, QLatin1Char('a')));
        QCOMPARE(many->find(QStringLiteral("a"), 0, 0, false).value("count").toInt(), 10000);
    }

    void replaceAllIsOneUndo()
    {
        Document *doc = newList()->newTab();
        QQuickItem *edit = attach(doc);
        insert(doc, 0, QStringLiteral("a a a"));
        QMetaObject::invokeMethod(edit, "undo"); // the typing itself
        insert(doc, 0, QStringLiteral("a a a"));
        QCOMPARE(doc->replaceAll(QStringLiteral("a"), QStringLiteral("bb"), 0), 3);
        QCOMPARE(doc->text(), QStringLiteral("bb bb bb"));
        QMetaObject::invokeMethod(edit, "undo");
        QCOMPARE(doc->text(), QStringLiteral("a a a"));
        QCOMPARE(doc->replaceAll(QStringLiteral("a"), QStringLiteral("<\\0>"), Document::RegularExpression), 3);
        QCOMPARE(doc->text(), QStringLiteral("<a> <a> <a>"));
        QMetaObject::invokeMethod(edit, "undo");
        QCOMPARE(doc->replaceAll(QStringLiteral("(a) (a)"), QStringLiteral("\\2\\1"), Document::RegularExpression), 1);
        QCOMPARE(doc->text(), QStringLiteral("aa a"));
    }

    void replaceOneFindsNext()
    {
        Document *doc = newList()->newTab();
        attach(doc);
        insert(doc, 0, QStringLiteral("a a"));
        QVariantMap next = doc->replaceOne(QStringLiteral("a"), QStringLiteral("b"), 0, 0, 1);
        QCOMPARE(doc->text(), QStringLiteral("b a"));
        QCOMPARE(next.value("start").toInt(), 2);
        // A stale selection replaces nothing.
        next = doc->replaceOne(QStringLiteral("a"), QStringLiteral("c"), 0, 0, 1);
        QCOMPARE(doc->text(), QStringLiteral("b a"));
        QCOMPARE(next.value("start").toInt(), 2);
    }

    void lineColumnAndPositionOfLine()
    {
        Document *doc = newList()->newTab();
        attach(doc);
        insert(doc, 0, QStringLiteral("ab\n\tc\nxyz"));
        QCOMPARE(doc->lineColumn(0), QPoint(1, 1));
        QCOMPARE(doc->lineColumn(2), QPoint(1, 3));
        QCOMPARE(doc->lineColumn(4), QPoint(2, 2));
        QCOMPARE(doc->lineColumn(9), QPoint(3, 4));
        QCOMPARE(doc->positionOfLine(1), 0);
        QCOMPARE(doc->positionOfLine(2), 3);
        QCOMPARE(doc->positionOfLine(3), 6);
        QCOMPARE(doc->positionOfLine(99), 6);
        QCOMPARE(doc->positionOfLine(0), 0);
    }

    void countsAreDebounced()
    {
        Document *doc = newList()->newTab();
        attach(doc);
        insert(doc, 0, QStringLiteral("one two\nthree"));
        QTRY_COMPARE(doc->wordCount(), 3);
        QCOMPARE(doc->lineCount(), 2);
        QCOMPARE(doc->characterCount(), 12);
    }

    void closeWindowRules()
    {
        m_app->start({});
        DocumentList *first = m_app->windows().first();
        Document *doc = first->current();
        attach(doc);
        insert(doc, 0, QStringLiteral("x"));
        QVERIFY(first->anyModified());
        QVERIFY(m_app->closeWindow(first)); // the last window: the session keeps it
        QCOMPARE(m_app->windows().size(), 1);

        m_app->newWindow();
        QCOMPARE(m_app->windows().size(), 2);
        QVERIFY(!m_app->closeWindow(first)); // another stays, and a tab is modified
        QCOMPARE(m_app->windows().size(), 2);
        QVERIFY(m_app->closeWindow(first, true));
        QCOMPARE(m_app->windows().size(), 1);
        QVERIFY(m_app->closeWindow(m_app->windows().first()));

        // Without a session even the last window asks.
        m_app->settings()->setContinueSession(false);
        DocumentList *only = m_app->windows().first();
        attach(only->current());
        insert(only->current(), 0, QStringLiteral("y"));
        QVERIFY(!m_app->closeWindow(only));
        QVERIFY(m_app->closeWindow(only, true));
    }

    void sessionKeepsOnlyLastWindowsTabs()
    {
        m_app->start({});
        DocumentList *first = m_app->windows().first();
        attach(first->current());
        insert(first->current(), 0, QStringLiteral("first"));
        m_app->newWindow();
        DocumentList *second = m_app->windows().first();
        attach(second->current());
        insert(second->current(), 0, QStringLiteral("second"));
        QVERIFY(m_app->closeWindow(first, true)); // its tabs leave the session
        QVERIFY(m_app->closeWindow(second)); // the last: kept
        m_app->saveSession();
        restart();
        m_app->start({});
        QCOMPARE(m_app->windows().size(), 1);
        QCOMPARE(m_app->windows().first()->current()->text(), QStringLiteral("second"));
    }

    void openFilesFromArguments()
    {
        const QString p = write(QStringLiteral("arg.txt"), "arg\n");
        m_app->settings()->setContinueSession(false);
        m_app->start({p});
        QCOMPARE(m_app->windows().size(), 1);
        DocumentList *list = m_app->windows().first();
        QCOMPARE(list->rowCount(), 1);
        QCOMPARE(list->current()->path(), p);
        // A second launch: relative to its directory, in the same window.
        write(QStringLiteral("rel.txt"), "rel\n");
        m_app->activate({QStringLiteral("atlas-notepad"), QStringLiteral("rel.txt")}, m_dir);
        QCOMPARE(m_app->windows().size(), 1);
        QCOMPARE(list->rowCount(), 2);
        QCOMPARE(list->current()->path(), m_dir + QStringLiteral("/rel.txt"));
        // After "--", a name starting with '-' is a file.
        write(QStringLiteral("-dash.txt"), "dash\n");
        m_app->activate({QStringLiteral("atlas-notepad"), QStringLiteral("--"), QStringLiteral("-dash.txt")}, m_dir);
        QCOMPARE(list->rowCount(), 3);
        QCOMPARE(list->current()->path(), m_dir + QStringLiteral("/-dash.txt"));
        // A relative path needs an absolute working directory.
        m_app->activate({QStringLiteral("atlas-notepad"), QStringLiteral("rel2.txt")}, QString());
        m_app->activate({QStringLiteral("atlas-notepad"), QStringLiteral("rel2.txt")}, QStringLiteral("relative/dir"));
        QCOMPARE(list->rowCount(), 3);
        m_app->settings()->setOpenInNewWindow(true);
        m_app->activate({QStringLiteral("atlas-notepad"), p}, m_dir);
        QCOMPARE(m_app->windows().size(), 2);
    }

    void saveHasMaxWait()
    {
        m_app->setSaveDelays(300, 800);
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        // Typing never pauses for the quiet period, yet the session is written.
        QElapsedTimer clock;
        clock.start();
        int pos = 0;
        while (clock.elapsed() < 2500 && !QFile::exists(sessionDir() + QStringLiteral("/session.json"))) {
            insert(doc, pos++, QStringLiteral("x"));
            QTest::qWait(100);
        }
        QVERIFY(QFile::exists(sessionDir() + QStringLiteral("/session.json")));
        QVERIFY(clock.elapsed() < 2000);
    }

    void missingTextKeepsTabModified()
    {
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        insert(doc, 0, QStringLiteral("precious"));
        m_app->saveSession();
        const QDir texts(sessionDir() + QStringLiteral("/texts"));
        QCOMPARE(texts.entryList(QDir::Files).size(), 1);
        QVERIFY(QFile::remove(texts.filePath(texts.entryList(QDir::Files).first())));
        restart();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("text .* is missing")));
        m_app->start({});
        QVERIFY(m_app->windows().first()->current()->isModified());
        QCOMPARE(m_app->windows().first()->current()->banner(), Document::Unrecovered); // said, not only logged
    }

    void missingTextOfNamedTabShowsFile()
    {
        const QString path = write(QStringLiteral("named.txt"), "on disk\n");
        m_app->start({path});
        Document *doc = m_app->windows().first()->current();
        QTRY_VERIFY(!doc->isLoading());
        attach(doc);
        insert(doc, 0, QStringLiteral("typed "));
        m_app->saveSession();
        const QDir texts(sessionDir() + QStringLiteral("/texts"));
        QCOMPARE(texts.entryList(QDir::Files).size(), 1);
        QVERIFY(QFile::remove(texts.filePath(texts.entryList(QDir::Files).first())));
        restart();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("text .* is missing")));
        m_app->start({});
        Document *back = m_app->windows().first()->current();
        QTRY_VERIFY(!back->isLoading());
        QCOMPARE(back->text(), QStringLiteral("on disk\n"));
        QVERIFY(!back->isModified());
        QCOMPARE(back->banner(), Document::Unrecovered); // kept through the load
    }

    void unusableSessionIsBackedUpAndKeepsTexts()
    {
        QVERIFY(QDir().mkpath(sessionDir() + QStringLiteral("/texts")));
        QFile keep(sessionDir() + QStringLiteral("/texts/keep.txt"));
        QVERIFY(keep.open(QIODevice::WriteOnly));
        keep.write("kept");
        keep.close();
        QFile bad(sessionDir() + QStringLiteral("/session.json"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("{ not json");
        bad.close();
        restart();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't use the session file")));
        m_app->start({});
        QCOMPARE(read(sessionDir() + QStringLiteral("/session.json.bak")), QByteArray("{ not json"));
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        insert(doc, 0, QStringLiteral("new"));
        m_app->saveSession();
        QVERIFY(sessionJson().contains(QStringLiteral("windows")));
        QVERIFY(QFile::exists(sessionDir() + QStringLiteral("/texts/keep.txt"))); // no orphan cleanup this run
    }

    void cleanupKeepsTextsTheBackupNames()
    {
        QVERIFY(QDir().mkpath(sessionDir() + QStringLiteral("/texts")));
        for (const char *name : {"named", "orphan"}) {
            QFile f(sessionDir() + QStringLiteral("/texts/%1.txt").arg(QLatin1String(name)));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        QFile bak(sessionDir() + QStringLiteral("/session.json.bak"));
        QVERIFY(bak.open(QIODevice::WriteOnly));
        bak.write(R"({"version":1,"windows":[{"tabs":[{"textFile":"named"}]}]})");
        bak.close();
        m_app->start({});
        m_app->saveSession();
        QVERIFY(QFile::exists(sessionDir() + QStringLiteral("/texts/named.txt")));
        QVERIFY(!QFile::exists(sessionDir() + QStringLiteral("/texts/orphan.txt")));
    }

    void oldSessionDirectoryIsMoved()
    {
        const QString old = QString::fromUtf8(qgetenv("XDG_DATA_HOME")) + QStringLiteral("/atlas-notepad");
        QVERIFY(QDir().mkpath(old + QStringLiteral("/texts")));
        QFile text(old + QStringLiteral("/texts/t1.txt"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("from before");
        text.close();
        QFile json(old + QStringLiteral("/session.json"));
        QVERIFY(json.open(QIODevice::WriteOnly));
        json.write(R"({"version":1,"windows":[{"tabs":[{"textFile":"t1"}]}]})");
        json.close();
        m_app->start({});
        QCOMPARE(m_app->windows().first()->current()->text(), QStringLiteral("from before"));
        QVERIFY(!QFileInfo::exists(old));
        QVERIFY(QFile::exists(sessionDir() + QStringLiteral("/texts/t1.txt")));
    }

    void textWriteFailureRetries()
    {
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        insert(doc, 0, QStringLiteral("one"));
        m_app->saveSession();
        const QDir texts(sessionDir() + QStringLiteral("/texts"));
        const QString name = texts.entryList(QDir::Files).first();
        const QString file = texts.filePath(name);
        const QByteArray before = read(sessionDir() + QStringLiteral("/session.json"));
        // A directory where the text file goes: the write can't succeed.
        QVERIFY(QFile::remove(file));
        QVERIFY(QDir().mkpath(file));
        insert(doc, 0, QStringLiteral("two"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't write the session's text")));
        m_app->saveSession();
        QCOMPARE(read(sessionDir() + QStringLiteral("/session.json")), before); // not rewritten
        QVERIFY(QDir(file).exists()); // the orphan pass didn't run
        QVERIFY(QDir().rmdir(file));
        m_app->saveSession(); // the key was dropped: written again
        const QStringList now = texts.entryList(QDir::Files);
        QCOMPARE(now.size(), 1);
        QCOMPARE(QString::fromUtf8(read(texts.filePath(now.first()))), QStringLiteral("twoone"));
    }

    void sessionDirIsPrivate()
    {
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        insert(doc, 0, QStringLiteral("x"));
        m_app->saveSession();
        const auto mask = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther
            | QFileDevice::ExeOther;
        QVERIFY(!(QFileInfo(sessionDir()).permissions() & mask));
        QVERIFY(!(QFileInfo(sessionDir() + QStringLiteral("/texts")).permissions() & mask));
        // The files too, whatever the umask: 0600.
        QVERIFY(!(QFileInfo(sessionDir() + QStringLiteral("/session.json")).permissions() & mask));
        const QDir texts(sessionDir() + QStringLiteral("/texts"));
        const QStringList names = texts.entryList(QDir::Files);
        QVERIFY(!names.isEmpty());
        for (const QString &name : names) {
            QVERIFY(!(QFileInfo(texts.filePath(name)).permissions() & mask));
        }
    }

    void linkedSessionDirIsMadePrivate()
    {
        // A session folder linked elsewhere (dotfiles) works, and what it
        // points at is made 0700.
        const QString elsewhere = m_dir + QStringLiteral("/elsewhere");
        QVERIFY(QDir().mkpath(elsewhere));
        QVERIFY(QFile::setPermissions(elsewhere, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadOther | QFileDevice::ExeOther));
        QVERIFY(QDir().mkpath(QFileInfo(sessionDir()).path()));
        QVERIFY(QFile::link(elsewhere, sessionDir()));
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        insert(doc, 0, QStringLiteral("secret"));
        m_app->saveSession();
        QCOMPARE(QDir(elsewhere + QStringLiteral("/texts")).entryList(QDir::Files).size(), 1);
        QVERIFY(!(QFileInfo(elsewhere).permissions() & (QFileDevice::ReadOther | QFileDevice::ExeOther)));
        QVERIFY(QFile::remove(sessionDir()));
    }

    void plantedSessionIsBounded()
    {
        QVERIFY(QDir().mkpath(sessionDir() + QStringLiteral("/texts")));
        QJsonArray tabs;
        for (int i = 0; i < 1500; ++i) { // a planted 20,000+ would be cut
            tabs.append(QJsonObject{{QStringLiteral("textFile"), QStringLiteral("none")}, {QStringLiteral("modified"), true}});
        }
        const QJsonObject window{{QStringLiteral("geometry"), QJsonArray{2147483647, 2147483647, 2147483647, 2147483647}}, {QStringLiteral("tabs"), tabs}};
        const QJsonObject root{{QStringLiteral("version"), 1}, {QStringLiteral("windows"), QJsonArray{window}}};
        write(QStringLiteral("planted.json"), QJsonDocument(root).toJson());
        QVERIFY(QFile::copy(m_dir + QStringLiteral("/planted.json"), sessionDir() + QStringLiteral("/session.json")));
        Session session;
        const QList<WindowState> windows = session.read();
        QCOMPARE(windows.size(), 1);
        QCOMPARE(windows.first().tabs.size(), 1500); // under the cap
        QVERIFY(windows.first().geometry.right() > 0); // no int overflow
        QVERIFY(windows.first().geometry.width() <= (1 << 20));
    }

    void unloadedTabStoresNoText()
    {
        const QString p = write(QStringLiteral("ul.txt"), "disk\n");
        m_app->start({});
        DocumentList *list = m_app->windows().first();
        Document *doc = openFile(list, p);
        QVERIFY(doc);
        attach(doc);
        insert(doc, 0, QStringLiteral("edit "));
        doc->d->loaded = false;
        m_app->saveSession();
        const QJsonObject tab = sessionJson().value(QStringLiteral("windows")).toArray().first().toObject().value(QStringLiteral("tabs")).toArray()[1].toObject();
        QVERIFY(!tab.contains(QStringLiteral("textFile")));
    }

    void staleTempFilesRemoved()
    {
        const QString texts = sessionDir() + QStringLiteral("/texts");
        QVERIFY(QDir().mkpath(texts));
        for (const char *n : {".abc.1f2e.tmp", "plain.txt.keep"}) {
            QFile f(texts + QLatin1Char('/') + QLatin1String(n));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        restart();
        m_app->start({});
        QVERIFY(!QFile::exists(texts + QStringLiteral("/.abc.1f2e.tmp")));
        QVERIFY(QFile::exists(texts + QStringLiteral("/plain.txt.keep")));
    }

    void recentKeepsMissingFiles()
    {
        const QString a = write(QStringLiteral("ra.txt"), "x");
        const QString b = write(QStringLiteral("rb.txt"), "x");
        m_app->addRecentFile(a);
        m_app->addRecentFile(b);
        QVERIFY(QFile::rename(a, a + QStringLiteral(".away"))); // an unmounted drive
        QCOMPARE(m_app->recentFiles(), QStringList{b});
        m_app->addRecentFile(write(QStringLiteral("rc.txt"), "x"));
        QVERIFY(QFile::rename(a + QStringLiteral(".away"), a)); // mounted again
        QCOMPARE(m_app->recentFiles().size(), 3);
        QVERIFY(m_app->recentFiles().contains(a));
    }

    // ------------------------------------------------------------ Reliable

    void closeAsksWhenSessionCantBeWritten()
    {
        m_app->start({});
        DocumentList *list = m_app->windows().first();
        attach(list->current());
        insert(list->current(), 0, QStringLiteral("unsaved"));
        breakSession();
        QSignalSpy messages(m_app, &App::message);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't keep the session private")));
        QVERIFY(!m_app->closeWindow(list)); // the last window, yet it asks
        QVERIFY(!m_app->sessionProblem().isEmpty());
        QCOMPARE(messages.size(), 1);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't keep the session private")));
        m_app->quit(); // asks too: nothing closes
        QCOMPARE(m_app->windows().size(), 1);
        QCOMPARE(messages.size(), 1); // said once while it stays broken
        m_app->cancelQuit();
        QVERIFY(m_app->closeWindow(list, true)); // the dialog decided

        // Unmodified tabs lose nothing: no question.
        m_app->windows().first()->current()->d->setModified(false);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't keep the session private")));
        QVERIFY(m_app->closeWindow(list));

        // Working again: the problem is over, and closing doesn't ask.
        fixSession();
        insert(list->current(), 0, QStringLiteral("more "));
        QSignalSpy problem(m_app, &App::sessionProblemChanged);
        QVERIFY(m_app->closeWindow(list));
        QVERIFY(m_app->sessionProblem().isEmpty());
        QCOMPARE(problem.size(), 1);
        // Broken again after it worked: said again.
        breakSession();
        insert(list->current(), 0, QStringLiteral("x"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't keep the session private")));
        m_app->saveSession();
        QCOMPARE(messages.size(), 2);
        fixSession();
    }

    void backgroundWriteFailureIsReported()
    {
        m_app->setSaveDelays(50, 200);
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        breakSession();
        QSignalSpy messages(m_app, &App::message);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't keep the session private")));
        insert(doc, 0, QStringLiteral("a"));
        QTRY_COMPARE(messages.size(), 1); // from the worker, on the main thread
        QVERIFY(messages.first().first().toString().contains(QStringLiteral("unsaved changes")));
        fixSession();
        insert(doc, 0, QStringLiteral("b"));
        QTRY_VERIFY(m_app->sessionProblem().isEmpty());
        QCOMPARE(messages.size(), 1);
    }

    void quitAfterAskingContinues()
    {
        m_app->start({});
        DocumentList *list = m_app->windows().first();
        attach(list->current());
        insert(list->current(), 0, QStringLiteral("unsaved"));
        breakSession();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't keep the session private")));
        m_app->quit();
        QCOMPARE(m_app->windows().size(), 1);
        // "Don't Save" in its dialog: the tab goes, then the window.
        list->closeDocument(list->current());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't keep the session private")));
        QVERIFY(m_app->closeWindow(list, true));
        // quit() runs again (queued): its session write is the second
        // warning, which ignoreMessage requires.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("can't keep the session private")));
        QTest::qWait(50);
        fixSession();
    }

    void unreadableFileGetsReadBanner()
    {
        // A session tab whose file is now a FIFO: nothing read, Try Again.
        const QString path = m_dir + QStringLiteral("/later.txt");
        QCOMPARE(mkfifo(QFile::encodeName(path).constData(), 0600), 0);
        QVERIFY(QDir().mkpath(sessionDir()));
        QJsonObject tab{{QStringLiteral("path"), path}};
        QJsonObject window{{QStringLiteral("tabs"), QJsonArray{tab}}};
        QFile json(sessionDir() + QStringLiteral("/session.json"));
        QVERIFY(json.open(QIODevice::WriteOnly));
        json.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("windows"), QJsonArray{window}}}).toJson());
        json.close();
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        QTRY_COMPARE(doc->banner(), Document::ReadFailed);
        QVERIFY(!doc->isModified());
        // A regular file in its place: read without being asked.
        QVERIFY(QFile::remove(path));
        write(QStringLiteral("later.txt"), "readable\n");
        QTRY_COMPARE_WITH_TIMEOUT(doc->text(), QStringLiteral("readable\n"), 10000);
        QCOMPARE(doc->banner(), Document::NoBanner);
        QVERIFY(!doc->isModified());
    }

    void readFailureOfOpenFileKeepsText()
    {
        const QString path = write(QStringLiteral("r.txt"), "kept\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(QFile::remove(path));
        QCOMPARE(mkfifo(QFile::encodeName(path).constData(), 0600), 0);
        doc->d->checkOnDisk();
        QTRY_COMPARE(doc->banner(), Document::ReadFailed);
        QCOMPARE(doc->text(), QStringLiteral("kept\n"));
        QVERIFY(QFile::remove(path));
        write(QStringLiteral("r.txt"), "new\n");
        doc->d->checkOnDisk();
        QTRY_COMPARE(doc->text(), QStringLiteral("new\n"));
        QCOMPARE(doc->banner(), Document::NoBanner);

        // Unreadable, then gone: Deleted shows, not the stale read error.
        QVERIFY(QFile::remove(path));
        QCOMPARE(mkfifo(QFile::encodeName(path).constData(), 0600), 0);
        doc->d->checkOnDisk();
        QTRY_COMPARE(doc->banner(), Document::ReadFailed);
        QVERIFY(QFile::remove(path));
        doc->d->checkOnDisk();
        QTRY_COMPARE(doc->banner(), Document::Deleted);
        // A save ends both.
        QVERIFY(saveAndWait(doc));
        QCOMPARE(doc->banner(), Document::NoBanner);
        QCOMPARE(read(path), QByteArray("new\n"));
    }

    void restoreMarkerLivesWhileRestoring()
    {
        markdownSession();
        m_app->start({});
        QVERIFY(QFile::exists(sessionDir() + QStringLiteral("/restoring")));
        restart(); // a clean exit ends it
        QVERIFY(!QFile::exists(sessionDir() + QStringLiteral("/restoring")));
        m_app->start({});
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(sessionDir() + QStringLiteral("/restoring")), 6000); // so does living long enough
    }

    void restoreAfterCrashIsPlain()
    {
        const QString path = markdownSession();
        const bool storedFormatted = sessionJson()[QStringLiteral("windows")].toArray()[0].toObject()[QStringLiteral("tabs")].toArray()[0].toObject()[QStringLiteral("formatted")].toBool();
        QVERIFY(storedFormatted);
        setRestoring(1); // the last launch died restoring
        QSignalSpy messages(m_app, &App::message);
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        QCOMPARE(doc->path(), path);
        QTRY_VERIFY(!doc->isLoading());
        QVERIFY(!doc->isMarkdown());
        QVERIFY(!doc->isFormatted());
        QVERIFY(!doc->isProse());
        QTRY_COMPARE(messages.size(), 1);
        // The view it had is what the session keeps.
        m_app->saveSession();
        QCOMPARE(sessionJson()[QStringLiteral("windows")].toArray()[0].toObject()[QStringLiteral("tabs")].toArray()[0].toObject()[QStringLiteral("formatted")].toBool(), storedFormatted);
        // The next launch is normal again.
        restart();
        m_app->start({});
        doc = m_app->windows().first()->current();
        QTRY_VERIFY(!doc->isLoading());
        QVERIFY(doc->isMarkdown());
        QVERIFY(doc->isFormatted());
    }

    void emptySessionDoesntCountCrashes()
    {
        // Launches that restored nothing died: no reason to set anything aside.
        QVERIFY(QDir().mkpath(sessionDir()));
        setRestoring(5);
        QSignalSpy messages(m_app, &App::message);
        m_app->start({});
        QVERIFY(!QFile::exists(sessionDir() + QStringLiteral("/restoring")));
        QTest::qWait(20);
        QCOMPARE(messages.size(), 0);
        QVERIFY(!QFile::exists(sessionDir() + QStringLiteral("/session.json.bak")));
    }

    void secondCrashSetsSessionAside()
    {
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        attach(doc);
        insert(doc, 0, QStringLiteral("precious"));
        m_app->saveSession();
        restart();
        setRestoring(2);
        // A text no session names: kept too (no clean-up this run).
        QFile orphan(sessionDir() + QStringLiteral("/texts/orphan.txt"));
        QVERIFY(orphan.open(QIODevice::WriteOnly));
        orphan.close();
        QSignalSpy messages(m_app, &App::message);
        m_app->start({});
        QVERIFY(!QFile::exists(sessionDir() + QStringLiteral("/session.json"))); // moved, not copied
        QCOMPARE(m_app->windows().size(), 1);
        QCOMPARE(m_app->windows().first()->rowCount(), 1);
        QVERIFY(m_app->windows().first()->current()->text().isEmpty());
        QTRY_COMPARE(messages.size(), 1);
        QVERIFY(messages.first().first().toString().contains(QStringLiteral("session.json.bak")));
        QVERIFY(QFile::exists(sessionDir() + QStringLiteral("/session.json.bak")));
        // This run's writes keep the old texts.
        m_app->saveSession();
        const QDir texts(sessionDir() + QStringLiteral("/texts"));
        bool found = false;
        for (const QString &name : texts.entryList(QDir::Files)) {
            found = found || read(texts.filePath(name)) == "precious";
        }
        QVERIFY(found);
        QVERIFY(QFile::exists(texts.filePath(QStringLiteral("orphan.txt"))));
    }

    void secondNotepadDoesntTouchSession()
    {
        // Another process (here another open file) holds the lock.
        QVERIFY(QDir().mkpath(QFileInfo(sessionDir()).path()));
        const QByteArray lockPath = QFile::encodeName(QFileInfo(sessionDir()).path() + QStringLiteral("/session.lock"));
        const int fd = ::open(lockPath.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        QVERIFY(fd >= 0);
        QCOMPARE(::flock(fd, LOCK_EX | LOCK_NB), 0);
        QSignalSpy messages(m_app, &App::message);
        m_app->start({});
        QTRY_COMPARE(messages.size(), 1);
        DocumentList *list = m_app->windows().first();
        attach(list->current());
        insert(list->current(), 0, QStringLiteral("mine"));
        m_app->saveSession();
        QVERIFY(!QFile::exists(sessionDir() + QStringLiteral("/session.json")));
        QVERIFY(!m_app->closeWindow(list)); // nothing keeps it: ask
        ::close(fd);
        // Free again: the next Notepad has the session.
        restart();
        m_app->start({});
        attach(m_app->windows().first()->current());
        insert(m_app->windows().first()->current(), 0, QStringLiteral("x"));
        m_app->saveSession();
        QVERIFY(QFile::exists(sessionDir() + QStringLiteral("/session.json")));
    }

    void deletedThenRecreatedReloads()
    {
        const QString path = write(QStringLiteral("back.txt"), "one\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(QFile::remove(path));
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::Deleted, 10000);
        QTest::qWait(300); // long after the file's watch went with it
        write(QStringLiteral("back.txt"), "two two\n");
        // Seen through the folder: clean before, so it reloads quietly.
        QTRY_COMPARE_WITH_TIMEOUT(doc->text(), QStringLiteral("two two\n"), 10000);
        QCOMPARE(doc->banner(), Document::NoBanner);
        QVERIFY(!doc->isModified());
        // Watched as a file again: a later change is seen too.
        write(QStringLiteral("back.txt"), "three three three\n");
        QTRY_COMPARE_WITH_TIMEOUT(doc->text(), QStringLiteral("three three three\n"), 10000);
    }

    void movedAwayIsDeleted()
    {
        const QString path = write(QStringLiteral("mv.txt"), "one\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(QFile::rename(path, m_dir + QStringLiteral("/moved.txt")));
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::Deleted, 10000);
        QVERIFY(doc->isModified()); // the session keeps the text
        QCOMPARE(doc->text(), QStringLiteral("one\n"));
        // Typed while it was gone, then the same file comes back: no
        // conflict, the typing stays unsaved.
        insert(doc, 0, QStringLiteral("mine "));
        QVERIFY(QFile::rename(m_dir + QStringLiteral("/moved.txt"), path));
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::NoBanner, 10000);
        QCOMPARE(doc->text(), QStringLiteral("mine one\n"));
        QVERIFY(doc->isModified());
        // Another file in its place is a conflict.
        QVERIFY(QFile::remove(path));
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::Deleted, 10000);
        write(QStringLiteral("mv.txt"), "theirs\n");
        QTRY_COMPARE_WITH_TIMEOUT(doc->banner(), Document::ChangedOnDisk, 10000);
        QCOMPARE(doc->text(), QStringLiteral("mine one\n"));
    }

    void closedTabStopsWaiting()
    {
        const QString path = write(QStringLiteral("gone.txt"), "one\n");
        DocumentList *list = newList();
        Document *doc = openFile(list, path);
        QVERIFY(doc);
        QVERIFY(QFile::remove(path));
        doc->d->checkOnDisk();
        QCOMPARE(doc->banner(), Document::Deleted);
        QPointer<Document> guard(doc);
        doc->d->setModified(false);
        list->closeDocument(doc);
        QTRY_VERIFY(guard.isNull());
        // The folder's events reach no deleted tab (ASan/valgrind would say).
        write(QStringLiteral("gone.txt"), "two\n");
        QTest::qWait(300);
    }

    void closedWindowIsDeleted()
    {
        m_app->start({});
        m_app->newWindow();
        DocumentList *closing = m_app->windows().first();
        QPointer<DocumentList> guard(closing);
        QVERIFY(m_app->closeWindow(closing, true));
        QTRY_VERIFY(guard.isNull());
    }

    void quitContinuesToNextWindow()
    {
        m_app->settings()->setContinueSession(false);
        m_app->start({});
        m_app->newWindow();
        DocumentList *second = m_app->windows().first();
        DocumentList *first = m_app->windows().last();
        attach(second->current());
        insert(second->current(), 0, QStringLiteral("unsaved"));
        m_app->quit(); // the modified window refuses
        QCOMPARE(m_app->windows().size(), 2);
        QVERIFY(m_app->closeWindow(second, true)); // "discard" in its dialog
        QTRY_COMPARE(m_app->windows().size(), 0); // the quit went on to the other
        Q_UNUSED(first)
    }

    void cancelQuitStopsContinuing()
    {
        m_app->settings()->setContinueSession(false);
        m_app->start({});
        m_app->newWindow();
        DocumentList *second = m_app->windows().first();
        attach(second->current());
        insert(second->current(), 0, QStringLiteral("unsaved"));
        m_app->quit();
        m_app->cancelQuit();
        QVERIFY(m_app->closeWindow(second, true));
        QTest::qWait(50);
        QCOMPARE(m_app->windows().size(), 1);
    }

    // ---- Big texts reach the TextEdit in pieces, and never go missing.

    static QByteArray bigText(bool longLines)
    {
        QByteArray out;
        const int width = longLines ? 60'000 : 40;
        const qsizetype target = longLines ? 3'000'000 : 4'000'000;
        for (int n = 0; out.size() < target; ++n) {
            out += QByteArray::number(n) + ' ';
            out += QByteArray(width, char('a' + n % 26));
            out += n % 7 == 3 ? "\r\n" : "\n";
            if (n % 5 == 0) {
                out += "\xC3\xA9\xF0\x9F\x98\x80 caf\xC3\xA9\n"; // a pair of surrogates among them
            }
        }
        return out;
    }
    static QString expectedOf(const QByteArray &bytes)
    {
        // What a load gives: line ends as LF.
        QString t = QString::fromUtf8(bytes);
        t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        return t;
    }
    // Opened with no TextEdit (so the text is pending), then attached: the fill starts.
    Document *openBig(const QByteArray &bytes, const QString &name = QStringLiteral("big.txt"))
    {
        Document *doc = openFile(newList(), write(name, bytes));
        return doc;
    }
    static bool filled(Document *doc, int ms = 60000)
    {
        return QTest::qWaitFor([&] { return !doc->isLoading(); }, ms);
    }
    static QString editText(Document *doc)
    {
        return doc->d->qdoc ? doc->d->qdoc->toPlainText() : QString();
    }

    void bigTextArrivesComplete_data()
    {
        QTest::addColumn<bool>("longLines");
        QTest::newRow("many lines") << false;
        QTest::newRow("long lines") << true;
    }

    void bigTextArrivesComplete()
    {
        QFETCH(bool, longLines);
        const QByteArray bytes = bigText(longLines);
        const QString expected = expectedOf(bytes);
        Document *doc = openBig(bytes);
        QVERIFY(doc);
        QCOMPARE(doc->text(), expected);
        QSignalSpy loading(doc, &Document::loadingChanged);
        QSignalSpy modified(doc, &Document::modifiedChanged);
        QSignalSpy edited(doc, &Document::edited);
        attach(doc);
        QVERIFY(doc->isLoading());
        QVERIFY(doc->d->qdoc->characterCount() < expected.size()); // only the first piece is in
        QCOMPARE(doc->text(), expected); // and the text is whole all the same
        QVERIFY(!doc->isModified());
        const quint64 version = doc->d->contentVersion;
        QVERIFY(filled(doc));
        QCOMPARE(loading.size(), 2);
        QCOMPARE(doc->text(), expected);
        QVERIFY(editText(doc) == expected);
        QVERIFY(!doc->isModified());
        QCOMPARE(modified.size(), 0);
        QCOMPARE(edited.size(), 0);
        QCOMPARE(doc->d->contentVersion, version);
        QVERIFY(!doc->d->qdoc->isUndoAvailable());
        QVERIFY(!doc->d->qdoc->isRedoAvailable());
        QVERIFY(doc->d->qdoc->isUndoRedoEnabled());
        // It is a text like any other now: an edit is one undo.
        insert(doc, 0, QStringLiteral("x"));
        QVERIFY(doc->isModified());
        QMetaObject::invokeMethod(doc->textEdit(), "undo");
        QCOMPARE(doc->text(), expected);
    }

    void oneHugeLineArrivesComplete()
    {
        // No line break to cut at: a line is put in whole.
        QByteArray bytes = QByteArray("head\n") + QByteArray(1'500'000, 'x') + "\n" + QByteArray(300'000, 'y') + "\ntail\n";
        Document *doc = openBig(bytes);
        QVERIFY(doc);
        attach(doc);
        QCOMPARE(doc->text(), expectedOf(bytes));
        QVERIFY(filled(doc));
        QVERIFY(editText(doc) == expectedOf(bytes));
        QVERIFY(!doc->isModified());
    }

    void bigTextIsNotWrittenOverTheFileMidFill()
    {
        const QByteArray bytes = bigText(false);
        Document *doc = openBig(bytes);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(doc->isLoading());
        QSignalSpy failed(doc, &Document::saveFailed);
        doc->save();
        QCOMPARE(failed.size(), 1);
        QVERIFY(filled(doc));
        QCOMPARE(read(doc->path()), bytes);
    }

    void bigTextSessionKeepsFullText()
    {
        m_app->start({});
        Document *doc = m_app->windows().first()->current();
        const QByteArray bytes = bigText(false);
        // An untitled tab with a big text: restored, modified, being filled.
        doc->d->pending = QString::fromUtf8(bytes);
        doc->d->hasPending = true;
        doc->d->modified = true;
        attach(doc);
        QVERIFY(doc->isLoading());
        QVERIFY(doc->isModified());
        m_app->saveSession();
        const QDir texts(sessionDir() + QStringLiteral("/texts"));
        const QStringList names = texts.entryList(QDir::Files);
        QCOMPARE(names.size(), 1);
        QCOMPARE(read(texts.filePath(names.first())), bytes);
        QVERIFY(filled(doc));
        QVERIFY(doc->isModified());
        QCOMPARE(doc->text(), expectedOf(bytes));
    }

    void reloadMidFillGivesTheNewText()
    {
        const QByteArray bytes = bigText(false);
        Document *doc = openBig(bytes);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(doc->isLoading());
        QTest::qWait(5);
        QVERIFY(doc->isLoading());
        const QByteArray other = bigText(true);
        write(QStringLiteral("big.txt"), other);
        doc->reload();
        QVERIFY(doc->isLoading());
        QVERIFY(filled(doc));
        QVERIFY(doc->d->qdoc->isUndoRedoEnabled());
        QCOMPARE(doc->text(), expectedOf(other));
        QVERIFY(editText(doc) == expectedOf(other));
        QVERIFY(!doc->isModified());
        // And to a small text.
        write(QStringLiteral("big.txt"), "small\n");
        attach(doc); // (a new TextEdit: fills again)
        QVERIFY(doc->isLoading());
        doc->reload();
        QVERIFY(filled(doc));
        QCOMPARE(doc->text(), QStringLiteral("small\n"));
        QCOMPARE(editText(doc), QStringLiteral("small\n"));
        QVERIFY(!doc->isModified());
    }

    void detachMidFillKeepsTheText()
    {
        const QByteArray bytes = bigText(false);
        const QString expected = expectedOf(bytes);
        Document *doc = openBig(bytes);
        QVERIFY(doc);
        QQuickItem *first = attach(doc);
        QVERIFY(doc->isLoading());
        QTest::qWait(5);
        QVERIFY(doc->isLoading());
        QSignalSpy loading(doc, &Document::loadingChanged);
        doc->setTextEdit(nullptr);
        QVERIFY(!doc->isLoading());
        QCOMPARE(loading.size(), 1);
        QVERIFY(doc->d->hasPending);
        QCOMPARE(doc->text(), expected);
        QVERIFY(!doc->isModified());
        QTest::qWait(30); // no timer fires into the detached text
        QCOMPARE(doc->text(), expected);
        QVERIFY(first->property("length").toInt() < expected.size());
        // Another TextEdit gets all of it.
        attach(doc);
        QVERIFY(doc->isLoading());
        QVERIFY(filled(doc));
        QCOMPARE(doc->text(), expected);
        QVERIFY(editText(doc) == expected);
        QVERIFY(!doc->isModified());
    }

    void otherTextEditMidFillGetsAllOfIt()
    {
        const QByteArray bytes = bigText(false);
        const QString expected = expectedOf(bytes);
        Document *doc = openBig(bytes);
        QVERIFY(doc);
        attach(doc);
        QTest::qWait(5);
        QSignalSpy loading(doc, &Document::loadingChanged);
        attach(doc);
        QVERIFY(doc->isLoading());
        QCOMPARE(loading.size(), 0); // never looked finished in between
        QCOMPARE(doc->text(), expected);
        QVERIFY(filled(doc));
        QVERIFY(editText(doc) == expected);
    }

    // The TextEdit dies mid-fill (no setTextEdit(nullptr)) and another comes
    // before the next piece: it gets all of the text, not the rest of it.
    void editDiesMidFillNextGetsAllOfIt()
    {
        const QByteArray bytes = bigText(false);
        const QString expected = expectedOf(bytes);
        Document *doc = openBig(bytes);
        QVERIFY(doc);
        QQuickItem *first = attach(doc);
        QTest::qWait(5);
        QVERIFY(doc->isLoading());
        std::erase_if(m_edits, [first](const auto &e) { return e.get() == first; });
        attach(doc);
        QVERIFY(filled(doc));
        QCOMPARE(doc->text(), expected);
        QVERIFY(editText(doc) == expected);
        QVERIFY(!doc->isModified());
    }

    // Reloading a modified document with a big text tells that it isn't
    // modified any more (the tab's mark goes).
    void bigReloadOfModifiedTellsModified()
    {
        const QString path = write(QStringLiteral("big.txt"), "small\n");
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        attach(doc);
        QVERIFY(filled(doc));
        insert(doc, 0, QStringLiteral("x"));
        QVERIFY(doc->isModified());
        write(QStringLiteral("big.txt"), bigText(false));
        QSignalSpy modified(doc, &Document::modifiedChanged);
        doc->reload();
        QVERIFY(filled(doc));
        QVERIFY(!doc->isModified());
        QCOMPARE(modified.size(), 1); // once, from the fill
    }

    // Find mid-fill looks only in what the editor has.
    void findMidFillStaysInTheEditor()
    {
        QByteArray bytes = bigText(false);
        bytes.append("needle at the very end\n");
        Document *doc = openBig(bytes);
        QVERIFY(doc);
        attach(doc);
        QTest::qWait(5);
        QVERIFY(doc->isLoading());
        QCOMPARE(doc->find(QStringLiteral("needle"), 0, 0, false).value(QStringLiteral("count")).toInt(), 0);
        QVERIFY(filled(doc));
        QCOMPARE(doc->find(QStringLiteral("needle"), 0, 0, false).value(QStringLiteral("count")).toInt(), 1);
    }

    // A TextEdit as EditorView has it: read-only while the document is
    // read-only or loading, and saving its caret to the document.
    QQuickItem *attachAsView(Document *doc)
    {
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nTextEdit {\n"
                          "    property QtObject doc\n"
                          "    textFormat: TextEdit.PlainText\n"
                          "    readOnly: doc !== null && (doc.readOnly || doc.loading)\n"
                          "    onCursorPositionChanged: if (doc) doc.cursorPosition = cursorPosition\n"
                          "}",
                          QUrl());
        auto *edit = qobject_cast<QQuickItem *>(component.createWithInitialProperties({{QStringLiteral("doc"), QVariant::fromValue(doc)}}));
        m_edits.emplace_back(edit);
        doc->setTextEdit(edit);
        return edit;
    }

    // QQuickTextEdit::setReadOnly moves the caret to the end: a load, which
    // makes the editor read-only and then not, must not leave it there.
    void openStartsAtTheTop_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("small") << QByteArray("one\ntwo\nthree\n").repeated(100);
        QTest::newRow("filled") << bigText(false);
        QTest::newRow("read-only") << QByteArray("bin\0ary\nmore\n", 13).repeated(100);
    }

    void openStartsAtTheTop()
    {
        QFETCH(QByteArray, bytes);
        DocumentList *list = newList();
        list->open({QUrl::fromLocalFile(write(QStringLiteral("top.txt"), bytes))});
        Document *doc = list->current();
        QVERIFY(doc);
        QQuickItem *edit = attachAsView(doc); // before the text is read, as the window does
        QVERIFY(doc->isLoading());
        QVERIFY(edit->property("readOnly").toBool());
        QVERIFY(filled(doc));
        QVERIFY(doc->text().size() > 100);
        QCOMPARE(edit->property("cursorPosition").toInt(), 0);
        QCOMPARE(doc->cursorPosition(), 0);
    }

    // And a reload keeps the caret where it was.
    void reloadKeepsTheCaret()
    {
        const QString path = write(QStringLiteral("caret.txt"), QByteArray("one\ntwo\nthree\n").repeated(100));
        Document *doc = openFile(newList(), path);
        QVERIFY(doc);
        QQuickItem *edit = attachAsView(doc);
        edit->setProperty("cursorPosition", 9);
        QSignalSpy readOnly(edit, SIGNAL(readOnlyChanged(bool)));
        doc->reload();
        QVERIFY(filled(doc));
        QCOMPARE(readOnly.size(), 2); // on and off again
        QCOMPARE(edit->property("cursorPosition").toInt(), 9);
        QCOMPARE(doc->cursorPosition(), 9);
    }

    // A click during the fill (read-only takes clicks) wins over the
    // session's caret, and keeps the view; a scroll alone keeps the
    // session's caret and the scroll; with neither, the session's caret is
    // put back.
    void moveMidFillWins_data()
    {
        QTest::addColumn<bool>("click");
        QTest::addColumn<bool>("scroll");
        QTest::newRow("neither") << false << false;
        QTest::newRow("click") << true << false;
        QTest::newRow("scroll") << false << true;
        QTest::newRow("both") << true << true;
    }

    void moveMidFillWins()
    {
        QFETCH(bool, click);
        QFETCH(bool, scroll);
        Document *doc = openBig(bigText(false));
        QVERIFY(doc);
        doc->d->cursor = doc->d->anchor = 1000; // as the session had it
        QQuickItem *flick = attachInFlickable(doc);
        QVERIFY(flick);
        QQuickItem *edit = doc->textEdit();
        QVERIFY(doc->isLoading());
        // What a move is told from: the first piece leaves the caret at 0.
        QCOMPARE(edit->property("cursorPosition").toInt(), 0);
        QCOMPARE(doc->d->fillBaseCursor, 0);
        if (click) {
            edit->setProperty("cursorPosition", 5);
        }
        if (scroll) {
            flick->setProperty("contentY", 5000.0);
        }
        QVERIFY(filled(doc));
        const int caret = click ? 5 : 1000;
        QCOMPARE(edit->property("cursorPosition").toInt(), caret);
        QCOMPARE(doc->cursorPosition(), caret);
        QCOMPARE(doc->d->cursor, caret);
        if (scroll) {
            QTest::qWait(50); // past applyView's queued scroll
            QCOMPARE(flick->property("contentY").toReal(), 5000.0);
            QCOMPARE(doc->scrollY(), 5000.0);
        }
    }

    // A TextEdit in a Flickable as EditorView has them: the view saves the
    // caret and scroll on a caret move, and keeps the scroll in bounds.
    QQuickItem *attachInFlickable(Document *doc)
    {
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nFlickable {\n"
                          "    id: flick\n"
                          "    property QtObject doc\n"
                          "    property alias edit: e\n"
                          "    width: 200; height: 100\n"
                          "    contentHeight: e.height\n"
                          "    onContentHeightChanged: contentY = Math.max(0, Math.min(contentY, contentHeight - height))\n"
                          "    TextEdit {\n"
                          "        id: e\n"
                          "        textFormat: TextEdit.PlainText\n"
                          "        readOnly: flick.doc !== null && (flick.doc.readOnly || flick.doc.loading)\n"
                          "        onCursorPositionChanged: if (flick.doc) { flick.doc.cursorPosition = cursorPosition; flick.doc.scrollY = flick.contentY; }\n"
                          "    }\n"
                          "}",
                          QUrl());
        auto *flick = qobject_cast<QQuickItem *>(component.createWithInitialProperties({{QStringLiteral("doc"), QVariant::fromValue(doc)}}));
        if (!flick) {
            return nullptr;
        }
        m_edits.emplace_back(flick);
        doc->setTextEdit(flick->property("edit").value<QQuickItem *>());
        return flick;
    }

    // The saved scroll comes back with a caret that isn't at 0, though
    // moving the caret makes the view save its (top) scroll.
    void scrollComesBackWithTheCaret()
    {
        Document *doc = openFile(newList(), write(QStringLiteral("scroll.txt"), QByteArray("one\ntwo\nthree\n").repeated(100)));
        QVERIFY(doc);
        doc->d->cursor = doc->d->anchor = 500;
        doc->d->scrollY = 300;
        QQuickItem *flick = attachInFlickable(doc);
        QVERIFY(flick);
        QTRY_COMPARE(flick->property("contentY").toReal(), 300.0);
        QCOMPARE(doc->textEdit()->property("cursorPosition").toInt(), 500);
    }

    // As attachInFlickable, but the content height follows the text late (as
    // a delayed binding does) and the text wraps at the view's width.
    // `lagMs`: how late the content height follows the text.
    QQuickItem *attachInLaggingFlickable(Document *doc, int lagMs = 80)
    {
        QQmlComponent component(&m_engine);
        component.setData("import QtQuick\nFlickable {\n"
                          "    id: flick\n"
                          "    property QtObject doc\n"
                          "    property alias edit: e\n"
                          "    width: 200; height: 100\n"
                          "    onContentHeightChanged: contentY = Math.max(0, Math.min(contentY, contentHeight - height))\n"
                          "    property int lag: 80\n"
                          "    Timer { interval: flick.lag; running: flick.doc !== null && !flick.doc.loading; repeat: true; onTriggered: flick.contentHeight = e.height }\n"
                          "    TextEdit {\n"
                          "        id: e\n"
                          "        width: flick.width\n"
                          "        wrapMode: TextEdit.Wrap\n"
                          "        textFormat: TextEdit.PlainText\n"
                          "        readOnly: flick.doc !== null && (flick.doc.readOnly || flick.doc.loading)\n"
                          "    }\n"
                          "}",
                          QUrl());
        auto *flick = qobject_cast<QQuickItem *>(
            component.createWithInitialProperties({{QStringLiteral("doc"), QVariant::fromValue(doc)}, {QStringLiteral("lag"), lagMs}}));
        if (!flick) {
            return nullptr;
        }
        m_edits.emplace_back(flick);
        doc->setTextEdit(flick->property("edit").value<QQuickItem *>());
        return flick;
    }

    // A restored scroll in a wrapped text survives a content height that is
    // not final when the text arrives, small or big (filled in pieces).
    void restoredScrollWaitsForTheLayout_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<qreal>("scroll");
        QTest::newRow("wrapped") << QByteArray("one two three four five six seven eight nine ten\n").repeated(300) << 800.0;
        QTest::newRow("over 64K") << bigText(false) << 200'000.0;
    }

    void restoredScrollWaitsForTheLayout()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(qreal, scroll);
        Document *doc = openFile(newList(), write(QStringLiteral("late.txt"), bytes));
        QVERIFY(doc);
        doc->d->cursor = doc->d->anchor = 0;
        doc->d->scrollY = scroll;
        QQuickItem *flick = attachInLaggingFlickable(doc);
        QVERIFY(flick);
        QVERIFY(filled(doc));
        QTRY_COMPARE_WITH_TIMEOUT(flick->property("contentY").toReal(), scroll, 5000);
    }

    // A restore waiting for a lagging layout, in a window, with the hold
    // started: the window is what the hold watches for the user's input.
    Document *startHeldRestore(QQuickWindow &window, QQuickItem *&flick)
    {
        Document *doc = openFile(newList(), write(QStringLiteral("late.txt"), QByteArray("one two three four five six seven eight nine ten\n").repeated(300)));
        if (!doc) {
            return nullptr;
        }
        doc->d->cursor = doc->d->anchor = 0;
        doc->d->scrollY = 800.0;
        flick = attachInLaggingFlickable(doc, 600); // the hold waits that long at least
        if (!flick) {
            return nullptr;
        }
        flick->setParentItem(window.contentItem()); // before the hold starts
        return doc;
    }

    // While the restore waits for the layout, the user's own scroll wins:
    // the view doesn't jump back to the restored place once it arrives.
    void restoredScrollYieldsToTheUser()
    {
        QQuickWindow window;
        QQuickItem *flick = nullptr;
        Document *doc = startHeldRestore(window, flick);
        QVERIFY(doc);
        auto unparent = qScopeGuard([flick] { flick->setParentItem(nullptr); });
        QVERIFY(filled(doc));
        QTRY_VERIFY(doc->d->heldWatch); // the hold is waiting
        QWheelEvent wheel(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &wheel);
        flick->setProperty("contentY", 5.0); // what the wheel does
        QVERIFY(!doc->d->heldFlick);
        QTest::qWait(1200);
        QVERIFY(flick->property("contentHeight").toReal() > 900);
        QCOMPARE(flick->property("contentY").toReal(), 5.0);
    }

    // The view's own moves while the layout settles (following the caret,
    // clamping to a shorter height) don't end the restore.
    void restoredScrollOutlastsTheViewsOwnMoves()
    {
        QQuickWindow window;
        QQuickItem *flick = nullptr;
        Document *doc = startHeldRestore(window, flick);
        QVERIFY(doc);
        auto unparent = qScopeGuard([flick] { flick->setParentItem(nullptr); });
        QVERIFY(filled(doc));
        QTRY_VERIFY(doc->d->heldWatch);
        flick->setProperty("contentY", 5.0); // no input: the view itself
        QTRY_COMPARE_WITH_TIMEOUT(flick->property("contentY").toReal(), 800.0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!doc->d->heldWatch, 7000); // done once the layout settled
    }

    // A hidden tab's view takes no input: a key in the shown one doesn't
    // end its restore.
    void hiddenViewKeepsItsHeldRestore()
    {
        QQuickWindow window;
        QQuickItem *flick = nullptr;
        Document *doc = startHeldRestore(window, flick);
        QVERIFY(doc);
        auto unparent = qScopeGuard([flick] { flick->setParentItem(nullptr); });
        flick->setVisible(false);
        QVERIFY(filled(doc));
        QTRY_VERIFY(doc->d->heldWatch);
        QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QCoreApplication::sendEvent(&window, &key);
        QVERIFY(doc->d->heldFlick);
        QTRY_COMPARE_WITH_TIMEOUT(flick->property("contentY").toReal(), 800.0, 5000);
    }

    // A modifier alone (the start of a shortcut) isn't the user moving
    // away; a key is, and a press.
    void heldRestoreEndsOnAKeyNotAModifier()
    {
        QQuickWindow window;
        QQuickItem *flick = nullptr;
        Document *doc = startHeldRestore(window, flick);
        QVERIFY(doc);
        auto unparent = qScopeGuard([flick] { flick->setParentItem(nullptr); });
        QVERIFY(filled(doc));
        QTRY_VERIFY(doc->d->heldWatch);
        QKeyEvent ctrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
        QCoreApplication::sendEvent(&window, &ctrl);
        QKeyEvent caps(QEvent::KeyPress, Qt::Key_CapsLock, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &caps);
        // A wheel turn over something beside the view (the tool capsule).
        QWheelEvent beside(QPointF(400, 10), QPointF(400, 10), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &beside);
        QVERIFY(doc->d->heldFlick);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &down);
        QVERIFY(!doc->d->heldFlick);
        QVERIFY(!doc->d->heldWatch);
    }

    // A view that gets its window only after the hold started is watched
    // from then on.
    void heldRestoreWatchesALateWindow()
    {
        Document *doc = openFile(newList(), write(QStringLiteral("late.txt"), QByteArray("one two three four five six seven eight nine ten\n").repeated(300)));
        QVERIFY(doc);
        doc->d->cursor = doc->d->anchor = 0;
        doc->d->scrollY = 800.0;
        QQuickItem *flick = attachInLaggingFlickable(doc, 600);
        QVERIFY(flick);
        QVERIFY(filled(doc));
        QTRY_VERIFY(doc->d->heldFlick);
        QVERIFY(!doc->d->heldWatch);
        QQuickWindow window;
        flick->setParentItem(window.contentItem());
        auto unparent = qScopeGuard([flick] { flick->setParentItem(nullptr); });
        QVERIFY(doc->d->heldWatch);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &press);
        QVERIFY(!doc->d->heldFlick);
    }

    // A new TextEdit (the tab's view rebuilt) ends the old view's restore.
    void newTextEditEndsAHeldRestore()
    {
        QQuickWindow window;
        QQuickItem *flick = nullptr;
        Document *doc = startHeldRestore(window, flick);
        QVERIFY(doc);
        auto unparent = qScopeGuard([flick] { flick->setParentItem(nullptr); });
        QVERIFY(filled(doc));
        QTRY_VERIFY(doc->d->heldWatch);
        doc->setTextEdit(nullptr);
        QVERIFY(!doc->d->heldFlick);
        QVERIFY(!doc->d->heldWatch);
    }

    // A new view state (a tab switch, a reload at the top) ends a restore
    // still waiting: the old place doesn't come back.
    void newViewStateEndsAHeldRestore()
    {
        QQuickWindow window;
        QQuickItem *flick = nullptr;
        Document *doc = startHeldRestore(window, flick);
        QVERIFY(doc);
        auto unparent = qScopeGuard([flick] { flick->setParentItem(nullptr); });
        QVERIFY(filled(doc));
        QTRY_VERIFY(doc->d->heldWatch);
        doc->d->scrollY = 0;
        doc->d->applyView();
        QVERIFY(!doc->d->heldFlick);
        flick->setProperty("contentY", 5.0);
        QTest::qWait(1200);
        QCOMPARE(flick->property("contentY").toReal(), 5.0);
    }

    // A reload of a big text keeps a scroll past the first piece, though the
    // view clamps it to the first piece meanwhile: that isn't the user's.
    void reloadKeepsAScrollPastTheFirstPiece()
    {
        Document *doc = openBig(bigText(false));
        QVERIFY(doc);
        QQuickItem *flick = attachInFlickable(doc);
        QVERIFY(flick);
        QVERIFY(filled(doc));
        QVERIFY(flick->property("contentHeight").toReal() > 300'000);
        flick->setProperty("contentY", 200'000.0); // the wheel: the caret stays at 0
        doc->reload();
        QVERIFY(doc->isLoading());
        QVERIFY(filled(doc));
        QTRY_COMPARE(flick->property("contentY").toReal(), 200'000.0);
        QCOMPARE(doc->scrollY(), 200'000.0);
        QCOMPARE(doc->cursorPosition(), 0);
    }

    void closingMidFillIsClean()
    {
        Document *doc = openBig(bigText(false));
        QVERIFY(doc);
        attach(doc);
        QTest::qWait(5);
        QVERIFY(doc->isLoading());
        newList()->close(newList()->currentIndex());
        m_edits.clear();
        QTest::qWait(50); // a timer that outlived it would crash here
        restart();
    }

    void destroyedMidFillIsClean()
    {
        Document *doc = openBig(bigText(false));
        QVERIFY(doc);
        attach(doc);
        QTest::qWait(5);
        QVERIFY(doc->isLoading());
        restart();
        QTest::qWait(50);
    }
};

int main(int argc, char *argv[])
{
    QTemporaryDir tmp;
    s_tmp = &tmp;
    qputenv("XDG_CONFIG_HOME", (tmp.path() + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_DATA_HOME", (tmp.path() + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + QStringLiteral("/cache")).toUtf8());
    qputenv("XDG_STATE_HOME", (tmp.path() + QStringLiteral("/state")).toUtf8());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationDomain(QStringLiteral("atlas.eterneon.net"));
    QCoreApplication::setApplicationName(QStringLiteral("notepad"));
    AppTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "app_test.moc"
