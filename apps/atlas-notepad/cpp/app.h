// The app's objects as QML sees them: the App (one per process: settings,
// windows, session, recent files), a DocumentList per window (its tabs) and a
// Document per tab. Files are read and written by the Rust core
// (notepad_core.h) on a worker thread; nothing here blocks on the disk.
#pragma once

#include "sizelimits.h"
#include "notepad_core.h"

#include <QAbstractListModel>
#include <QFont>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QSettings>
#include <QUrl>
#include <QVariantMap>

#include <memory>

class QFileSystemWatcher;
class QQmlApplicationEngine;
class QTextDocument;
class DocumentList;
class Session;

// ~/.config/atlas-notepadrc. Every setter writes through at once.
class Settings : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("App.settings")
    // The font for plain text and the Syntax view; the Formatted view uses
    // the system's general font at this font's size. Default: the system's
    // fixed-width font.
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(bool wordWrap READ wordWrap WRITE setWordWrap NOTIFY wordWrapChanged)
    Q_PROPERTY(bool lineNumbers READ lineNumbers WRITE setLineNumbers NOTIFY lineNumbersChanged)
    Q_PROPERTY(bool statusBar READ statusBar WRITE setStatusBar NOTIFY statusBarChanged)
    Q_PROPERTY(bool formattingToolbar READ formattingToolbar WRITE setFormattingToolbar NOTIFY formattingToolbarChanged)
    // Off: .md files are plain text everywhere, no Formatted view.
    Q_PROPERTY(bool formatting READ formatting WRITE setFormatting NOTIFY formattingChanged)
    Q_PROPERTY(bool openMarkdownFormatted READ openMarkdownFormatted WRITE setOpenMarkdownFormatted NOTIFY openMarkdownFormattedChanged)
    // true: the last session's tabs come back; false: start empty (and
    // closing a window asks about unsaved tabs).
    Q_PROPERTY(bool continueSession READ continueSession WRITE setContinueSession NOTIFY continueSessionChanged)
    // true: files opened from outside go to a new window instead of a tab.
    Q_PROPERTY(bool openInNewWindow READ openInNewWindow WRITE setOpenInNewWindow NOTIFY openInNewWindowChanged)
    // Takes effect at the next start (main.cpp reads it before Qt starts).
    Q_PROPERTY(bool gpuRendering READ gpuRendering WRITE setGpuRendering NOTIFY gpuRenderingChanged)
    // Percent, 50..400, steps of 10. Shared by all windows.
    Q_PROPERTY(int zoom READ zoom WRITE setZoom NOTIFY zoomChanged)

public:
    explicit Settings(QObject *parent = nullptr);
    // For main.cpp before QApplication exists.
    static bool readGpuRendering();
    // The rc file (tests point it elsewhere through XDG_CONFIG_HOME).
    static QString filePath();

    QFont font() const;
    void setFont(const QFont &font);
    bool wordWrap() const;
    void setWordWrap(bool on);
    bool lineNumbers() const;
    void setLineNumbers(bool on);
    bool statusBar() const;
    void setStatusBar(bool on);
    bool formattingToolbar() const;
    void setFormattingToolbar(bool on);
    bool formatting() const;
    void setFormatting(bool on);
    bool openMarkdownFormatted() const;
    void setOpenMarkdownFormatted(bool on);
    bool continueSession() const;
    void setContinueSession(bool on);
    bool openInNewWindow() const;
    void setOpenInNewWindow(bool on);
    bool gpuRendering() const;
    void setGpuRendering(bool on);
    int zoom() const;
    void setZoom(int percent);

    // Window geometry, remembered for the next new window.
    QRect windowGeometry() const;
    bool windowMaximized() const;
    void setWindowGeometry(const QRect &rect, bool maximized);

    // The rc file itself, for the recent files ([Recent], kept by App).
    QSettings &rc();

