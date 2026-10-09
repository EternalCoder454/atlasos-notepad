//! Guards of the QML that shows or opens what other programs wrote
//! (docs/SECURITY.md, "Rich text" and "Opening links"). They read the QML
//! source, so a new `Text`, link or image that skips a rule fails here, in CI,
//! and not in a review three releases later.
//!
//! - Text from outside (file names, paths, URLs, hosts, error text from KIO)
//!   is drawn as plain text: every `Text`, `Label` and `TextEdit` says
//!   `textFormat: Text.PlainText` (the editor's `TextEdit.PlainText`), nothing
//!   asks for rich, styled, Markdown or auto-detected text except the one
//!   `StyledText` on `STYLED` below, and `TelamonLabel` (plain by default) is
//!   never switched away from it.
//! - A link from a document is opened only by `App.openLink`, which checks the
//!   scheme in C++ (`documentLink`, tested in `app_test`). `Qt.openUrlExternally`
//!   is used only on the two literals on `OPENERS`.
//! - QML does not run text as code, load components from a name, fetch from the
//!   network or draw images from a URL.
//! - The checker itself is tested: snippets that must pass and must fail.

use std::fs;
use std::path::PathBuf;

/// Text types: the last segment of the type name (`QQC2.Label` is `Label`).
const TEXT_TYPES: &[&str] = &["Text", "Label", "TextEdit", "TextArea"];

/// Files that may ask for `StyledText`, how often, and why it is safe.
const STYLED: &[(&str, usize, &str)] = &[(
    "Main.qml",
    1,
    "the About dialog's project link: a literal <a href> built around qsTr(\"Project page\"), no \
     data in it",
)];

/// `Qt.openUrlExternally` calls: (file, how many, why the URL is not data).
const OPENERS: &[(&str, usize, &str)] = &[
    (
        "Main.qml",
        1,
        "the About dialog's link activated: the only link in that label is the literal above",
    ),
    (
        "SettingsPage.qml",
        1,
        "the project page button: a string literal",
    ),
];

/// QML types Notepad has no use for, and that would fetch or run something.
const FORBIDDEN: &[&str] = &[
    "createQmlObject",
    "Qt.include",
    "eval(",
    "new Function",
    "XMLHttpRequest",
    "fetch(",
    "WebSocket",
    "WebView",
    "WebEngine",
    "Qt.createComponent",
    "Qt.callLater(eval",
    "Image {",
    "AnimatedImage {",
    "BorderImage {",
    "Qt.openUrl(",
    "QQC2.ToolTip.text",
    "Controls.ToolTip.text",
    "ToolTip.text:",
];

struct Source {
    name: String,
    text: String,
}

fn qml_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("qml")
}

fn sources() -> Vec<Source> {
    let mut out = Vec::new();
    for entry in fs::read_dir(qml_dir()).expect("the qml folder") {
        let path = entry.unwrap().path();
        let ext = path.extension().and_then(|e| e.to_str()).unwrap_or("");
        if ext == "qml" || ext == "js" {
            out.push(Source {
                name: path.file_name().unwrap().to_string_lossy().into_owned(),
                text: fs::read_to_string(&path).unwrap(),
            });
        }
    }
    out.sort_by(|a, b| a.name.cmp(&b.name));
    assert!(out.len() >= 8, "found only {} QML files", out.len());
    out
}

/// `text` with the inside of comments and string literals blanked (same
/// length and line breaks), so braces and names can be found without a string
/// or a comment fooling the search.
fn mask(text: &str) -> String {
    let b = text.as_bytes();
    let mut out = b.to_vec();
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'/' if b.get(i + 1) == Some(&b'/') => {
                while i < b.len() && b[i] != b'\n' {
                    out[i] = b' ';
                    i += 1;
                }
            }
            b'/' if b.get(i + 1) == Some(&b'*') => {
                out[i] = b' ';
                out[i + 1] = b' ';
                i += 2;
                while i < b.len() && !(b[i] == b'*' && b.get(i + 1) == Some(&b'/')) {
                    if b[i] != b'\n' {
                        out[i] = b' ';
                    }
                    i += 1;
                }
                if i < b.len() {
                    out[i] = b' ';
                    out[i + 1] = b' ';
                    i += 2;
                }
            }
            q @ (b'"' | b'\'' | b'`') => {
                i += 1;
                while i < b.len() && b[i] != q {
                    if b[i] == b'\\' && i + 1 < b.len() {
                        out[i] = b' ';
                        i += 1;
                    }
                    if b[i] != b'\n' {
                        out[i] = b' ';
                    }
                    i += 1;
                }
                i += 1;
            }
            _ => i += 1,
        }
    }
    String::from_utf8(out).unwrap()
}

