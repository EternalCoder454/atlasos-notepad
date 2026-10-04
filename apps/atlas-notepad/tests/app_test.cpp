// Documents, tabs, the session and settings. XDG_* point into a temp dir
// (main below), App runs without QML (a null engine), and the TextEdits are
// real ones made from QML, as in editor_test.
#include "app.h"
#include "document_p.h"
#include "session.h"

#include <QApplication>
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
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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
        // sftp://host/<dir>/remote.txt is not the local <dir>/remote.txt.
        QUrl remote;
        remote.setScheme(QStringLiteral("sftp"));
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