Q_SIGNALS:
    void fontChanged();
    void wordWrapChanged();
    void lineNumbersChanged();
    void statusBarChanged();
    void formattingToolbarChanged();
    void formattingChanged();
    void openMarkdownFormattedChanged();
    void continueSessionChanged();
    void openInNewWindowChanged();
    void gpuRenderingChanged();
    void zoomChanged();

private:
    QSettings m_rc;
};

// One tab: a file (or an untitled text), how it is stored on disk, and the
// TextEdit showing it once the tab has been shown.
class Document : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by DocumentList")
    // "notes.md", "Untitled", "Untitled 2".
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    // Local path, empty for an untitled tab.
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    Q_PROPERTY(QUrl folder READ folder NOTIFY pathChanged) // for file dialogs
    Q_PROPERTY(QString toolTip READ toolTip NOTIFY pathChanged)
    Q_PROPERTY(bool modified READ isModified NOTIFY modifiedChanged)
    // Reading the file (or the session's copy) has not finished.
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    // A Markdown file (.md, .markdown, .mdown, .mkd), or an untitled tab.
    // False when Settings.formatting is off or the text is over the
    // Formatted view's size limit.
    Q_PROPERTY(bool markdown READ isMarkdown NOTIFY markdownChanged)
    // Formatted view (markers hidden) or Syntax view; only for markdown.
    Q_PROPERTY(bool formatted READ isFormatted WRITE setFormatted NOTIFY formattedChanged)
    Q_PROPERTY(bool readOnly READ isReadOnly NOTIFY readOnlyChanged)
    // Setting either marks the document modified; the next save uses it.
    Q_PROPERTY(Encoding encoding READ encoding WRITE setEncoding NOTIFY encodingChanged)
    Q_PROPERTY(QString encodingName READ encodingName NOTIFY encodingChanged) // "UTF-8", "UTF-8 with BOM", "UTF-16 LE", "UTF-16 BE", "Windows-1252"
    Q_PROPERTY(LineEnding lineEnding READ lineEnding WRITE setLineEnding NOTIFY lineEndingChanged)
    Q_PROPERTY(QString lineEndingName READ lineEndingName NOTIFY lineEndingChanged) // "Unix (LF)", "Windows (CRLF)", "Macintosh (CR)"
    Q_PROPERTY(Banner banner READ banner NOTIFY bannerChanged)
    Q_PROPERTY(QString bannerText READ bannerText NOTIFY bannerChanged)
    // The TextEdit for this tab; set by QML when the tab is first shown. The
    // text read so far goes into it then. Clearing it keeps the text.
    Q_PROPERTY(QQuickItem *textEdit READ textEdit WRITE setTextEdit NOTIFY textEditChanged)
    // Where the caret and the view were, kept for the session; QML writes
    // them, and reads them back once after setting textEdit.
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY viewStateChanged)
    Q_PROPERTY(int selectionAnchor READ selectionAnchor WRITE setSelectionAnchor NOTIFY viewStateChanged)
    Q_PROPERTY(qreal scrollY READ scrollY WRITE setScrollY NOTIFY viewStateChanged)
    // Characters and words of the whole text, kept up to date cheaply
    // (counted in the background, a moment after typing stops).
    Q_PROPERTY(int characterCount READ characterCount NOTIFY countsChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY countsChanged)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY countsChanged)

