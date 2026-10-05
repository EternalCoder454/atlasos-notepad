# Notepad: design

Notepad (`atlas-notepad`, app ID `net.eterneon.atlas.notepad`) is the AtlasOS
text editor: Windows 11 Notepad's layout (tabs, editor, status bar) in the
Atlas look, with a WYSIWYG Markdown view. The plan and the roadmap live in
Atlas Notes (`AtlasOS/Atlas Text Editor/Plan` and `/Roadmap`).

## Layout of the code

| Path | What |
| --- | --- |
| `crates/notepad-core` | Rust, no Qt: the Markdown line reader (`markdown.rs`), and reading, decoding, encoding and saving files (`file.rs`). |
| `apps/atlas-notepad/src/lib.rs` | The C ABI the C++ side calls (`np_md_*`, `np_file_*`). |
| `apps/atlas-notepad/cpp/` | Qt: the editor (highlighter, editing, decorations), and the app's objects in `app.h`: `App`, `Settings`, `DocumentList`, `Document`, `LineNumbers`, plus the session. |
| `apps/atlas-notepad/qml/` | The window: tabs, toolbar, editors, find, banners, status bar, menus, settings. |
| `apps/atlas-notepad/icons/` | Material Symbols from Atlas Notes (Apache-2.0, see `NOTICE`). |

The Rust side is linked as a static library through Corrosion and exposes a
plain C ABI. The QObjects are C++: they mostly wrap Qt (the TextEdit's
document, file watching, the session bus), and the Rust work behind them is a
handful of pure functions, so CXX-Qt would add a build step for little.

The shared Atlas parts come from atlas-framework: the `Atlas.Ui` QML module
(the window and its merged header, tabs, banners, sections, menus, the status
bar, the scroll bars, popovers and dialogs) is the installed `atlas-ui`
package (1.4.0 or later), loaded like Kirigami, and the opt-in crash reports are the
`atlas-framework-system` crate, pinned in the workspace `Cargo.toml`.

Find and replace search the document's text (`QString`/`QRegularExpression`
on a snapshot), not `QTextDocument::find`, which is slower and can't count
matches cheaply. Replace All is one edit block, so one undo.

## The WYSIWYG editor (spike S1)

The text in the editor is always the raw Markdown. A QML `TextEdit` holds it
and nothing converts it to rich text and back, so saving, undo, copy and
search all work on the file as it is on disk.

1. **Reading.** `MarkdownHighlighter` (a `QSyntaxHighlighter`) hands each
   block's UTF-16 to Rust `np_md_line`, which returns runs of flags (hidden,
   strong, emphasis, code, link, heading, quote, list, fence…) and a line
   summary (kind, heading level, quote depth, marker and content offsets, the
   state carried to the next line for code fences). Emphasis follows
   CommonMark's delimiter rules, including the rule of three.
2. **Formatting.** Each flag set maps to a cached `QTextCharFormat`. In the
   Formatted view, syntax characters get a 1 px transparent font with 1 %
   letter spacing, so they take no room and aren't drawn. (0 % reads as
   "unset" in `QTextEngine`, and a 1 px glyph alone still advances half a
   pixel.) List and quote prefixes stay in place but transparent, widened to
   160 %, to make room for what is drawn there.
3. **Drawing.** `MarkdownDecorations`, a `QQuickPaintedItem` behind the text,
   draws bullets, checkboxes, quote bars, rules and code-block backgrounds for
   the visible blocks only. It recomputes the shapes in `updatePolish` and
   repaints only when they change.
4. **Editing.** `MarkdownEditor` filters the `TextEdit`'s keys and mouse:
   - the caret steps over hidden syntax;
   - Backspace and Delete take the visible character, or remove the span's
     markers along with its last character;
   - Backspace at the start of a heading, list item or quote removes the prefix;
   - Enter continues a list or quote, and ends it on an empty item;
   - Tab and Shift+Tab nest list items;
   - a click on a checkbox toggles it.

   Toolbar edits (`toggleInline`, `setHeading`, `toggleBlock`) go through one
   `QTextCursor` edit block each, so a single undo reverts them.

