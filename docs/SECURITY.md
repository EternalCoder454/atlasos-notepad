# Notepad: security

The threat model of Telamon Notepad: what it protects, who it defends against,
the rule each entry point follows, and the test that keeps the rule true.
`docs/DESIGN.md` says how the editor works; this file says why the edges are
where they are, and what is left. Change it together with the code it
describes: some of the tests below read the QML and fail when a rule here is
skipped.

Report a vulnerability privately to the maintainer through GitHub's "Report a
vulnerability" on the repository (Security tab), not in a public issue.

## What Notepad is, security-wise

- It runs as the signed-in user, in the session, and has **no privilege of its
  own**: no setuid file, no root helper, no polkit action, no service of its
  own. The only thing it does with elevated rights is nothing: it cannot write
  a file the user cannot write.
- It opens files it did not write, from anywhere: downloads, archives, shared
  folders, `/tmp`, network shares, and URLs. Their **contents, names, and the
  folders they sit in may be hostile**.
- Other programs start it, by name (`telamon-notepad FILE`, `xdg-open`, a
  file manager) and over D-Bus, and a running Notepad is sent the files of
  every later start.
- It keeps **the user's unsaved text** on disk, in the session, so that a
  crash, a logout or a power loss does not lose it.
- It has **no secrets of its own**: no passwords, no keys, no accounts, no
  telemetry. Notepad does not encrypt, lock or store notes: a "locked note", a
  notes database, an autosave next to the file, a backup file, a plug-in, a
  scripting language and a dictionary download do not exist, so none of their
  risks do. If one is added, this file gets its section first.

## Who we defend against

| Attacker | What they can reach | Defended? |
|---|---|---|
| **A hostile file** (a download, an archive member, a file on a share, a file another user left in `/tmp`) | Its bytes, its name, the folder around it, its type (FIFO, device, link, huge, sparse, growing), its encoding | Yes: this is the main model (1, 2, 4) |
| **A hostile program in the session**, typically a sandboxed (Flatpak) app that was given Notepad's D-Bus name, or a compromised app | The session bus name and the command line: files and URLs to open, `CommandLine`, `Open`, `Activate` | Yes, as far as the bus allows: what it can send is bounded and checked (6); what a same-user process can do anyway is out of scope |
| **A hostile server or share** (sftp, smb, ftp, http, webdav...) | Everything KIO returns: sizes, times, error text, redirects, bytes | Yes: sizes counted on what arrives, text drawn as plain text, no password kept (7) |
| **Another local user** | Files in shared folders, `/tmp`, links they plant, the permissions of the folders Notepad writes into | Yes: private files are 0600 in 0700 folders, a planted link is replaced or refused, never followed with a write of ours (2, 3) |
| **The supply chain** | crates.io, the pinned `atlas-framework` commit, the build container, CI actions | Partly: locked and pinned builds, `cargo-deny`, `cargo-audit`, hardening checked on the built program (10) |

Out of scope, because it is not Notepad's to stop: code running as the same
user outside any sandbox (it can already read and change every file Notepad
can, and kill it); a malicious or compromised OS image; physical access; bugs
in Qt, KDE Frameworks, KIO workers and the libraries under them; and what the
pinned `atlas-framework` crates and the installed `telamon-ui` do inside their
own boundaries (we review how Notepad calls them, below).

## 1. Opening a file

Entry points: the Open dialog and its recent list, the command line, a second
launch, a drop onto the window, the session's tabs, a KIO URL (7). All of them
end in `DocumentList::open` and then `readFile` (`cpp/document.cpp`), which
calls `np_file_read` (`crates/notepad-core/src/file.rs`, `read`).

- **Regular files only.** A FIFO would hang the read, `/dev/zero` never end, a
  folder is not text. The path is checked with `stat`, then the file is opened
  with `O_NONBLOCK|O_NOCTTY` (a FIFO with no writer does not block the open),
  and `fstat` is asked again **on the open file**, so a file swapped for a FIFO
  or a device after the first look is still refused (`EINVAL`).
  *Tests:* `files_special_files_are_refused_quickly` (FIFO, `/dev/zero`,
  `/dev/null`, `/dev/full`, `/dev/urandom`, a folder, a link to each),
  `read_only_regular_files`, `AppTest::specialFilesRefused`,
  `AppTest::oddPathsOpenNothing`.
