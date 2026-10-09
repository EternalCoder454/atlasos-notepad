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

/// (file, token) pairs allowed, with the reason. None today.
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

/// The part of a source file before its `#[cfg(test)]` module, with comments
/// and the contents of string literals blanked.
fn production(src: &str) -> String {
    let cut = src.find("\n#[cfg(test)]").unwrap_or(src.len());
    let mut out = String::new();
    for line in src[..cut].lines() {
        let code = line.split("//").next().unwrap_or("");
        out.push_str(code);
        out.push('\n');
    }
    out
}

fn problems(file: &str, src: &str) -> Vec<String> {
    let mut found = Vec::new();
    for (n, line) in production(src).lines().enumerate() {
        for token in TOKENS {
            if line.contains(token) && !ALLOWED.iter().any(|(f, t, _)| *f == file && t == token) {
                found.push(format!("{file}:{}: {token}", n + 1));
            }
        }
    }
    found
}

#[test]
fn production_code_has_no_panicking_macros() {
    let mut all = Vec::new();
    for file in FILES {
        let src = fs::read_to_string(root().join(file)).expect(file);
        assert!(src.len() > 100, "{file} is empty?");
        all.extend(problems(file, &src));
    }
    assert!(
        all.is_empty(),
        "a panic message can carry text into a crash report:\n{}",
        all.join("\n")
    );
}

#[test]
fn checker_sees_what_it_should() {
    let bad = "fn f(x: Option<u8>) -> u8 {\n    x.unwrap()\n}\n";
    assert_eq!(problems("t.rs", bad).len(), 1);
    let tests_only = "fn f() {}\n\n#[cfg(test)]\nmod tests {\n    fn t() { panic!(\"x\"); }\n}\n";
    assert!(problems("t.rs", tests_only).is_empty());
    let commented = "fn f() {\n    // x.unwrap()\n}\n";
    assert!(problems("t.rs", commented).is_empty());
    let ok = "fn f(x: Option<u8>) -> u8 {\n    x.unwrap_or(0)\n}\n";
    assert!(problems("t.rs", ok).is_empty());
}