The Syntax view uses the same code with the markers visible and dimmed.

### Rules worth knowing

- `#` and bullets need a space after them; a bare `#` or `-` is a paragraph.
- Indented code blocks aren't supported, so any indent nests a list instead.
- Fence lines (```` ``` ````) are shown small, dim and monospace, not hidden.

### Reading the text back

`TextEdit.text` (`QTextDocument::toPlainText`) turns no-break spaces into
plain spaces. Saving must read the document's raw text and turn its paragraph
separators back into the file's line endings, or a round trip changes the file.

The editing rules are tested in `apps/atlas-notepad/tests/editor_test.cpp`
(a real `TextEdit`, offscreen).

## S1 figures

`atlas-notepad --bench FILE` sends key events to the `TextEdit` and times each
from the event to `QQuickWindow::frameSwapped`. `scripts/bench-s1.sh` runs it
on `bench/sample-50k.md` (52 KB, 599 lines) in Xvfb, software backend, with
`QT_QPA_UPDATE_IDLE_TIME=0` (xcb otherwise waits 5 ms before each frame).
`NP_BENCH_PLAIN=1` runs the same window without Markdown as a baseline.

Quiet machine (i9-14900KF), milliseconds, event to frame:

| | type mean | type p95 | enter | backspace | arrows | scroll |
| --- | --- | --- | --- | --- | --- | --- |
| Markdown, 1x | 1.96 | 2.16 | 2.31 | 2.00 | 0.14 | 2.14 |
| Markdown, 1.5x | 2.26 | 2.54 | | | | 2.82 |
| Plain TextEdit, 1x | 0.92 | 1.12 | | | | |
| Plain TextEdit, 1.5x | 1.13 | 1.73 | | | | |

Load to first frame: 67 ms, of which the first full highlight is 35 ms.

These were taken on an idle desktop and don't reproduce while the desktop
is in use: the plain TextEdit baseline alone then reads 1.1 to 3.6 ms mean
from run to run. Current figures, and how to read them, are under
Performance.

**Where the time goes.** `perf` puts the Rust reader and the highlighter
under 1 %. The extra millisecond over plain text is Qt drawing more glyph
runs per formatted block (`QQuickTextNodeEngine::addTextBlock`,
`optimizeRenderList`) and `QSyntaxHighlighter` laying a block out a second
time when its formats change.

**Decision.** Accepted. Plain text meets the < 1 ms target. The Markdown
target is adjusted to < 2.5 ms p95 at 1x, about one frame at 160 Hz less
than half used. Still to measure: the GPU backend, and a comparison with Kate.

**Open for S2.** The first highlight is synchronous: about 35 ms per 50 KB,
so large files need it done lazily or in chunks, or Formatted turned off past
a size limit. (Done in the Performance phase: see there.)

## S2: large files

`scripts/bench-s2.sh` runs the same bench on 1 MB and 10 MB Markdown and a
5 MB single line (`bench/make-large.py` writes them to `out/s2`). The machine
was busy for these runs (load 4-6 from other work), so the plain-text
baseline doubled against S1's; compare within a row, not with S1. Opening
is no longer like this: see Performance, Opening.

| File | View | Open to first frame | Key, mean | Memory (RSS) |
| --- | --- | --- | --- | --- |
| 1 MB Markdown | Formatted | 494 ms | 4.8 ms | 133 MB |
| 1 MB Markdown | plain | 167 ms | 4.3 ms | 151 MB |
| 10 MB Markdown | Formatted | 4.7 s | 28 ms | 504 MB |
| 10 MB Markdown | plain | 1.6 s | 28 ms | 728 MB |
| 5 MB, one line | Formatted | 2.2 s | 940 ms | 577 MB |

**Fixed on the way.** The first highlight of a 1 MB file took 9 s:
`QSyntaxHighlighter::rehighlight()` reports each block to the layout on its
own, and `QTextDocumentLayout` walks the document from the top for each.
`MarkdownHighlighter::rehighlightAll()` sets the formats, user data and
states straight on the blocks and reports once (9 s to 0.24 s). Typing still
goes through `highlightBlock`, one block at a time.

**What can't be fixed here.** Past a few MB the time goes to Qt's own
`TextEdit`: every edit walks the block list from the top
(`QTextDocumentLayout::layoutFlow`), every frame resets the font cache of
every block (`QQuickTextEdit::invalidateFontCaches`), and a fully laid-out
document keeps about 70 bytes per character. A long line is laid out again
whole on every keystroke. Getting past that means a different text widget,
which is out of scope for a Notepad.

**Limits** (`cpp/sizelimits.h`):
- Formatted view up to 1 MiB. Bigger Markdown opens as plain text with a
  banner ("Large file: formatting is off").
- Lines up to 100,000 characters stay editable (typing about 15 ms at that
  length). A file with a longer line opens read-only with a banner.
- Files up to 10 MiB open. Bigger ones are refused with a message saying so:
  a 10 MB file already costs 0.7 GB of memory and 30 ms a key.

## Spell check

Sonnet with the system's Hunspell dictionaries (`cpp/spellcheck.cpp`), on
for Markdown, text, extensionless and untitled tabs (`Document::prose`), not
for code. One speller and one cache of answers for every view, loaded on the
first word; with the C locale it falls back to the UI languages, then en_US.

- The highlighter only notes each line's misspelled ranges in
  `BlockInfo::misspelled`. Qt Quick's TextEdit draws an underline format in
  the text colour only (no `SpellCheckUnderline`, no `underlineColor`), so
  `SpellUnderlines`, a painted item over the visible lines like
  `MarkdownDecorations`, draws the squiggles.
- Code, fences and links end words; hidden markers inside a word don't split
  it (`**bo**ld` is checked as "bold", with no suggestions, since replacing
  would drop the markers). Skipped: addresses, camelCase, acronyms, words
  with digits or `_`, one letter, emoji, and letters of a script the
  dictionary isn't in (CJK has no spaces between words).
- Plain text gets a `MarkdownHighlighter` with Markdown off. Deleting a
  `QSyntaxHighlighter` clears every block's formats, so `SpellChecker` and
  `MarkdownEditor` tell each other to highlight again (queued) when one goes.
- Typing bench: no change beyond noise against the plain TextEdit baseline.

## Performance

Measured in the dev container (Qt 6.11.2), Xvfb, software backend, pinned
to the fast cores, with the desktop in use (load 2 to 6): the figures move
with what else runs, so compare builds in alternate runs, never against an
old table. On a hybrid CPU an unpinned run lands on an efficiency core now
and then, which takes three or four times as long; `bench-s1.sh` pins
itself (`BENCH_CPUS` overrides).

**Typing** (`scripts/bench-s1.sh`, 50 KB Markdown, event to frame, ms,
range of three runs):

| | type mean | type p95 |
| --- | --- | --- |
| Markdown, 1x | 3.8 to 4.3 | 5.1 to 7.3 |
| Markdown, 1.5x | 3.7 to 4.4 | 4.9 to 8.8 |
| Plain TextEdit, 1x | 1.1 to 3.6 | 2.1 to 4.7 |
| Plain TextEdit, 1.5x | 2.9 to 4.1 | 4.7 to 6.3 |

The target is a keystroke within one frame at 160 Hz (6.25 ms): the mean
is, in every run, at both scales. The p95 crosses it now and then, and so
does Qt's own TextEdit with no Markdown at all: that tail is the machine
and Qt, not Notepad. The previous commit measures the same within noise.

**What Qt costs, and the patch we don't ship.** On every keystroke
`QQuickTextEdit` redraws the whole visible page on the software backend:
`updatePaintNode` sets its root node's matrix every frame, unchanged or
not, and the software renderer treats a matrix change as dirtying every
child; after a highlighter restyles blocks, `q_textChanged` marks every
node dirty. A three-hunk patch to `qquicktextedit.cpp` (set the matrix only
when it changes, move nodes only by a non-zero delta, redo nodes from the
edit on rather than from the top) took Markdown typing from 2.7 ms mean and
6 ms p95 to 1.9 and 3.5. It isn't shipped: it would mean carrying a patched
Qt in AtlasOS for one app. Worth sending upstream.

Other Qt costs that set the limits for big files:
- `QQuickTextEdit::invalidateFontCaches` and `QTextDocumentLayout`'s
  `doLayout` walk the whole document, so a keystroke in a 10 MB file takes
  about 25 ms whatever the app does.
- Setting `text` lays out all of it at once, on the GUI thread.
- `QSyntaxHighlighter::setDocument` on a non-empty document queues a full
  rehighlight that ignores edits until it runs, then marks every block
  dirty one at a time: quadratic on big files. `MarkdownHighlighter` mutes
  that pass and highlights itself (below).

**Opening** (`NP_BENCH_OPEN_ONLY=1 atlas-notepad --bench FILE`: from
process start to the first frame with text, and to the frame with all of
it; median of three; ms):

| File | First text | All text | Longest stall | Before (first text, stall) |
| --- | --- | --- | --- | --- |
| 50 KB Markdown | 224 | 224 | 0 | 229, 0 |
| 1 MB Markdown | 220 | 514 | 39 | 718, window blocked until then |
| 1 MB prose | 185 | 342 | 36 | 349, blocked |
| 10 MB Markdown (opens plain) | 182 | 2,313 | 32 | 1,486, 1,313 |
| 5 MB, one line (read-only) | 1,862 | 1,862 | 0, then 970 | 1,905 |

- The text goes into the TextEdit in pieces (`Document::Private::fillEdit`,
  `fillStep`): the first 64 K characters at once, then pieces sized to take
  about 10 ms each, cut after a line break, from the event loop. The editor
  is read-only until the last piece is in; undo is off meanwhile and the
  document isn't modified by it. `text()`, Save, the session and the
  counts see the whole text from the start, Find only what the editor has.
- A line is never cut: appending to a block lays all of it out again. The
  one-line file is read-only (over `Limits::lineLength`) and costs Qt's
  single layout of the line, then a 970 ms stall when spell check reads it.
- `MarkdownHighlighter::rehighlightAll` highlights big texts in slices
  under an 8 ms budget per frame, from the top; a line edited ahead of the
  slices is highlighted on its own and its state passed on when the slices
  get there.
- The editor is read-only while loading, and `QQuickTextEdit::setReadOnly`
  moves the caret to the end: `Document::Private::emitKeepingView` puts
  the caret, selection and scroll back around those changes. A click or
  scroll during the fill wins over the session's position.

**Launch** (first frame, cold process, warm caches): 213 to 223 ms with an
empty tab, 221 to 226 ms with a small file, 254 to 261 ms with 50 KB
Markdown. About 20 ms of each is loading the libraries Qt Quick pulls in
(`libQt6Quick` alone brings 83), which a QML app can't avoid.

**Second launch** (a running Notepad, `atlas-notepad FILE` again): 21 to
23 ms to bring up a file that's open, 37 to 63 ms (median 56) to open a
new tab, most of it the running window making the tab's view. The target
was 50 ms.

**Idle**: with the caret blink off, 0 to 3 ticks of CPU in 20 s and no
wakeups of Notepad's own; with it on, about 8 wakeups a second from the
caret, as in Kate, stopping when the window loses focus.

**Memory** (`smaps_rollup` after 5 s, the same container image for both):

| | Rss | Pss | Private dirty |
| --- | --- | --- | --- |
| Notepad, empty tab | 117 MB | 108 MB | 36 MB |
| Notepad, 50 KB Markdown | 126 MB | 117 MB | 42 MB |
| Kate 26.08, empty | 80 MB | 75 MB | 25 MB |
| Kate 26.08, 50 KB Markdown | 102 MB | 97 MB | 35 MB |

Notepad starts about 33 MB (Pss) above Kate, most of it the QML engine,
Qt Quick and Kirigami; the file itself costs Notepad 9 MB and Kate 22.

## Security

Threat model: Notepad runs as the user and opens files from anywhere
(downloads, archives, shared folders, other users' files in `/tmp`). Their
contents, names and the folders they sit in may be hostile. Another process
on the session bus can send it files to open.

- **Reading.** Regular files only: a FIFO would hang the read, a link to
  `/dev/zero` never end. `np_file_read` opens with `O_NONBLOCK`, checks
  the open file and reads at most the 10 MiB limit plus one byte, so a file
  that grows mid-read stops there too.
- **Saving.**
  - Symlinks are followed, as in other editors.
  - A file with several hard links, or one whose owner Notepad can't give
    back (another user's file, writable through its group), is written in
    place. Replacing it would hand it to us, setuid bits included. The old
    bytes are kept and written back if the write fails part way.
- **Session.**
  - Unsaved text lives in `$XDG_STATE_HOME/atlas-notepad/session`. Nothing
    is written unless that folder (or what a link there points to, for a
    state folder kept elsewhere) is a folder owned by the user; it is set to
    0700 first.
  - Its files are 0600 and are replaced by rename. A link planted in place
    of a file is replaced, not followed.
  - Reading a corrupt or planted session is bounded: 64 MiB of JSON, 1 GiB
    per text, 1000 windows, 20,000 tabs, clamped geometry. The caps sit well
    above what Notepad writes, so they never drop the user's own tabs. An
    unreadable session is kept as `session.json.bak`.
- **Markdown.** The line reader is linear: hostile 100,000-character lines
  (`[` × 100k, `*_` × 50k, `http://a` + `)` × 100k) take about 1 ms, not
  seconds. A test keeps each of them under a budget, and a differential
  test checks the result against the old, simple implementation.
- **Links.**
  - Only `http(s)` with a host, `mailto` and `www.` open. A mailto loses
    its `attach` parameters, which some mail clients honour.
  - The Formatted view hides a link's target, so hovering shows it and the
    context menu names the host.
- **Printing.** Markdown is printed without HTML and loads no resources:
  `![](/home/you/private.png)` mustn't put a local file into a PDF, and
  `![](/dev/zero)` mustn't hang.
- **Names.** Titles drop bidi controls (a U+202E between `evil` and
  `txt.exe` makes it read `evilexe.txt`) and show control characters as
  U+FFFD. The desktop style's tooltip reads a tag as rich text, so a word
  joiner follows each `<` in a path.
- **Second launches.** `--` ends the options. At most 100 files are taken,
  and a relative path is used only with an absolute working directory.
- **Other.**
  - Atlas Updater is started from `/usr/bin` rather than found on `$PATH`.
  - The C ABI refuses null pointers, and no panic crosses into C++.
  - The locked crates have no known advisories (OSV, October 2026).
  - The RPM's binary is PIE, full RELRO, NX, FORTIFY, stack protector and
    CET shadow stack.

## Building and testing

Everything builds in the `localhost/atlas-notepad-dev:44` container
(`scripts/dev.sh`); see `CLAUDE.md` for the commands. `atlas-ui` is in no
repository: build atlas-framework's RPMs (its `packaging/build-rpm.sh`) and
give their directory as `ATLAS_LOCAL_RPMS` to the first `scripts/dev.sh` run
and to `packaging/build-rpm.sh`.