- **Size limit, from the open file.** At most 10 MiB (`Limits::fileBytes`).
  The size comes from `fstat`; the read itself stops at the limit plus one byte,
  so a file that **grows while it is read** (or a `/proc` file that reports
  size 0) cannot make the read run on; a sparse file that claims a terabyte is
  refused before anything is allocated. Remote files are counted again as the
  bytes arrive (7). *Tests:* `files_size_limits_hold_for_sparse_and_growing_files`,
  `read_respects_the_limit`, `read_limit`, `AppTest::tooLargeBanner`.
- **Links.** A link to a regular file opens as that file (a user's own links
  are everywhere: dotfiles, `~/Documents`); a loop gives `ELOOP`, a link to a
  device or a FIFO is that device or FIFO and is refused.
  *Test:* `files_symlink_loops_and_dangling_links`.
- **Encoding.** UTF-8, UTF-8 with BOM, UTF-16 with BOM (either order) and
  Windows-1252. Bytes that are not valid UTF-8 are **not** turned into U+FFFD:
  the file is read as Windows-1252, which maps every byte to a character and
  writes the same bytes back, so opening and saving never silently replaces a
  byte. Where a save would change the file, the app **says so first**: mixed
  line endings, U+2029, and malformed UTF-16 (lone surrogates, a cut-off unit)
  show a banner; a file with a NUL byte in its first 64 KiB (UTF-16 apart) opens
  **read-only** ("looks binary"); a line over 100,000 characters opens
  read-only. A character the file's encoding cannot hold is refused at save
  (`Unencodable`), with the position, instead of being replaced.
  *Tests:* `props::decode_then_encode_is_exact_unless_flagged`,
  `props::bytes_that_are_not_utf8_are_not_replaced`,
  `props::encode_never_panics_and_reads_back`, `props::nul_makes_binary`,
  `props::files_bom_tricks`, `property_round_trip`, `windows_1252_is_exact`,
  `AppTest::roundTrip`, `lossyAndMixedBanners`, `binaryReadOnly`,
  `longLinesReadOnly`, `exoticCharactersSurviveASave` (U+2028, U+0085, VT, FF,
  ESC, DEL, a BOM in the middle, bidi controls, ZWJ sequences, NUL past the
  scan: opened, edited and undone by hand, saved: the same bytes).
- **No resource is loaded because a file asks.** The editor's text is the file;
  nothing in it is fetched, run, or turned into links except by the user's
  click (4).
- **Huge input is bounded in time.** The decoder is linear: 10 MB of one
  repeated byte, CR, NUL, a surrogate, an emoji or a UTF-16 BOM decodes in well
  under the 60 s the test allows (about a second in practice).
  *Test:* `files_ten_megabytes_of_everything_decode_in_time`.
- **Names and folders.** A path goes to the system calls as bytes (a NUL
  cannot get into a C string: `EINVAL`); a leading `-`, a newline, a tab, an
  escape, a bidi override or a 255-byte name are ordinary names to the file
  layer. They are shown safely (4). *Test:* `files_special_names_and_contents`.

## 2. Saving

`file::save` (`np_file_save`) writes the user's file; `file::save_private`
(`np_file_save_private`) writes the session.

- **Atomic.** The bytes go to a temp file in the **same folder** as the target,
  `fsync`ed, then one `rename` over it, then the folder is `fsync`ed. The old
  file is never half-replaced; a failed save leaves it as it was and removes
  the temp file (`TempGuard`). *Tests:* `save_fails_cleanly`,
  `props::save_writes_exact_bytes_and_leaves_nothing` (exact bytes, nothing
  else left in the folder, whatever the old file and its mode).
- **The temp file** is created `O_EXCL` (`create_new`), so it **never follows a
  link** planted at its name and never reuses a file: it is mode 0600 until
  the final mode is set, and its name is `.<name>.<16 hex>.tmp`, new on every
  try. In a shared sticky folder like `/tmp` a planted file of that name only
  makes the save pick another name (8 tries).
- **Mode and owner are kept** for an existing file: owner and group (`fchown`
  first, because it clears setuid bits), mode (`fchmod`), and extended
  attributes (ACLs and the like; what the file system or our rights do not
  allow is skipped). A **new** file gets `0666` less the umask.
  *Tests:* `save_keeps_mode`, `save_keeps_xattr`,
  `props::save_writes_exact_bytes_and_leaves_nothing`.
- **Files that cannot be replaced** are written **in place**: one with other
  hard links (they would stop seeing the changes) or one whose owner we cannot
  give back (another user's file, writable through its group; replacing it
  would make it ours, setuid bits included). The old bytes are kept in memory
  and written back if the write fails part way. A file we may not write
  (`access(W_OK)`) is refused, even if the folder would let a replace through.
  *Tests:* `save_hard_link_in_place`, `save_in_place_shrinks_and_grows`,
  `save_refuses_read_only`.