/// The line's type, if it opens a block: `Type {`, `QQC2.Type {` or
/// `property: Type {` (`delegate: QQC2.Label {`).
fn block_type(line: &str) -> Option<&str> {
    let mut t = line.trim_start();
    if let Some(colon) = t.find(':') {
        let (head, rest) = t.split_at(colon);
        let rest = rest[1..].trim_start();
        if head
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '.')
            && rest.chars().next().is_some_and(char::is_uppercase)
            && !head.is_empty()
        {
            t = rest;
        }
    }
    let open = t.find('{')?;
    let name = t[..open].trim_end();
    let valid = !name.is_empty()
        && name
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '.');
    let last = name.rsplit('.').next().unwrap_or(name);
    (valid && last.chars().next().is_some_and(char::is_uppercase)).then_some(name)
}

/// (line, type, text of the block with its braces matched) for every block
/// whose type's last segment is in `types`.
fn blocks<'a>(src: &'a str, masked: &str, types: &[&str]) -> Vec<(usize, String, &'a str)> {
    let mut out = Vec::new();
    let mut offset = 0;
    let mb = masked.as_bytes();
    for (n, line) in masked.split_inclusive('\n').enumerate() {
        if let Some(name) = block_type(line) {
            let last = name.rsplit('.').next().unwrap_or(name);
            if types.contains(&last) {
                let open = offset + line.find('{').unwrap();
                let (mut depth, mut i, mut end) = (0i32, open, mb.len());
                while i < mb.len() {
                    match mb[i] {
                        b'{' => depth += 1,
                        b'}' => {
                            depth -= 1;
                            if depth == 0 {
                                end = i + 1;
                                break;
                            }
                        }
                        _ => {}
                    }
                    i += 1;
                }
                out.push((n + 1, name.to_string(), &src[offset..end.min(src.len())]));
            }
        }
        offset += line.len();
    }
    out
}

/// The `textFormat:` values of a block (those of the blocks inside it count
/// too: a block is checked as a whole, which is stricter).
fn text_formats(block: &str) -> Vec<&str> {
    block
        .lines()
        .filter_map(|l| l.trim().strip_prefix("textFormat:"))
        .map(|v| v.trim().trim_end_matches(';').trim())
        .collect()
}

/// Every violation of the rules in one file, as messages.
fn problems(file: &str, src: &str) -> Vec<String> {
    let masked = mask(src);
    let mut out = Vec::new();
    for (line, name, block) in blocks(src, &masked, TEXT_TYPES) {
        let kept = mask_keep_values(block);
        let formats = text_formats(&kept);
        let styled_ok = STYLED.iter().any(|(f, ..)| *f == file)
            && formats.iter().any(|f| f.ends_with("StyledText"));
        if !styled_ok && !formats.iter().any(|f| f.ends_with("PlainText")) {
            out.push(format!(
                "{file}:{line}: a {name} without `textFormat: Text.PlainText` shows <b>, <a href> and \
                 <img src> of the text it is given as markup"
            ));
        }
    }
    for (line, name, block) in blocks(src, &masked, &["TelamonLabel"]) {
        for f in text_formats(block) {
            if !f.ends_with("PlainText") {
                out.push(format!(
                    "{file}:{line}: {name} is plain unless told otherwise; this one is {f}"
                ));
            }
        }
    }
    for (n, l) in masked.lines().enumerate() {
        for bad in FORBIDDEN {
            if l.contains(bad) {
                out.push(format!(
                    "{file}:{}: {bad}: QML here neither runs text as code, fetches, draws images \
                     nor sets the style's tooltip",
                    n + 1
                ));
            }
        }
    }
    // Rich text asked for: in the source (strings blanked, comments too).
    for (n, l) in masked.lines().enumerate() {
        for bad in ["RichText", "MarkdownText", "AutoText", "Text.Markdown"] {
            if l.contains(bad) {
                out.push(format!(
                    "{file}:{}: {bad} draws text as markup; file names and remote text are plain",
                    n + 1
                ));
            }
        }
    }
    let styled = masked.matches("StyledText").count();
    let allowed = STYLED
        .iter()
        .find(|(f, ..)| *f == file)
        .map_or(0, |(_, n, _)| *n);
    if styled != allowed {
        out.push(format!(
            "{file}: {styled} use(s) of StyledText, {allowed} allowed (STYLED in tests/qml_text.rs)"
        ));
    }
    let openers = masked.matches("Qt.openUrlExternally").count();
    let allowed = OPENERS
        .iter()
        .find(|(f, ..)| *f == file)
        .map_or(0, |(_, n, _)| *n);
    if openers != allowed {
        out.push(format!(
            "{file}: {openers} Qt.openUrlExternally call(s), {allowed} allowed (OPENERS in \
             tests/qml_text.rs): a document's link goes through App.openLink"
        ));
    }
    out
}

/// The block with comments blanked but the values kept (the `textFormat`
/// value is an identifier, not a string, so masking only strings is enough).
fn mask_keep_values(block: &str) -> String {
    block
        .lines()
        .map(|l| l.split("//").next().unwrap_or(""))
        .collect::<Vec<_>>()
        .join("\n")
}

