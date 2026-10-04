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
a size limit.

## S2: large files

`scripts/bench-s2.sh` runs the same bench on 1 MB and 10 MB Markdown and a
5 MB single line (`bench/make-large.py` writes them to `out/s2`). The machine
was busy for these runs (load 4-6 from other work), so the plain-text
baseline doubled against S1's; compare within a row, not with S1.

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

## Building and testing

Everything builds in the `localhost/atlas-notepad-dev:44` container
(`scripts/dev.sh`); see `CLAUDE.md` for the commands.