- **Symlinks: written through, the link stays.** If the name is a link, the
  file it **points to** is the one saved, and the temp file is made beside that
  target, never in the link's folder (the rename must stay on one file system).
  A link to nothing creates its target. This is what other editors do and what
  dotfile setups need. It means a user who opens and saves a link someone else
  planted writes where the link points: the tab shows the link's name, not the
  target (see "What is left"). A chain of 39 links works, a loop is `ELOOP`
  (nothing is written). *Tests:* `save_through_symlink`,
  `save_through_relative_symlink`, `save_dangling_symlink_creates_target`,
  `files_symlink_loops_and_dangling_links`.
- **The session is replaced, never followed.** `save_private` is mode 0600
  whatever the umask or the old file's mode, written by temp file and rename,
  so a link planted at the name is **replaced** and its target untouched.
  *Tests:* `save_private_is_0600_and_replaces_links`,
  `props::save_private_is_always_0600`.
- **A file that changed on disk since it was read** is not overwritten
  silently: the stamp (device, inode, size, mtime) is compared first and the
  tab asks. *Tests:* `AppTest::externalChangeWhileModified`,
  `saveWhileTypingStaysModified`.

## 3. The session, recent files and settings

What Notepad keeps, and where:

| What | Where | Mode |
|---|---|---|
| Tabs, window geometry, **the text of unsaved and untitled tabs** | `$XDG_STATE_HOME/telamon-notepad/session/` (`session.json`, `texts/<uuid>.txt`), the crash counter `restoring`, `session.json.bak` | folders 0700, files 0600 |
| The lock that keeps two Notepads from sharing a session | `$XDG_STATE_HOME/telamon-notepad/session.lock` | 0600 |
| Settings (font, switches, window size) and the **recent files** (ten paths or URLs) | `~/.config/telamon-notepadrc` | the umask's (it is in the user's own `~/.config`) |
| KDE's recent documents (file manager, launcher) | `~/.local/share/RecentDocuments/` (`KRecentDocument`), written after the file is opened | KDE's |

Rules:

- **Private.** The session folder is made only when it (or what a link there
  points to, for a state folder kept elsewhere) is **a folder owned by the
  user**, and is `chmod`ed 0700 first; nothing is written otherwise, and the
  user is told once ("can't keep your unsaved changes"). The files are 0600 by
  `save_private`, whatever the umask. The text of a locked-away note does not
  exist, but unsaved text does: it is plain text in a private folder, like the
  file it replaces, and removed when the tab is saved or closed, or when
  "continue session" is turned off. *Tests:* `AppTest::sessionDirIsPrivate`,
  `linkedSessionDirIsMadePrivate`, `closeAsksWhenSessionCantBeWritten`.
- **A hostile or corrupt session is bounded.** 64 MiB of JSON, 1 GiB per text,
  1000 windows, 20,000 tabs, coordinates clamped, encodings and line endings
  checked, a text name must be a bare file name (`readText`: no `/`, no `..` as
  a path, no folder under it), and a FIFO or folder where a text should be is
  not opened (so a start cannot hang). A session that cannot be used is kept as
  `session.json.bak` and its texts stay. The caps sit far above what Notepad
  writes, so they never drop the user's own tabs. *Tests:*
  `AppTest::plantedSessionIsBounded`, `sessionTextNamesCannotEscape`,
  `unusableSessionIsBackedUpAndKeepsTexts`, `cleanupKeepsTextsTheBackupNames`.
- **Interrupted writes** leave `.<name>.<hex>.tmp` in the session folders;
  they are removed at the next start (`staleTempFilesRemoved`). They are
  0600 in a 0700 folder like the rest.
- **A crash loop is detected, not repeated.** A start that dies twice while
  restoring sets the session aside instead of restoring it again; once, it
  opens the tabs as plain text. *Tests:* `restoreAfterCrashIsPlain`,
  `secondCrashSetsSessionAside`.