public:
    enum Encoding { Utf8 = NP_UTF8, Utf8Bom = NP_UTF8_BOM, Utf16Le = NP_UTF16_LE, Utf16Be = NP_UTF16_BE, Windows1252 = NP_WINDOWS_1252 };
    Q_ENUM(Encoding)
    enum LineEnding { Lf = NP_LF, CrLf = NP_CRLF, Cr = NP_CR };
    Q_ENUM(LineEnding)
    // At most one banner shows; the first in this order wins.
    enum Banner {
        NoBanner,
        SaveFailed,      // bannerText says why; actions: Save As, Retry
        Unencodable,     // the text has characters `encoding` can't store: Save as UTF-8
        ChangedOnDisk,   // modified here and changed outside: Reload, Keep Mine
                         // (save() refuses to overwrite until Keep Mine)
        Deleted,         // the file is gone: Save (recreates it), Close
        TooLarge,        // over the size limit: nothing loaded, Close
        Binary,          // NUL bytes: opened read-only
        LongLines,       // a line over the limit: opened read-only
        Lossy,           // malformed UTF-16 replaced: Save would change it
        MixedLineEndings,// saving makes them all lineEnding
        FormattingOff,   // Markdown over the Formatted view's limit
        ReadOnlyFile,    // no write permission: Save As
    };
    Q_ENUM(Banner)

    explicit Document(DocumentList *list);
    ~Document() override;

    QString title() const;
    QString path() const;
    QUrl folder() const;
    QString toolTip() const;
    bool isModified() const;
    bool isLoading() const;
    bool isMarkdown() const;
    bool isFormatted() const;
    void setFormatted(bool on);
    bool isReadOnly() const;
    Encoding encoding() const;
    void setEncoding(Encoding encoding);
    QString encodingName() const;
    LineEnding lineEnding() const;
    void setLineEnding(LineEnding ending); // marks the document modified
    QString lineEndingName() const;
    Banner banner() const;
    QString bannerText() const;
    QQuickItem *textEdit() const;
    void setTextEdit(QQuickItem *edit);
    int cursorPosition() const;
    void setCursorPosition(int position);
    int selectionAnchor() const;
    void setSelectionAnchor(int position);
    qreal scrollY() const;
    void setScrollY(qreal y);
    int characterCount() const;
    int wordCount() const;
    int lineCount() const;

    // The text as it would be saved, lines joined with "\n" (not
    // QTextDocument::toPlainText, which turns no-break spaces into spaces).
    QString text() const;

    // Save to path; untitled: emits saveAsRequested instead. Async: emits
    // saved() or sets the SaveFailed banner (and saveFailed(message)).
    Q_INVOKABLE void save();
    // Save under a new name (the tab takes it). Markdown-ness follows the
    // new name.
    Q_INVOKABLE void saveAs(const QUrl &url);
    // Read the file again, dropping changes here.
    Q_INVOKABLE void reload();
    // Read the file again as another encoding (only when not modified).
    Q_INVOKABLE void reopenWithEncoding(int encoding);
    // Keep this text over the changed file (next save overwrites it).
    Q_INVOKABLE void keepMine();
    Q_INVOKABLE void dismissBanner();
    // 1-based line and column of a position (column in characters, a tab
    // counting as one), for the status bar: {line, column}.
    Q_INVOKABLE QPoint lineColumn(int position) const;
    // The position at the start of 1-based `line`, clamped (Go To Line).
    Q_INVOKABLE int positionOfLine(int line) const;

    // Find: flags are FindFlag values or'ed. From `from`, forward or back,
    // wrapping around. Returns {start, end, index, count} (index 1-based of
    // count matches; count stops at 10000, shown as "10000+"), {count: 0}
    // for no match, or {error: "message"} for a bad regular expression.
    // Empty text: {count: 0}. Runs on the text, not the QTextDocument.
    enum FindFlag { MatchCase = 1, WholeWords = 2, RegularExpression = 4 };
    Q_ENUM(FindFlag)
    Q_INVOKABLE QVariantMap find(const QString &text, int flags, int from, bool backward) const;
    // Replaces every match in one undo step; returns how many. A regular
    // expression's replacement can use \1..\9 (and \0 for the whole match).
    Q_INVOKABLE int replaceAll(const QString &text, const QString &replacement, int flags);
    // Replaces the match at start..end (if it still matches) and returns
    // find() from after it.
    Q_INVOKABLE QVariantMap replaceOne(const QString &text, const QString &replacement, int flags, int start, int end);

    // The name suggested to Save As: the title, with .md if formatting was
    // used in an untitled tab, else .txt.
    Q_INVOKABLE QString suggestedFileName() const;