#[test]
fn the_qml_follows_the_rules() {
    let mut all = Vec::new();
    for s in sources() {
        all.extend(problems(&s.name, &s.text));
    }
    assert!(all.is_empty(), "\n{}\n", all.join("\n"));
}

#[test]
fn the_exceptions_are_real() {
    // A listed exception that no longer exists is removed from the list.
    let srcs = sources();
    for (file, n, why) in STYLED {
        let s = srcs.iter().find(|s| s.name == *file).expect(file);
        assert_eq!(
            mask(&s.text).matches("StyledText").count(),
            *n,
            "{file}: {why}"
        );
    }
    for (file, n, why) in OPENERS {
        let s = srcs.iter().find(|s| s.name == *file).expect(file);
        assert_eq!(
            mask(&s.text).matches("Qt.openUrlExternally").count(),
            *n,
            "{file}: {why}"
        );
    }
    // The About link's href is the literal in the source, never built.
    let main = srcs.iter().find(|s| s.name == "Main.qml").unwrap();
    assert!(
        main.text
            .contains("<a href=\\\"https://github.com/EternalCoder454/atlasos-notepad\\\">%1</a>")
    );
    let settings = srcs.iter().find(|s| s.name == "SettingsPage.qml").unwrap();
    assert!(
        settings.text.contains(
            "Qt.openUrlExternally(\"https://github.com/EternalCoder454/atlasos-notepad\")"
        )
    );
}

#[test]
fn the_editor_is_a_plain_text_edit() {
    let srcs = sources();
    let editor = srcs.iter().find(|s| s.name == "EditorView.qml").unwrap();
    let masked = mask(&editor.text);
    let edits = blocks(&editor.text, &masked, &["TextEdit"]);
    assert_eq!(edits.len(), 1, "one TextEdit holds the document");
    assert!(
        text_formats(edits[0].2).contains(&"TextEdit.PlainText"),
        "the editor must be TextEdit.PlainText: the document is never rich text"
    );
}

// ------------------------------------------------------- the checker itself

#[test]
fn checker_passes_good_snippets() {
    let good = [
        "Item {\n    QQC2.Label {\n        text: x\n        textFormat: Text.PlainText\n    }\n}\n",
        "Item {\n    delegate: QQC2.Label {\n        textFormat: Text.PlainText\n        text: modelData\n    }\n}\n",
        "TextEdit {\n    textFormat: TextEdit.PlainText\n}\n",
        "Item {\n    TelamonLabel { text: x }\n}\n",
        "Item {\n    TelamonLabel {\n        textFormat: Text.PlainText\n    }\n}\n",
        // A word in a string or a comment is not a use.
        "Item {\n    // RichText is not wanted\n    property string s: \"RichText Image { eval(\"\n}\n",
    ];
    for (i, snippet) in good.iter().enumerate() {
        assert!(
            problems("Test.qml", snippet).is_empty(),
            "snippet {i}: {:?}",
            problems("Test.qml", snippet)
        );
    }
}

#[test]
fn checker_fails_bad_snippets() {
    let bad: &[(&str, &str)] = &[
        (
            "Item {\n    QQC2.Label {\n        text: x\n    }\n}\n",
            "without",
        ),
        ("Item {\n    Text {\n        text: x\n    }\n}\n", "without"),
        (
            "Item {\n    delegate: QQC2.Label {\n        text: modelData\n    }\n}\n",
            "without",
        ),
        (
            "Item {\n    QQC2.Label {\n        textFormat: Text.StyledText\n    }\n}\n",
            "StyledText",
        ),
        (
            "Item {\n    Text {\n        textFormat: Text.RichText\n    }\n}\n",
            "RichText",
        ),
        (
            "Item {\n    Text {\n        textFormat: Text.MarkdownText\n    }\n}\n",
            "MarkdownText",
        ),
        (
            "Item {\n    TelamonLabel {\n        textFormat: Text.AutoText\n    }\n}\n",
            "AutoText",
        ),
        (
            "Item {\n    TextEdit {\n        text: x\n    }\n}\n",
            "without",
        ),
        (
            "Item {\n    Image {\n        source: url\n    }\n}\n",
            "Image {",
        ),
        (
            "Item {\n    onClicked: Qt.openUrlExternally(link)\n}\n",
            "openUrlExternally",
        ),
        ("Item {\n    onClicked: eval(text)\n}\n", "eval("),
        (
            "Item {\n    Component.onCompleted: Qt.createQmlObject(code, this)\n}\n",
            "createQmlObject",
        ),
        (
            "Item {\n    Component.onCompleted: { const x = new XMLHttpRequest(); }\n}\n",
            "XMLHttpRequest",
        ),
        ("Item {\n    QQC2.ToolTip.text: path\n}\n", "ToolTip.text"),
    ];
    for (i, (snippet, want)) in bad.iter().enumerate() {
        let found = problems("Test.qml", snippet);
        assert!(
            found.iter().any(|p| p.contains(want)),
            "snippet {i} ({want}) was not caught: {found:?}"
        );
    }
}
