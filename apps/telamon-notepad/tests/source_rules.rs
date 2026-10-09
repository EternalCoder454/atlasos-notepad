//! Rules about the Rust source itself (docs/SECURITY.md, "Crash reports").
//!
//! A panic message goes into a crash report (the framework scrubs it, but the
//! best text to scrub is none). The code that sees a document's text -- the
//! file layer, the Markdown reader, the C ABI -- therefore has no `panic!`,
//! `unwrap()`, `expect()`, `assert!`, `unreachable!` or `todo!` outside its
//! tests: a failure there is an error code or a default, and an out-of-range
//! index panics with numbers, never with the text. A new one must be argued
//! here, in the list, with the reason it cannot print the text.

use std::fs;
use std::path::PathBuf;

/// Source files that handle document text, relative to the repository.
const FILES: &[&str] = &[
    "crates/notepad-core/src/file.rs",
    "crates/notepad-core/src/markdown.rs",
    "crates/notepad-core/src/lib.rs",
    "apps/telamon-notepad/src/lib.rs",
    "apps/telamon-notepad/src/crash.rs",
];

/// (file, token, reason) allowed. None today.
const ALLOWED: &[(&str, &str, &str)] = &[];

const TOKENS: &[&str] = &[
    "panic!",
    ".unwrap()",
    ".expect(",
    "unreachable!",
    "todo!",
    "unimplemented!",
    "assert!",
    "assert_eq!",
    "assert_ne!",
    "debug_assert",
];

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

/// `src` with comments (line, and block with nesting) and the contents of
/// string, raw string and character literals blanked to spaces, line breaks
/// kept, so tokens and braces can be searched without a comment or a string
/// fooling the search.
fn blank(src: &str) -> String {
    let b: Vec<char> = src.chars().collect();
    let mut out = b.clone();
    let blank_at = |out: &mut Vec<char>, i: usize| {
        if out[i] != '\n' {
            out[i] = ' ';
        }
    };
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        let next = b.get(i + 1).copied();
        if c == '/' && next == Some('/') {
            while i < b.len() && b[i] != '\n' {
                blank_at(&mut out, i);
                i += 1;
            }
        } else if c == '/' && next == Some('*') {
            let mut depth = 0;
            while i < b.len() {
                if b[i] == '/' && b.get(i + 1) == Some(&'*') {
                    depth += 1;
                    blank_at(&mut out, i);
                    blank_at(&mut out, i + 1);
                    i += 2;
                } else if b[i] == '*' && b.get(i + 1) == Some(&'/') {
                    depth -= 1;
                    blank_at(&mut out, i);
                    blank_at(&mut out, i + 1);
                    i += 2;
                    if depth == 0 {
                        break;
                    }
                } else {
                    blank_at(&mut out, i);
                    i += 1;
                }
            }
        } else if c == 'r' && (next == Some('"') || next == Some('#')) && !prev_is_ident(&b, i) {
            // r"..." or r#"..."#
            let mut j = i + 1;
            let mut hashes = 0;
            while b.get(j) == Some(&'#') {
                hashes += 1;
                j += 1;
            }
            if b.get(j) == Some(&'"') {
                j += 1;
                loop {
                    if j >= b.len() {
                        break;
                    }
                    if b[j] == '"' && (0..hashes).all(|k| b.get(j + 1 + k) == Some(&'#')) {
                        j += 1 + hashes;
                        break;
                    }
                    blank_at(&mut out, j);
                    j += 1;
                }
                i = j;
            } else {
                i += 1;
            }
        } else if c == '"' {
            i += 1;
            while i < b.len() && b[i] != '"' {
                if b[i] == '\\' && i + 1 < b.len() {
                    blank_at(&mut out, i);
                    i += 1;
                }
                blank_at(&mut out, i);
                i += 1;
            }
            i += 1;
        } else if c == '\'' {
            // A character literal ('x', '\n', '\u{1F600}', '"'), not a lifetime.
            let is_char = if next == Some('\\') {
                true
            } else {
                b.get(i + 2) == Some(&'\'') && next != Some('\'')
            };
            if is_char {
                let mut j = i + 1;
                if b[j] == '\\' {
                    j += 1;
                }
                while j < b.len() && b[j] != '\'' {
                    j += 1;
                }
                for k in i + 1..j.min(b.len()) {
                    blank_at(&mut out, k);
                }
                i = j + 1;
            } else {
                i += 1;
            }
        } else {
            i += 1;
        }
    }
    out.into_iter().collect()
}

fn prev_is_ident(b: &[char], i: usize) -> bool {
    i > 0 && (b[i - 1].is_alphanumeric() || b[i - 1] == '_')
}

