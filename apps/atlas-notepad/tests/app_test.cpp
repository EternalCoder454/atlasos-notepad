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

    QString dataDir() const
    {
        return QString::fromUtf8(qgetenv("XDG_DATA_HOME"));
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
        return dataDir() + QStringLiteral("/atlas-notepad");
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
        QDir(dataDir()).removeRecursively();
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
        QVERIFY(QFile::exists(dataDir() + QStringLiteral("/atlas-notepad/session.json")));

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
        const QDir texts(dataDir() + QStringLiteral("/atlas-notepad/texts"));
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
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationDomain(QStringLiteral("atlas.eterneon.net"));
    QCoreApplication::setApplicationName(QStringLiteral("notepad"));
    AppTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "app_test.moc"