Q_SIGNALS:
    void titleChanged();
    void pathChanged();
    void modifiedChanged();
    void loadingChanged();
    void markdownChanged();
    void formattedChanged();
    void readOnlyChanged();
    void encodingChanged();
    void lineEndingChanged();
    void bannerChanged();
    void textEditChanged();
    void viewStateChanged();
    void countsChanged();
    void saved();
    void saveFailed(const QString &message);
    void saveAsRequested();
    // The text changed (debounced by the session, not here).
    void edited();

private:
    friend class DocumentList;
    friend class Session;
    friend class App;
    friend class AppTest;
    struct Private;
    std::unique_ptr<Private> d;
};

// The line-number gutter beside a TextEdit: draws the number of each visible
// block at the block's first line (wrapped lines get none), the caret's line
// in currentColor. Width follows the digits of the line count.
class LineNumbers : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem *textEdit READ textEdit WRITE setTextEdit NOTIFY textEditChanged)
    // The Flickable the TextEdit scrolls in: only its visible part is drawn.
    Q_PROPERTY(QQuickItem *flickable READ flickable WRITE setFlickable NOTIFY flickableChanged)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY styleChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY styleChanged)
    Q_PROPERTY(QColor currentColor READ currentColor WRITE setCurrentColor NOTIFY styleChanged)
    Q_PROPERTY(qreal padding READ padding WRITE setPadding NOTIFY styleChanged) // left and right

public:
    explicit LineNumbers(QQuickItem *parent = nullptr);
    ~LineNumbers() override;
    void paint(QPainter *painter) override;
    QQuickItem *textEdit() const;
    void setTextEdit(QQuickItem *edit);
    QQuickItem *flickable() const;
    void setFlickable(QQuickItem *flickable);
    QFont font() const;
    void setFont(const QFont &font);
    QColor color() const;
    void setColor(const QColor &color);
    QColor currentColor() const;
    void setCurrentColor(const QColor &color);
    qreal padding() const;
    void setPadding(qreal padding);

Q_SIGNALS:
    void textEditChanged();
    void flickableChanged();
    void styleChanged();

private Q_SLOTS:
    // Connected to the TextEdit's and Flickable's signals by name.
    void caretMoved();
    void relayout();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// One window's tabs, in order. Roles: title, modified, toolTip, document.
class DocumentList : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Made by App")
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(Document *current READ current NOTIFY currentIndexChanged)
    Q_PROPERTY(bool canReopenClosed READ canReopenClosed NOTIFY closedChanged)
    // Any tab modified and not saved to a file (for the window title dot and
    // closing with "Start a new session").
    Q_PROPERTY(bool anyModified READ anyModified NOTIFY anyModifiedChanged)

public:
    enum Role { TitleRole = Qt::UserRole + 1, ModifiedRole, ToolTipRole, DocumentRole };

    explicit DocumentList(QObject *parent = nullptr);
    ~DocumentList() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int currentIndex() const;
    void setCurrentIndex(int index);
    Document *current() const;
    bool canReopenClosed() const;
    bool anyModified() const;
    Q_INVOKABLE QList<Document *> documents() const;
    // Appends an untitled-or-restored tab without making it current (for the
    // session's restore). Open files with open().
    Document *append();

    // A new untitled tab after the current one, made current.
    Q_INVOKABLE Document *newTab();
    // Opens each file in a tab (an already open file: its tab becomes
    // current). The last one becomes current. Reading is asynchronous.
    Q_INVOKABLE void open(const QList<QUrl> &urls);
    // Closes the tab if it is not modified, else emits
    // closeConfirmationNeeded(document) and does nothing.
    Q_INVOKABLE void requestClose(int index);
    // Closes without asking (after the user chose Don't Save, or saved).
    Q_INVOKABLE void close(int index);
    Q_INVOKABLE void closeDocument(Document *document);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void reopenClosed();
    Q_INVOKABLE int indexOf(Document *document) const;
    // Ctrl+Tab order: next and previous, wrapping.
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();