/// The production code of a source file: `blank`ed, with each
/// `#[cfg(test)] mod name { ... }` (or `mod name;`) at the top level removed.
/// A `#[cfg(test)]` anywhere else (inside a block, on a function or a `use`)
/// is an error: it would hide code from the check.
fn production(src: &str) -> Result<String, String> {
    let code = blank(src);
    let chars: Vec<char> = code.chars().collect();
    let attr: Vec<char> = "#[cfg(test)]".chars().collect();
    let mut out = String::new();
    let mut depth = 0i32;
    let mut i = 0;
    while i < chars.len() {
        if chars[i..].starts_with(&attr) {
            if depth != 0 {
                return Err(format!("#[cfg(test)] inside a block at char {i}"));
            }
            let mut j = i + attr.len();
            while j < chars.len() && chars[j].is_whitespace() {
                j += 1;
            }
            let rest: String = chars[j..].iter().take(4).collect();
            if rest != "mod " {
                return Err("#[cfg(test)] on something that is not a `mod`".to_string());
            }
            while j < chars.len() && chars[j] != '{' && chars[j] != ';' {
                j += 1;
            }
            if j < chars.len() && chars[j] == '{' {
                let mut d = 0;
                while j < chars.len() {
                    match chars[j] {
                        '{' => d += 1,
                        '}' => {
                            d -= 1;
                            if d == 0 {
                                break;
                            }
                        }
                        _ => {}
                    }
                    j += 1;
                }
            }
            // The module is gone; its line breaks stay for the numbering.
            for &c in &chars[i..(j + 1).min(chars.len())] {
                if c == '\n' {
                    out.push('\n');
                }
            }
            i = j + 1;
            continue;
        }
        match chars[i] {
            '{' => depth += 1,
            '}' => depth -= 1,
            _ => {}
        }
        out.push(chars[i]);
        i += 1;
    }
    Ok(out)
}

fn problems(file: &str, src: &str) -> Result<Vec<String>, String> {
    let mut found = Vec::new();
    for (n, line) in production(src)?.lines().enumerate() {
        for token in TOKENS {
            if line.contains(token) && !ALLOWED.iter().any(|(f, t, _)| *f == file && t == token) {
                found.push(format!("{file}:{}: {token}", n + 1));
            }
        }
    }
    Ok(found)
}

#[test]
fn production_code_has_no_panicking_macros() {
    let mut all = Vec::new();
    for file in FILES {
        let src = fs::read_to_string(root().join(file)).expect(file);
        assert!(src.len() > 100, "{file} is empty?");
        match problems(file, &src) {
            Ok(p) => all.extend(p),
            Err(e) => all.push(format!("{file}: {e}")),
        }
    }
    assert!(
        all.is_empty(),
        "a panic message can carry text into a crash report:\n{}",
        all.join("\n")
    );
}

#[test]
fn checker_sees_what_it_should() {
    let n = |src: &str| problems("t.rs", src).unwrap().len();
    // Caught: a plain unwrap, one after a string holding "//", one on the same
    // line as a trailing comment, and one in production code after a test module.
    assert_eq!(n("fn f(x: Option<u8>) -> u8 {\n    x.unwrap()\n}\n"), 1);
    assert_eq!(
        n("fn f(x: Option<u8>) -> u8 {\n    let _u = \"http://a\"; x.unwrap() // c\n}\n"),
        1
    );
    assert_eq!(
        n("fn f(x: Option<u8>) { let _ = r#\"//\"#; x.unwrap(); }\n"),
        1
    );
    assert_eq!(n("fn f(x: Option<u8>) { let _c = '\"'; x.unwrap(); }\n"), 1);
    assert_eq!(
        n(
            "#[cfg(test)]\nmod tests {\n    fn t() { panic!(\"x\"); }\n}\n\nfn g(x: Option<u8>) { x.unwrap(); }\n"
        ),
        1
    );
    // Not caught, rightly: tests, comments (line and block, nested), strings.
    assert_eq!(
        n("fn f() {}\n\n#[cfg(test)]\nmod tests {\n    fn t() { panic!(\"x\"); }\n}\n"),
        0
    );
    assert_eq!(n("#[cfg(test)]\nmod props;\nfn f() {}\n"), 0);
    assert_eq!(n("fn f() {\n    // x.unwrap()\n}\n"), 0);
    assert_eq!(n("fn f() {\n    /* a /* nested */ x.unwrap() */\n}\n"), 0);
    assert_eq!(
        n("fn f() -> &'static str {\n    \"panic!(x) .unwrap()\"\n}\n"),
        0
    );
    assert_eq!(n("fn f(x: Option<u8>) -> u8 {\n    x.unwrap_or(0)\n}\n"), 0);
    // A lifetime is not a character literal that swallows the code after it.
    assert_eq!(n("fn f<'a>(x: &'a Option<u8>) { x.unwrap(); }\n"), 1);
    // Refused loudly: cfg(test) where it could hide production code.
    assert!(problems("t.rs", "#[cfg(test)]\nfn f() { x.unwrap(); }\n").is_err());
    assert!(problems("t.rs", "mod m {\n    #[cfg(test)]\n    mod t {}\n}\n").is_err());
    assert!(problems("t.rs", "#[cfg(test)]\nuse std::fs;\n").is_err());
}