- **Recent files** are ten entries (`recentLimit`); only ten are read however
  long the file is, local ones are shown if they exist, URLs without being
  looked up (a network is not waited on), and **a URL never carries its
  password** (`Remote::display`: the user name stays, the password is dropped,
  here, in the session and in KDE's list). A planted entry is only text: it
  goes through the same checks as any URL on the way in (7). *Tests:*
  `AppTest::storedRecentListIsBounded`, `recentDocumentsHaveNoPassword`,
  `kioSessionStripsPassword`, `recentKeepsMissingFiles`.
- **Settings** are read with fallbacks and clamps (zoom 50 to 400, a window
  rectangle is moved onto a screen). They hold no secret.
- **The move from Atlas Notepad** (`cpp/legacy.cpp`) is `rename(2)` with
  `RENAME_NOREPLACE`: the old settings file and the old session folder are moved
  as they are (so modes come with them, links are not followed or copied), never
  over something at the new name, and not while an old Notepad that holds the
  old session lock is running. Nothing is copied, so there is no copy of a
  planted link's target. *Tests:* `atlasSessionIsMoved`,
  `atlasSessionIsNotMovedOverANewOne`, `runningAtlasNotepadKeepsItsSession`,
  `atlasSettingsAreMoved`, `atlasSettingsDontReplaceNewOnes`.

## 4. Showing text that came from outside

**Markdown is never rendered to HTML.** The editor holds the raw Markdown in a
plain-text `TextEdit` (`textFormat: TextEdit.PlainText`) and draws it with
character formats from a line reader in Rust (`crates/notepad-core/src/markdown.rs`).
HTML in a file is text. An image in a file is its text: nothing is fetched or
loaded from a local path. **Printing** builds a `QTextDocument` with Markdown's
HTML turned off and `loadResource` overridden to load nothing, so
`![](/home/you/private.png)` does not put a local file into a PDF and
`![](/dev/zero)` does not hang (`cpp/app.cpp`, `PrintDocument`; this needs a
print dialog to run and has no automated test).

- **The Markdown reader is linear and total.** One line at a time, UTF-16
  units, flags per unit. For **any** line and any incoming state (including
  garbage from the host) it does not panic, writes one flag per unit, gives
  runs that are in order, inside the line and non-empty, positions inside the
  line, no unknown flag, and finishes in a bounded time. Hostile 100,000-unit
  lines (`[` × 100k, `[](`, `*_`, `` ` ``, `http://a)`, a lone surrogate...) and
  10 MB in one line are tested against a time budget. *Tests:*
  `props::markdown_line_is_always_in_range`,
  `props::markdown_document_never_loses_the_plot`,
  `props::markdown_link_at_is_in_range`, `props::markdown_line_is_deterministic`,
  `props::markdown_hostile_lines_stay_fast`,
  `props::markdown_ten_megabytes_in_one_line`, `hostile_lines_stay_linear`,
  `matches_the_simple_implementation`.
- **The C ABI** (`np_md_line`, `np_md_link_at`, `np_file_*`) refuses null
  pointers and unknown enum numbers, writes at most the capacity it was given
  (a canary after the buffer is checked), and no panic crosses into C++
  (`catch_unwind`). *Tests:* `props::md_line_respects_the_buffer_it_was_given`,
  `props::md_link_at_is_inside_the_line`, `props::decode_encode_through_the_abi`,
  `props::read_of_any_path_is_an_error_or_a_file`,
  `props::abi_refuses_nulls_and_unknown_numbers`.
- **Links open only after a click, and only these.** Ctrl+click or the context
  menu on a link goes to `App::openLink` (`documentLink`): `http` and `https`
  with a host, `mailto`, and `www.` (read as `https://`). Everything else
  (`file:`, `javascript:`, `data:`, `smb:`, `ftp:`, `sftp:`, relative paths, an
  empty host) is refused. A `mailto:` loses its `attach` parameters (some mail
  clients attach the file they name: `~/.ssh/id_rsa`); the other fields stay
  byte for byte. What reaches the desktop's opener is `QUrl::FullyEncoded`: no
  control character, space or bidi mark survives, and a bad one makes the link
  not open. The Formatted view hides a link's target, so hovering shows it, the
  context menu names the **host as punycode** (a Cyrillic "а" in `аpple.com`
  shows as `xn--...`), and `https://google.com@evil.example` shows
  `evil.example`. *Tests:* `AppTest::linksThatOpen`,
  `linksNeverCarryControlCharacters`.
- **Names are shown safely.** A tab title and a banner drop bidi controls
  (U+202E between `evil` and `txt.exe` makes it read `evilexe.txt`), zero-width
  and other invisible format characters, and show control characters and line
  separators as U+FFFD; long names are elided in the middle; the window title is
  the same string. The desktop style's tooltip reads a tag as rich text, so a
  word joiner follows each `<` in a path. *Tests:* `AppTest::namesShownSafely`.
- **Every `Text`, `Label` and `TextEdit` in the QML says plain text.** File
  names, paths, hosts, KIO's error text, the recent list and the languages are
  all drawn plain; nothing asks for rich, styled, Markdown or auto-detected
  text, except the About dialog's one project link (a literal `<a href>` around
  the translated words "Project page"). `Qt.openUrlExternally` appears twice: on a string literal
  (the settings page's project button), and on the link of that About label,
  whose only link is the literal `href` (a translator could in theory write
  markup into the words around it; translations are shipped with the package). No QML runs text as code, loads a component by name,
  fetches (`XMLHttpRequest`, `fetch`, web views) or draws an `Image`. Telamon.Ui's
  banners, tabs, dialogs, menus and tooltips are plain by the framework's own
  rules (its `qml_text` lint, `docs/SECURITY.md` there). *Tests:*
  `apps/telamon-notepad/tests/qml_text.rs` (`the_qml_follows_the_rules`,
  `the_exceptions_are_real`, `the_editor_is_a_plain_text_edit`, and the
  checker's own `checker_passes_good_snippets` / `checker_fails_bad_snippets`).
- **The syntax highlighter is bounded.** Code is highlighted with KDE's
  KSyntaxHighlighting definitions, whose regular expressions are **quadratic on
  some long lines**: measured over all 462 definitions with 33 hostile line
  shapes, a line of 100,000 characters took **19 s** (`<<` in TSX), **18 s**
  (`%` in Crystal), 7 s (`<` in TypeScript/PHP/Twig, `[` in Fish) on the
  window's own thread, with the unsaved text unsaved until it returned. A line
  over **4,000 characters** (`Limits::highlightedLineLength`) is now left
  unhighlighted: at 10,000 the worst combination measured took 0.2 s, so one
  line at the cap costs about 0.04 s, and the highlighter's time slices read the
  clock after every line over 1,000 characters (a slice stops within one line of
  its 8 ms budget, instead of after up to 16 lines). Lines up to 100,000
  characters are still editable, plain; a document of many lines just under the
  cap is highlighted in slices from the event loop, as any large file is. The
  Markdown reader is not affected (linear, above). *Tests:*
  `EditorTest::codeHighlighterHostileLinesAreBounded` (fails at 31.6 s with no
  cap), `codeHighlighterLinesNearTheCapStayInSlices`,
  `codeHighlighterKeepsLinesUnderTheCap`, `codeHighlighterSkipsHugeLines`.
- **Spell check** asks the system's Hunspell dictionaries through Sonnet, in
  the dictionaries' own search path; nothing is downloaded. A word of 100,000
  letters costs no time (measured: 0 ms, as does a 20,000-letter word asked for
  suggestions), and the cache of answers is cleared at 20,000 entries, so it holds
  at most as many characters as the files read.
- **Find and Replace** run a regular expression the user typed on a snapshot of
  the text. A pattern with catastrophic backtracking can slow the window: it is
  the user's own pattern, not the file's, and not reachable from outside.

## 5. Crash reports

Opt-in, off by default, and handled by the framework (`telamon-framework-system`
crash module, pinned by commit): Notepad installs its panic hook first thing
(`src/crash.rs`) and passes **Qt's fatal messages** (`QtFatalMsg`, before the
abort) to it. Reports are written locally, shown to the user in Telamon
Updater, and sent only when the user sends them.

- **No document text is put in a report by Notepad.** The Rust code indexes
  UTF-16 slices and never formats the text into an error or a `panic!`/`expect`
  message (none exists outside tests; `tests/source_rules.rs` fails on a new one in the
  file layer, the Markdown reader or the C ABI); the FFI functions catch panics
  and return an error. Qt's fatal messages are Qt's own words (assertions, plugin and
  platform failures). From telamon-framework **2.0.8** the framework scrubs
  message and frames (home paths, user names, control and bidi characters),
  caps the payload at 64 KiB and never includes a command line, a host or an
  environment; before that the message went in as it was. Notepad is on
  2.0.9 as of this phase for that reason, among others (see the commit).
- A caught panic still runs the hook, so a report can be made for a panic the
  ABI turned into an `EIO`.

## 6. Launch arguments and the session bus

Entry points: `telamon-notepad [options] [file...]`, and the single-instance
service (`KDBusService::Unique`): a second launch is forwarded to the first.

| Entry | Who can call it | What it can ask for |
|---|---|---|
| The command line | the user's own processes | open files and URLs, `--new-window`, `--bench FILE` (a developer's typing benchmark of its own window; it opens one file, takes no session) |
| `net.eterneon.telamon.notepad` at `/net/eterneon/telamon/notepad`: `org.kde.KDBusService.CommandLine(args, dir, platform)`, `org.freedesktop.Application.Open(uris)` and `Activate` | any process on the session bus (and a sandboxed app only if its sandbox lets it talk to the name) | open files and URLs, a new window, raise the window |
| `/MainApplication`: Qt's `QCoreApplication`/`QGuiApplication`/`QApplication` exported by `KDBusService` | any process on the session bus | `quit()`, `exit()`, `closeAllWindows()`, `setStyleSheet(...)`, some properties. *Accepted*, see below |

Rules:

- **At most 100 files** are taken from one forwarded launch; the rest is
  dropped. An argument that starts with `-` before `--` is an option, never a
  file; `--` ends the options; empty names and URLs that are not valid
  (`StrictMode`) are dropped. A **relative path is used only with an absolute
  working directory**, and is resolved against it. `file:` URLs are local paths
  (and `file://host/x` is not a network path: it is `/host/x`). Any other URL
  must be of a scheme Notepad opens (7) or it makes a message, not a tab.
  *Tests:* `AppTest::launchArgumentsAreBounded`, `openFilesFromArguments`,
  `argumentUrls`.
- **What a forwarded launch can do is what the command line can do**, no more:
  it does not run anything, write anything or change settings; it opens tabs
  (read-only access to files the user can read) and focuses the window. An
  `http(s)` URL sent this way makes KIO fetch it (7), as a typed one does.
- **`/MainApplication` is KDE's standard export** of the application object
  (every KDE app has it). A process on the session bus could close Notepad with
  it: quitting saves the session first (`aboutToQuit`), and `closeAllWindows`
  goes through the windows' own close confirmation. A process that can already
  reach the user's session bus can do the same with `kill`, and Flatpak's bus
  filter is what keeps a sandboxed app from the name at all. Unexporting it would
  also break scripts that quit Notepad cleanly, so it is left as it is.
- **The activation token** (`XDG_ACTIVATION_TOKEN`, `DESKTOP_STARTUP_ID`) of a
  forwarded launch goes only to the window system.
- **The old name** (`net.eterneon.atlas.notepad`): a new Notepad hands its launch
  to a still-running old one with the same call a second launch would use, at
  most the arguments it got.
- **Other programs Notepad starts**: only Telamon Updater, by the **absolute
  path** `/usr/bin/telamon-updater` (never found on `$PATH`; `atlas-updater` in
  this release), with no arguments. "Open With..." and "Open containing folder"
  go through KIO's launcher with the file's URL **without a password**. No shell
  is ever used; no `QProcess` takes text from a file.
- **Environment.** `NP_BENCH_PLAIN` and `NP_BENCH_OPEN_ONLY` only change what
  `--bench` measures. The test hook that sends local files through KIO
  (`NP_KIO_TEST_HOOK`) is compiled into the test program only; the package's
  `%check` fails if the release program has it (10).

## 7. Remote files (KIO)

A non-local URL (`sftp://`, `smb://`, ...) is read and written through KIO
(`cpp/remote.cpp`); local files never are.

- **Schemes.** Only: `sftp`, `ftp`, `ftps`, `smb`, `webdav(s)`, `nfs`, `mtp`,
  `gdrive`, `kdeconnect` (read and write) and `http`, `https`, `zip`, `tar`,
  `ar`, `archive` (**read only**); and the protocol must be installed and able to
  read (or write) files. Everything else (`trash:`, `fish:`, `javascript:`,
  `settings:`, `desktop:`, `recentlyused:`...) is refused with a message,
  whether it came from the command line, D-Bus, a drop, the recent list or the
  session. *Tests:* `AppTest::kioRefusals`, `storedRecentListIsBounded`.
- **Fetching is the feature.** Opening `https://example.com/a.txt` makes KIO
  fetch it with the user's network access: from the command line, a drop, a
  forwarded launch and a stored recent entry alike. That includes
  `http://localhost:PORT/` and addresses on the LAN. It is what the user (or the
  program that started Notepad) asked for; a process that can ask for it can
  also run `curl`. The fetch is bounded (below) and read only, and no Markdown
  image or link in a file ever starts one.
- **Sizes are not believed.** A remote file is `stat`ed first (a size over the
  limit is refused), and then the bytes are **counted as they arrive**: past
  10 MiB the transfer is killed, since "a server can lie about the size".
  *Tests:* `AppTest::kioRoundTrip`, `kioConflictAndFailure`,
  `kioCancelFirstOpenLeavesNoClosedTab`.
- **A save keeps the file's mode.** Over sftp, KIO writes `<name>.part` and
  renames it; the worker gives the new file the old one's permission bits, so a
  private file does not come out world-readable (the server's default). *Test:*
  `AppTest::sftpSaveKeepsTheFilesMode`, which needs a real sshd and runs from
  `scripts/kio-sftp-test.sh` (skipped by `ctest`, and not in CI).
- **No password is kept.** The tab, the session, the recent list, KDE's recent
  documents, "Copy location" and what "Open With" is given show the URL with the
  user name and **without the password**; KIO's own prompts ask for it, with the
  window as parent. Errors never go to a `KMessageBox`: they are banners, drawn
  plain. *Tests:* `recentDocumentsHaveNoPassword`, `kioSessionStripsPassword`.
- **A notice on the bus is a claim, not a command.** Other programs announce
  renames and removals (`org.kde.KDirNotify`); a tab follows one only when the
  same file is really at the new place (same size and time, same server and
  login) and the old place is empty, at most 1000 per burst, and a remote file
  that went to another folder is only offered (a banner), never followed on a
  program's word. *Tests:* `AppTest::dirNotifyMovesAreChecked`,
  `dirNotifyRemoteMoveToOtherFolderIsOffered`, `dirNotifyRenamesClosedTabs`.
- *Residual:* the protocol workers are KDE's (`kio-extras`, `kio-fuse`...); their
  parsing of a hostile server is theirs. Opening a `zip:` or `tar:` member
  decompresses what the worker gives, up to the 10 MiB cut.

## 8. Tests

| What | Where | How to run |
|---|---|---|
| The file reader, writer and encodings; the Markdown line reader (properties and a corpus of nasty inputs) | `crates/notepad-core/src/props.rs` | `PROPTEST_CASES=20000 cargo test --workspace --locked -- props` |
| The C ABI | `apps/telamon-notepad/src/props.rs` | the same command |
| The QML text and link rules, and the checker's own tests | `apps/telamon-notepad/tests/qml_text.rs` | `cargo test --workspace --locked` |
| No panicking macro in the code that sees document text | `apps/telamon-notepad/tests/source_rules.rs` | `cargo test --workspace --locked` |
| Opening, saving, the session, recents, launch arguments, links, remote files | `apps/telamon-notepad/tests/app_test.cpp` (`AppTest`) | `ctest --test-dir build/s1 -j10 --output-on-failure` |
| The highlighters and editing | `apps/telamon-notepad/tests/editor_test.cpp` | the same |
| Hardening of the built program, and the check's own test | `scripts/check-hardening.sh` (the spec's `%check`), `scripts/test-check-hardening.sh` | `scripts/test-check-hardening.sh` |

Every property test is in a module named `props`, so one `-- props` runs them
all; each takes under a minute at 20,000 cases (about 15 s together; the three that
write files in a temp folder stop at 400 cases, hundreds of saves and reads). A failure
prints the smallest input that broke the rule, and proptest keeps it in
`proptest-regressions/` to be run again first.

## 9. Not an issue (checked)

- **Temp-file names in `/tmp`-like folders** are not secret and do not need to
  be: creation is `O_EXCL`, a planted name only costs a retry.
- **`random_name` is not a CSPRNG.** It does not have to be (above).
- **Opening a device node is refused after `fstat`**, but the `open` itself ran
  first (`O_NONBLOCK|O_NOCTTY`). The C++ side checks `stat` first, so only a file
  swapped for a device in the instant between the two is opened once (never read
  or written). Opening a device with side effects needs the user to have rights
  to it.
- **`write_in_place` keeps the old bytes in memory** to restore them when a write
  fails part way. A file with several hard links that has grown huge would be read
  whole; the attacker would need write access to the file, and could then simply
  corrupt it.
- **Dropping or opening many files** at once opens many tabs (only a forwarded
  launch is capped at 100): a drop is the user's own action.
- **The settings file and KDE's recent list** hold the paths of recent files and
  are not 0600; they are in the user's own `~/.config` and `~/.local/share`.
- **A path with bytes that are not UTF-8** cannot be opened (it is decoded to
  U+FFFD on the way in), so it is refused, not mis-opened by name.
- **`--bench FILE`** reads one file and types into its own window; it saves
  nothing.
- **The user's own regular expression in Find** (above).
- **Recursion and hash-flooding in the Markdown reader**: the line reader does
  not recurse (`inline` and `emphasis` are loops over arrays), and its one hash
  set holds positions, with Rust's randomly keyed hasher.
- **`file:x`** (no slash) in a forwarded launch is read against Notepad's own
  working directory, not the sender's. It names nothing the sender could not
  name with a full path, so it needs no extra rule.

## 10. Build hardening and supply chain

- **RPM flags.** The spec builds with Fedora's `%build_cflags`,
  `%build_cxxflags`, `%build_ldflags` and `%build_rustflags` (stack protector
  strong, `_FORTIFY_SOURCE=3`, `_GLIBCXX_ASSERTIONS`, stack clash protection,
  `-fcf-protection`, PIE, full RELRO and `BIND_NOW`, `-Werror=format-security`),
  with the build path removed from the binary. The flags are **confirmed on the
  result**: `scripts/check-hardening.sh`, run by `%check` on the built program,
  reads the ELF file back with `readelf` and fails the package build without
  PIE, `GNU_RELRO` with `BIND_NOW`, a non-executable stack, no RPATH, RUNPATH or
  TEXTREL, stack protectors (`__stack_chk_fail` is used), and the absence of the
  tests' KIO hook (`Remote::setForceKio`). The build also fails if the build
  tree's path is in the binary. CI tests the check itself
  (`scripts/test-check-hardening.sh`) against programs built with and without each
  protection, and `annocheck` agrees on the RPM ("Hardened: telamon-notepad:
  PASS"). *Not met:* Intel CET's IBT marking. The C++ is built with
  `-fcf-protection`, but rustc has no stable switch for it and the linker marks a
  program only if every object in it is marked, so the program carries the
  shadow stack mark (`SHSTK`) and not `IBT`; the check reports it as a note
  (`--require-cet` makes it an error).
- **Locked and pinned.** Builds use `--locked`. The one git dependency,
  `atlas-framework`, is pinned by a 40-character commit in `Cargo.toml`;
  `ci/check-cargo.py` fails CI unless every `telamon-framework` package in
  `Cargo.lock` comes from exactly that commit, and fails on any other git source,
  a `[patch]` or `[replace]`, a `.cargo/config` or a symlink. Moving the pin is
  done together with `telamon-ui >=` in the spec (CLAUDE.md).
- **`cargo-deny`** (`deny.toml`: advisories, licences, bans, sources) and
  **`cargo-audit`** run in CI on every change to the dependencies and every week
  (`.github/workflows/security.yml`), so a new advisory against the lock file is
  seen without a commit. The advisory database is read at the time of the run; the
  run for this phase (1,295 advisories, 54 crates) found none.
- **Dependabot** (`.github/dependabot.yml`) proposes cargo, GitHub Actions and
  base-image updates weekly; the framework crates are excluded (moved by hand).
- **CI** (`.github/workflows/ci.yml`): every action is pinned by commit (with its
  tag in a comment), `permissions: contents: read` by default and `packages:
  write` only in `publish` on `main`, `persist-credentials: false` on every
  checkout, no `pull_request_target`, no `${{ }}` of an event field in a `run:`
  (values go through `env:`), the registry token only in the one login step. Jobs
  pull the CI image **by digest** that `publish` recorded on `main`; the image is
  checked for secrets before it is pushed (`ci/check-image-secrets.sh`).
  `shellcheck` covers every script; the property tests run with 20,000 cases.
- **Releases** are built in the Fedora 44 container; nothing is fetched at build
  time except crates and the one pinned git dependency.

## 11. What is left

- **A link someone else planted is saved through.** A user who opens and saves
  `/tmp/x.txt`, which is a link to a file the user can write, changes that file.
  The tab shows the link's name. Refusing would break dotfile setups; showing the
  target (a banner "this is a link to ...") is a product decision.
- **Same-user processes can use the bus name** (`/MainApplication` and the file
  forwarding). Only a sandbox stops that.
- **Unsaved text is plain on disk** (in the 0700 session folder), like the swap
  file of any editor; encrypting it needs a key the session does not have.
  Full-disk encryption is the answer.
- **KIO and its workers, Qt and KDE's KSyntaxHighlighting** are reviewed at their
  call sites, not inside. The highlighter's regular expressions are bounded by
  the line cap, not fixed.
- **IBT marking** of the program (above).
- **Fuzzing is property testing** (stable Rust, no coverage guidance);
  `cargo-fuzz` targets for `decode` and `parse_line` are a possible next step.