Q_SIGNALS:
    void countChanged();
    void currentIndexChanged();
    void closedChanged();
    void anyModifiedChanged();
    void closeConfirmationNeeded(Document *document);
    // The last tab was closed: the window should close.
    void empty();
    // A file couldn't be opened at all (not found, too large, no permission).
    void openFailed(const QString &message);

private:
    friend class Session;
    friend class App;
    struct Private;
    std::unique_ptr<Private> d;
};

// The process: settings, the session, recent files and the windows.
class App : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(Settings *settings READ settings CONSTANT)
    Q_PROPERTY(QStringList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    // Plasma's global menu is there (com.canonical.AppMenu.Registrar on the
    // session bus); watched, so it can come and go.
    Q_PROPERTY(bool hasGlobalMenu READ hasGlobalMenu NOTIFY hasGlobalMenuChanged)
    Q_PROPERTY(QString version READ version CONSTANT)

public:
    // main.cpp makes the one App before loading QML. A null engine makes
    // windows without QML (tests). Each window is Main.qml
    // loaded with initial property `documents` (its DocumentList); the App
    // owns the lists and deletes a window's list after the window closes.
    App(QQmlApplicationEngine *engine, QObject *parent = nullptr);
    ~App() override;
    static App *instance();
    static App *create(QQmlEngine *, QJSEngine *); // QML_SINGLETON factory

    Settings *settings() const;
    QStringList recentFiles() const;
    bool hasGlobalMenu() const;
    QString version() const;

    // Starts up: restores the session's windows (if Settings.continueSession)
    // and opens `files` in the active window (or a new one if the session
    // had none). Always leaves at least one window.
    void start(const QStringList &files);
    // A second launch (KDBusService::activateRequested): files go to a tab of
    // the most recent window, or a new window per Settings.openInNewWindow.
    void activate(const QStringList &arguments, const QString &workingDirectory);

    Q_INVOKABLE void newWindow();
    Q_INVOKABLE void addRecentFile(const QString &path);
    Q_INVOKABLE void clearRecentFiles();
    // Prints the document with Qt's print dialog: the Formatted look for
    // Markdown in the Formatted view, the editor font for everything else.
    Q_INVOKABLE void print(Document *document, QQuickWindow *parent);
    // "Fri, Oct 3, 2026 4:12 PM", the locale's short date and time, for F5.
    Q_INVOKABLE QString timeDate() const;
    // The windows' tab lists, most recently used first (also for tests, which
    // run with a null engine: windows then have no QML).
    QList<DocumentList *> windows() const;
    QQuickWindow *activeWindow() const; // the most recent window's, or null
    // false: nothing is read from or written to the session (--bench).
    void setSessionEnabled(bool enabled);

    // The window asks before closing: true = close now. With
    // continueSession the tabs go to the session and it closes; without, it
    // returns false when tabs are modified (QML asks about them, then calls
    // closeWindow again with force).
    Q_INVOKABLE bool closeWindow(DocumentList *documents, bool force = false);
    // Quits the app. With continueSession: saves the session, closes every
    // window (closeWindow says yes while quitting) and quits. Without: closes
    // the windows one at a time and stops at the first that refuses (its QML
    // asks about the unsaved tabs, then calls quit() again).
    Q_INVOKABLE void quit();
    Q_INVOKABLE void copyToClipboard(const QString &text);
    // An Action's shortcut (a key sequence string or a StandardKey) as the
    // menus show it.
    Q_INVOKABLE QString shortcutText(const QVariant &shortcut) const;
    // Saves the session now (also done a second after any edit stops and at
    // quit).
    Q_INVOKABLE void saveSession();

Q_SIGNALS:
    void recentFilesChanged();
    void hasGlobalMenuChanged();

private:
    struct Private;
    std::unique_ptr<Private> d;
};
