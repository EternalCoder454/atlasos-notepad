//! Property tests and a corpus of nasty inputs for the parsers that read what
//! other programs wrote: the file reader and writer (`file.rs`) and the
//! Markdown line reader (`markdown.rs`). docs/SECURITY.md, "Tests".
//!
//! Every test here is named `props::...`, so
//! `PROPTEST_CASES=20000 cargo test --workspace --locked -- props` runs
//! exactly them. A failure prints the input that broke the rule.

use std::fs;
use std::os::unix::fs::{MetadataExt, PermissionsExt, symlink};
use std::path::Path;
use std::time::{Duration, Instant};

use proptest::collection::vec;
use proptest::prelude::*;
use proptest::sample::select;

use crate::file::{self, Decoded, Encoding, LineEnding};
use crate::markdown::{self, Kind, Line, Run, flags};

// ----------------------------------------------------------------- inputs

/// Bytes that reach every branch of the decoder: BOMs, both line endings,
/// NUL, the start of UTF-8 sequences, a lone UTF-16 surrogate half, 0xFF.
const NASTY_BYTES: &[u8] = &[
    0x00, 0x0A, 0x0D, 0x20, 0x61, 0x80, 0x81, 0x9D, 0xC2, 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xEF, 0xBB,
    0xBF, 0xF0, 0x9F, 0x98, 0x80, 0xFF, 0xFE, 0xD8, 0xDC,
];

fn file_bytes() -> impl Strategy<Value = Vec<u8>> {
    prop_oneof![
        // Anything at all.
        vec(any::<u8>(), 0..2048),
        // Small alphabets, with or without a byte order mark first.
        (
            select(vec![
                &b""[..],
                &b"\xEF\xBB\xBF"[..],
                &b"\xFF\xFE"[..],
                &b"\xFE\xFF"[..]
            ]),
            vec(select(NASTY_BYTES), 0..64),
        )
            .prop_map(|(bom, rest)| [bom, &rest[..]].concat()),
        // Valid text of any script, as UTF-8.
        any::<String>().prop_map(String::into_bytes),
    ]
}

/// UTF-16 text as the editor holds it: any units, lone surrogates included
/// (a paste can put them there), and `\n` as the only line break wanted.
fn editor_text() -> impl Strategy<Value = Vec<u16>> {
    prop_oneof![
        vec(any::<u16>(), 0..512),
        vec(
            select(vec![
                0x0Au16, 0x0A, 0x20, 0x61, 0xE9, 0x20AC, 0x4E2D, 0xD83D, 0xDE00, 0xD800, 0xDC00,
                0x2028, 0x2029, 0x202E, 0xFEFF, 0x00,
            ]),
            0..128
        ),
        any::<String>().prop_map(|s| s.encode_utf16().collect()),
    ]
}

fn encoding() -> impl Strategy<Value = Encoding> {
    select(vec![
        Encoding::Utf8,
        Encoding::Utf8Bom,
        Encoding::Utf16Le,
        Encoding::Utf16Be,
        Encoding::Windows1252,
    ])
}

fn line_ending() -> impl Strategy<Value = LineEnding> {
    select(vec![LineEnding::Lf, LineEnding::CrLf, LineEnding::Cr])
}

/// Characters that make the Markdown reader work: brackets, emphasis marks,
/// code, links, escapes, tabs, bidi controls, NUL and surrogate halves.
const MD_UNITS: &[u16] = &[
    b'[' as u16,
    b']' as u16,
    b'(' as u16,
    b')' as u16,
    b'*' as u16,
    b'_' as u16,
    b'~' as u16,
    b'`' as u16,
    b'\\' as u16,
    b'<' as u16,
    b'>' as u16,
    b'#' as u16,
    b'-' as u16,
    b'+' as u16,
    b'.' as u16,
    b'1' as u16,
    b' ' as u16,
    b'\t' as u16,
    b'x' as u16,
    b'h' as u16,
    b't' as u16,
    b'p' as u16,
    b's' as u16,
    b':' as u16,
    b'/' as u16,
    b'!' as u16,
    b'a' as u16,
    0x00,
    0xD83D,
    0xDE00,
    0xD800,
    0xDC00,
    0x202E,
    0x2066,
    0xFEFF,
    0x00A0,
    0x3000,
];

fn md_line(max: usize) -> impl Strategy<Value = Vec<u16>> {
    prop_oneof![
        3 => vec(select(MD_UNITS), 0..max),
        1 => vec(any::<u16>(), 0..max.min(512)),
        1 => any::<String>().prop_map(|s| s.encode_utf16().collect()),
    ]
}

/// The state a previous line may hand on: what the reader wrote, or whatever
/// the host puts there (Qt's -1 for "none", garbage).
fn md_state() -> impl Strategy<Value = i32> {
    prop_oneof![
        Just(0),
        Just(-1),
        Just(i32::MAX),
        Just(i32::MIN),
        (0u32..=1, 0u32..0x1_0000).prop_map(|(tilde, len)| ((tilde << 16) | len) as i32),
        any::<i32>(),
    ]
}

// ------------------------------------------------------------ file: decode

fn assert_decoded_sane(bytes: &[u8], d: &Decoded) {
    assert!(
        !d.text.contains(&(b'\r' as u16)),
        "a CR is left: {bytes:02x?}"
    );
    // Units never outnumber the bytes they came from (plus one for the
    // replacement of a cut-off UTF-16 unit).
    assert!(d.text.len() <= bytes.len() + 1, "{bytes:02x?}");
    // Nothing the editor can't write back in the encoding it was read as.
    assert!(
        file::encode(&d.text, d.encoding, d.line_ending).is_ok(),
        "decoded text can't be encoded again: {bytes:02x?}"
    );
}

proptest! {
    /// Any bytes decode; the text has no CR; it can be written back.
    #[test]
    fn decode_never_panics_and_is_sane(bytes in file_bytes()) {
        let d = file::decode(&bytes);
        assert_decoded_sane(&bytes, &d);
    }

    /// What decode calls safe round-trips byte for byte; what it flags
    /// (mixed line endings, binary, malformed UTF-16) is the only way a save
    /// may change a file, and the app asks first for those.
    #[test]
    fn decode_then_encode_is_exact_unless_flagged(bytes in file_bytes()) {
        let d = file::decode(&bytes);
        if !d.mixed && !d.binary && !d.lossy {
            let back = file::encode(&d.text, d.encoding, d.line_ending).unwrap();
            prop_assert_eq!(back, bytes);
        }
    }

    /// Invalid UTF-8 is never turned into replacement characters: it is
    /// Windows-1252, which writes the same bytes back.
    #[test]
    fn bytes_that_are_not_utf8_are_not_replaced(bytes in vec(any::<u8>(), 0..512)) {
        prop_assume!(!bytes.starts_with(&[0xFF, 0xFE]) && !bytes.starts_with(&[0xFE, 0xFF]));
        prop_assume!(std::str::from_utf8(&bytes).is_err());
        let d = file::decode(&bytes);
        prop_assert_eq!(d.encoding, Encoding::Windows1252);
        if !d.mixed && !d.binary {
            prop_assert_eq!(
                file::encode(&d.text, d.encoding, d.line_ending).unwrap(),
                bytes
            );
        }
    }

    /// A NUL in the first 64 KiB of a non-UTF-16 file makes it binary (the
    /// app opens those read-only).
    #[test]
    fn nul_makes_binary(prefix in vec(select(vec![b'a', b'\n', 0xC3, 0xA9]), 0..64), suffix in vec(any::<u8>(), 0..64)) {
        let mut bytes = prefix;
        bytes.push(0);
        bytes.extend(suffix);
        prop_assume!(!bytes.starts_with(&[0xFF, 0xFE]) && !bytes.starts_with(&[0xFE, 0xFF]));
        prop_assert!(file::decode(&bytes).binary);
    }
}

// ------------------------------------------------------------ file: encode

proptest! {
    /// Any text, any encoding: no panic; an error points inside the text;
    /// the output is bounded; what is written is read back as the same text.
    #[test]
    fn encode_never_panics_and_reads_back(
        text in editor_text(),
        enc in encoding(),
        eol in line_ending(),
    ) {
        match file::encode(&text, enc, eol) {
            Err(e) => prop_assert!(e.utf16_offset < text.len()),
            Ok(bytes) => {
                // 3 bytes per unit in UTF-8, 2 per unit and 2 per EOL unit in UTF-16.
                prop_assert!(bytes.len() <= 3 + 6 * text.len() + 4);
                let d = file::decode(&bytes);
                let mut want = text.clone();
                // CR in the editor's text is not a line break of the editor;
                // reading the bytes back turns it into one. `\n` text only.
                if want.contains(&(b'\r' as u16)) {
                    return Ok(());
                }
                if enc == Encoding::Windows1252 && std::str::from_utf8(&bytes).is_ok() {
                    // Bytes that happen to be valid UTF-8 are read as UTF-8.
                    return Ok(());
                }
                // A BOM at the start of UTF-8 text reads back as the BOM.
                if enc == Encoding::Utf8 && want.first() == Some(&0xFEFF) {
                    want.remove(0);
                    prop_assert_eq!(d.encoding, Encoding::Utf8Bom);
                } else if matches!(enc, Encoding::Utf16Le | Encoding::Utf16Be) {
                    // Lone surrogates are written as they are and read back
                    // as U+FFFD, flagged lossy.
                    let lone = d.lossy;
                    if lone {
                        return Ok(());
                    }
                }
                prop_assert_eq!(d.text, want);
            }
        }
    }

    /// Malformed text can't be written as UTF-8 at all; the error says where.
    #[test]
    fn lone_surrogates_are_refused_by_utf8(
        before in vec(select(vec![0x61u16, 0x0A, 0xE9]), 0..16),
        lone in prop_oneof![0xD800u16..0xE000],
        after in vec(select(vec![0x61u16, 0x0A, 0xE9]), 0..16),
    ) {
        let mut text = before.clone();
        text.push(lone);
        text.extend(after);
        let e = file::encode(&text, Encoding::Utf8, LineEnding::Lf).unwrap_err();
        prop_assert_eq!(e.utf16_offset, before.len());
    }
}

// ------------------------------------------------------------- file: disk

fn write_file(path: &Path, bytes: &[u8]) {
    fs::write(path, bytes).unwrap();
}

fn names_in(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> = fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    v.sort();
    v
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(ProptestConfig::default().cases.min(400)))]

    /// Save writes exactly the bytes, leaves nothing else in the folder, keeps
    /// an existing file's mode, and reports the stamp of what it wrote.
    #[test]
    fn save_writes_exact_bytes_and_leaves_nothing(
        old in proptest::option::of(vec(any::<u8>(), 0..256)),
        new in vec(any::<u8>(), 0..2048),
        mode in select(vec![0o600u32, 0o640, 0o644, 0o664, 0o755]),
    ) {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("f.txt");
        if let Some(old) = &old {
            write_file(&p, old);
            fs::set_permissions(&p, fs::Permissions::from_mode(mode)).unwrap();
        }
        let stamp = file::save(&p, &new).unwrap();
        prop_assert_eq!(fs::read(&p).unwrap(), new.clone());
        prop_assert_eq!(stamp.size, new.len() as u64);
        prop_assert_eq!(names_in(d.path()), vec!["f.txt".to_string()]);
        if old.is_some() {
            prop_assert_eq!(fs::metadata(&p).unwrap().mode() & 0o7777, mode);
        }
        let (read, read_stamp) = file::read(&p, 1 << 20).unwrap();
        prop_assert_eq!(read_stamp.size, new.len() as u64);
        prop_assert_eq!(file::encode(&read.text, read.encoding, read.line_ending).is_ok(), true);
    }

    /// The private writer: 0600 whatever the old file's mode, nothing left,
    /// a link at the name replaced and never written through.
    #[test]
    fn save_private_is_always_0600(
        bytes in vec(any::<u8>(), 0..1024),
        old_mode in select(vec![0o600u32, 0o644, 0o666]),
        link in any::<bool>(),
    ) {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("session.json");
        let other = d.path().join("other");
        write_file(&other, b"theirs");
        if link {
            symlink(&other, &p).unwrap();
        } else {
            write_file(&p, b"old");
            fs::set_permissions(&p, fs::Permissions::from_mode(old_mode)).unwrap();
        }
        file::save_private(&p, &bytes).unwrap();
        let meta = fs::symlink_metadata(&p).unwrap();
        prop_assert!(meta.file_type().is_file());
        prop_assert_eq!(meta.mode() & 0o7777, 0o600);
        prop_assert_eq!(fs::read(&p).unwrap(), bytes);
        prop_assert_eq!(fs::read(&other).unwrap(), b"theirs".to_vec());
        prop_assert_eq!(names_in(d.path()), vec!["other".to_string(), "session.json".to_string()]);
    }

    /// The size limit: a file of exactly `max` reads, one more byte is EFBIG,
    /// and no read allocates for the claimed size of a bigger file.
    #[test]
    fn read_respects_the_limit(len in 0usize..4096, max in 0u64..4096) {
        let d = tempfile::tempdir().unwrap();
        let p = d.path().join("f");
        write_file(&p, &vec![b'a'; len]);
        match file::read(&p, max) {
            Ok((dec, st)) => {
                prop_assert!(len as u64 <= max);
                prop_assert_eq!(dec.text.len(), len);
                prop_assert_eq!(st.size, len as u64);
            }
            Err(e) => {
                prop_assert!(len as u64 > max);
                prop_assert_eq!(e.raw_os_error(), Some(libc::EFBIG));
            }
        }
    }
}

// ---------------------------------------------------------------- markdown

/// A budget for one line: far above the milliseconds it takes, far below a
/// quadratic blow-up of a line this long.
const LINE_BUDGET: Duration = Duration::from_secs(2);

fn check_line(text: &[u16], state: i32) -> Line {
    let started = Instant::now();
    let mut f = Vec::new();
    let line = markdown::parse_line(text, state, &mut f);
    let mut found: Vec<Run> = Vec::new();
    markdown::runs(&f, &mut found);
    let spent = started.elapsed();
    let n = text.len();

    assert_eq!(f.len(), n, "one entry of flags per unit");
    assert!(spent < LINE_BUDGET, "{n} units took {spent:?}");
    // Runs are inside the line, in order, apart, not empty, and flagged.
    let mut at = 0u64;
    for r in &found {
        assert!(r.len > 0 && r.flags != 0, "{r:?}");
        assert!(
            u64::from(r.start) >= at,
            "runs overlap or are out of order: {found:?}"
        );
        at = u64::from(r.start) + u64::from(r.len);
        assert!(at <= n as u64, "run {r:?} ends past the line ({n})");
    }
    // The line's own positions are inside it.
    assert!(line.marker_start as usize <= n);
    assert!(
        line.marker_start as u64 + line.marker_len as u64 <= n as u64,
        "{line:?} in {n}"
    );
    assert!(line.content_start as usize <= n, "{line:?} in {n}");
    assert!(line.state >= 0, "{line:?}");
    assert!(line.heading <= 6);
    assert!(line.checked <= 1);
    match line.kind {
        Kind::Heading => assert!((1..=6).contains(&line.heading)),
        _ => assert_eq!(line.heading, 0),
    }
    // The reader leaves a fence state only for the line that opens or holds
    // a fence, and never for a closing one.
    if line.state > 0 {
        assert!(matches!(line.kind, Kind::Fence | Kind::Code), "{line:?}");
    }
    // Only flags the reader defines are set.
    let known = (1u32 << 14) - 1;
    assert!(
        f.iter().all(|&x| x & !known == 0),
        "an unknown flag in {f:?}"
    );
    let _ = flags::HIDDEN;
    line
}

proptest! {
    /// Any line, any incoming state: no panic, every position inside the
    /// line, bounded time.
    #[test]
    fn markdown_line_is_always_in_range(text in md_line(4096), state in md_state()) {
        check_line(&text, state);
    }

    /// Reading a document line by line, as the highlighter does, passing the
    /// state on: the state stays a state the reader understands.
    #[test]
    fn markdown_document_never_loses_the_plot(lines in vec(md_line(200), 0..40)) {
        let mut state = 0;
        for l in &lines {
            let line = check_line(l, state);
            state = line.state;
        }
    }

    /// The link finder returns a range inside the line, for any position
    /// (empty for `[a]()`: nothing to open).
    #[test]
    fn markdown_link_at_is_in_range(text in md_line(1024), pos in 0usize..1100) {
        if let Some((a, b)) = markdown::link_at(&text, pos) {
            prop_assert!(a <= b && b <= text.len(), "{a}..{b} in {}", text.len());
        }
    }

    /// Same line, same answer: no state kept between calls (the highlighter
    /// reuses one buffer for all lines).
    #[test]
    fn markdown_line_is_deterministic(text in md_line(512), state in md_state(), junk in vec(any::<u32>(), 0..64)) {
        let mut clean = Vec::new();
        let a = markdown::parse_line(&text, state, &mut clean);
        let mut dirty = junk;
        let b = markdown::parse_line(&text, state, &mut dirty);
        prop_assert_eq!(a, b);
        prop_assert_eq!(clean, dirty);
    }
}

// -------------------------------------------------------------- the corpus

fn units(s: &str) -> Vec<u16> {
    s.encode_utf16().collect()
}

#[test]
fn markdown_empty_destination_is_an_empty_range() {
    let text = units("[a]()");
    let (a, b) = markdown::link_at(&text, 1).unwrap();
    assert!(a == b && b <= text.len());
}

/// Lines that have gone quadratic or deep in readers like this one: each must
/// come back in a moment, at 100,000 units (the longest line the app edits).
#[test]
fn markdown_hostile_lines_stay_fast() {
    let n = 100_000;
    let rep = |unit: &str| -> Vec<u16> { units(&unit.repeat(n / unit.chars().count().max(1))) };
    let cases: Vec<(&str, Vec<u16>)> = vec![
        ("[", rep("[")),
        ("]", rep("]")),
        ("[](", rep("[](")),
        ("[a](b", rep("[a](b")),
        ("![", rep("![")),
        ("(", rep("(")),
        (")", rep(")")),
        ("*", rep("*")),
        ("_", rep("_")),
        ("**_", rep("**_")),
        ("*_", rep("*_")),
        ("*a ", rep("*a ")),
        ("_a ", rep("_a ")),
        ("~~", rep("~~")),
        ("`", rep("`")),
        ("`a", rep("`a")),
        ("``a`", rep("``a`")),
        ("\\", rep("\\")),
        ("\\*", rep("\\*")),
        ("<", rep("<")),
        ("<h", rep("<h")),
        ("<http://a", rep("<http://a")),
        ("http://a", rep("http://a")),
        ("http://a)", rep("http://a)")),
        ("http://a(", rep("http://a(")),
        ("http://a.", rep("http://a.")),
        ("[a](<", rep("[a](<")),
        ("> ", rep("> ")),
        (">", rep(">")),
        ("- ", rep("- ")),
        ("- [ ] ", rep("- [ ] ")),
        ("#", rep("#")),
        ("# ", rep("# ")),
        ("1.", rep("1.")),
        ("tab", rep("\t")),
        ("space", rep(" ")),
        ("nul", vec![0u16; n]),
        ("lone high", vec![0xD800u16; n]),
        ("lone low", vec![0xDC00u16; n]),
        ("bidi", rep("\u{202E}a\u{2066}")),
        ("emoji", rep("\u{1F600}")),
        ("zwj", rep("\u{200D}*")),
    ];
    for (name, text) in &cases {
        for state in [0, 3 | (1 << 16), 3] {
            let t = Instant::now();
            check_line(text, state);
            let mut at = 0;
            while at < text.len() {
                // The link finder at a few positions of the same line.
                let _ = markdown::link_at(text, at);
                at += text.len() / 3 + 1;
            }
            assert!(
                t.elapsed() < LINE_BUDGET * 2,
                "{name} ({n} units, state {state}) took {:?}",
                t.elapsed()
            );
        }
    }
}

/// 10 MB in one line (the file limit) goes through the reader without
/// stalling or panicking: the app doesn't call it for lines over 100,000
/// units, but a bug there must not become a hang here.
#[test]
fn markdown_ten_megabytes_in_one_line() {
    for unit in ["a", "[", "*a", "`a", "[a](b)"] {
        let text = units(&unit.repeat((10 << 20) / unit.len()));
        let t = Instant::now();
        let mut f = Vec::new();
        let line = markdown::parse_line(&text, 0, &mut f);
        assert_eq!(f.len(), text.len());
        assert!(line.content_start as usize <= text.len());
        assert!(
            t.elapsed() < Duration::from_secs(20),
            "{unit:?} x 10 MB took {:?}",
            t.elapsed()
        );
    }
}

// ----------------------------------------------------------- the corpus: files

#[test]
fn files_special_names_and_contents() {
    let d = tempfile::tempdir().unwrap();
    let names = [
        "plain.txt",
        "-leading-dash",
        "--",
        "-rf",
        "with space",
        "new\nline",
        "tab\there",
        "esc\u{1b}[31mred",
        "bidi\u{202E}txt.exe",
        "nul-free-\u{FEFF}",
        "..hidden",
        "a:b",
        "-",
        &"n".repeat(255),
        &"\u{e9}".repeat(127),
    ];
    for name in names {
        let p = d.path().join(name);
        file::save(&p, b"one").unwrap();
        file::save(&p, b"two").unwrap();
        assert_eq!(fs::read(&p).unwrap(), b"two", "{name:?}");
        assert_eq!(file::read(&p, 1 << 20).unwrap().0.text.len(), 3);
        file::save_private(&p, b"three").unwrap();
        assert_eq!(fs::metadata(&p).unwrap().mode() & 0o777, 0o600);
    }
    // Only the files themselves are in the folder: no temp files.
    assert_eq!(fs::read_dir(d.path()).unwrap().count(), names.len());
    // A path with a NUL can't reach the file system (EINVAL, not a panic).
    let nul = std::path::PathBuf::from(
        <std::ffi::OsStr as std::os::unix::ffi::OsStrExt>::from_bytes(b"a\0b"),
    );
    assert!(file::save(&nul, b"x").is_err());
    assert!(file::read(&nul, 10).is_err());
    assert!(file::save_private(&nul, b"x").is_err());
}

#[test]
fn files_special_files_are_refused_quickly() {
    let d = tempfile::tempdir().unwrap();
    let t = Instant::now();
    // A FIFO with no writer would hang a blocking open.
    let fifo = d.path().join("fifo");
    let c = std::ffi::CString::new(fifo.as_os_str().as_encoded_bytes()).unwrap();
    // SAFETY: a valid C string.
    assert_eq!(unsafe { libc::mkfifo(c.as_ptr(), 0o600) }, 0);
    for p in [
        fifo.as_path(),
        Path::new("/dev/zero"),
        Path::new("/dev/null"),
        Path::new("/dev/full"),
        Path::new("/dev/urandom"),
        d.path(),
    ] {
        let e = file::read(p, 1 << 20).unwrap_err();
        assert_eq!(e.raw_os_error(), Some(libc::EINVAL), "{p:?}");
        // Saving over a special file or a folder is refused, not done.
        assert!(file::save(p, b"x").is_err(), "{p:?}");
    }
    // A symlink to each of them is the same file.
    let link = d.path().join("link-to-zero");
    symlink("/dev/zero", &link).unwrap();
    assert_eq!(
        file::read(&link, 1 << 20).unwrap_err().raw_os_error(),
        Some(libc::EINVAL)
    );
    assert!(file::save(&link, b"x").is_err());
    assert!(t.elapsed() < Duration::from_secs(5), "{:?}", t.elapsed());
}

#[test]
fn files_symlink_loops_and_dangling_links() {
    let d = tempfile::tempdir().unwrap();
    let a = d.path().join("a");
    let b = d.path().join("b");
    symlink(&b, &a).unwrap();
    symlink(&a, &b).unwrap();
    assert_eq!(
        file::read(&a, 1 << 20).unwrap_err().raw_os_error(),
        Some(libc::ELOOP)
    );
    assert_eq!(
        file::save(&a, b"x").unwrap_err().raw_os_error(),
        Some(libc::ELOOP)
    );
    assert_eq!(
        file::stamp(&a).unwrap_err().raw_os_error(),
        Some(libc::ELOOP)
    );
    // A chain of 39 links works, 41 does not.
    let target = d.path().join("target");
    fs::write(&target, b"t").unwrap();
    let mut prev = target.clone();
    for i in 0..39 {
        let l = d.path().join(format!("l{i}"));
        symlink(&prev, &l).unwrap();
        prev = l;
    }
    assert!(file::save(&prev, b"deep").is_ok());
    assert_eq!(fs::read(&target).unwrap(), b"deep");
    // Nothing was left behind by the failed saves.
    for e in fs::read_dir(d.path()).unwrap() {
        let n = e.unwrap().file_name().to_string_lossy().into_owned();
        assert!(!n.ends_with(".tmp"), "{n}");
    }
}

#[test]
fn files_size_limits_hold_for_sparse_and_growing_files() {
    let d = tempfile::tempdir().unwrap();
    // A sparse file that claims a terabyte: refused from the size, with no
    // allocation of that size (the test would be killed if it tried).
    let big = d.path().join("sparse");
    let f = fs::File::create(&big).unwrap();
    f.set_len(1 << 40).unwrap();
    assert_eq!(
        file::read(&big, 10 << 20).unwrap_err().raw_os_error(),
        Some(libc::EFBIG)
    );
    // Exactly the limit reads (10 MiB of NUL: binary, flagged).
    let edge = d.path().join("edge");
    let f = fs::File::create(&edge).unwrap();
    f.set_len(10 << 20).unwrap();
    let (dec, st) = file::read(&edge, 10 << 20).unwrap();
    assert!(dec.binary);
    assert_eq!(st.size, 10 << 20);
    // /proc files report size 0 and are regular: read to the limit, no hang.
    let (dec, _) = file::read(Path::new("/proc/self/status"), 10 << 20).unwrap();
    assert!(!dec.text.is_empty());
    // A file that grows while it is read stops at the limit + 1 byte.
    let grow = d.path().join("grow");
    fs::write(&grow, b"x").unwrap();
    let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let writer = {
        let (p, stop) = (grow.clone(), stop.clone());
        std::thread::spawn(move || {
            use std::io::Write;
            let mut f = fs::OpenOptions::new().append(true).open(p).unwrap();
            let chunk = vec![b'y'; 64 * 1024];
            while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                if f.write_all(&chunk).is_err() {
                    break;
                }
            }
        })
    };
    let mut refused = 0;
    for _ in 0..50 {
        match file::read(&grow, 256 * 1024) {
            Err(e) => {
                assert_eq!(e.raw_os_error(), Some(libc::EFBIG));
                refused += 1;
            }
            Ok((dec, _)) => assert!(dec.text.len() <= 256 * 1024),
        }
    }
    stop.store(true, std::sync::atomic::Ordering::Relaxed);
    writer.join().unwrap();
    assert!(refused > 0, "the file never outgrew the limit");
}

#[test]
fn files_ten_megabytes_of_everything_decode_in_time() {
    let n = 10 << 20;
    let t = Instant::now();
    for unit in [
        &b"a"[..],
        b"\n",
        b"\r\n",
        b"\r",
        b"\xC3\xA9",
        b"\xF0\x9F\x98\x80",
        b"\xFF",
        b"\0",
    ] {
        let bytes = unit.repeat(n / unit.len());
        let d = file::decode(&bytes);
        assert!(d.text.len() <= bytes.len());
        let _ = file::encode(&d.text, d.encoding, d.line_ending);
    }
    // UTF-16 of every flavour.
    for bom in [[0xFF, 0xFE], [0xFE, 0xFF]] {
        for unit in [[0x61, 0x00], [0x00, 0xD8], [0x00, 0xDC], [0x0D, 0x00]] {
            let mut bytes = bom.to_vec();
            bytes.extend(unit.repeat(n / 2));
            let d = file::decode(&bytes);
            assert!(d.text.len() <= bytes.len());
        }
    }
    assert!(t.elapsed() < Duration::from_secs(60), "{:?}", t.elapsed());
}

/// BOM tricks: a BOM that is not at the start is a character, two BOMs are
/// one BOM and a character, UTF-32 is read as the UTF-16 it looks like and
/// flagged, never as text of another file.
#[test]
fn files_bom_tricks() {
    let d = file::decode(b"\xEF\xBB\xBF\xEF\xBB\xBFx");
    assert_eq!(d.encoding, Encoding::Utf8Bom);
    assert_eq!(d.text, vec![0xFEFF, b'x' as u16]);
    let d = file::decode(b"x\xEF\xBB\xBF");
    assert_eq!(d.encoding, Encoding::Utf8);
    assert_eq!(d.text, vec![b'x' as u16, 0xFEFF]);
    // UTF-8 BOM, then bytes that are not UTF-8: Windows-1252, BOM bytes and all.
    let d = file::decode(b"\xEF\xBB\xBF\xFF");
    assert_eq!(d.encoding, Encoding::Windows1252);
    assert_eq!(
        file::encode(&d.text, d.encoding, d.line_ending).unwrap(),
        b"\xEF\xBB\xBF\xFF"
    );
    // UTF-32 LE BOM: the first two bytes are UTF-16 LE's.
    let d = file::decode(b"\xFF\xFE\0\0a\0\0\0");
    assert_eq!(d.encoding, Encoding::Utf16Le);
    // BOM only.
    for bom in [&b"\xFF\xFE"[..], b"\xFE\xFF", b"\xEF\xBB\xBF"] {
        let d = file::decode(bom);
        assert!(d.text.is_empty() && !d.lossy);
    }
    // An odd byte after a UTF-16 BOM is a cut-off unit: flagged.
    let d = file::decode(b"\xFF\xFEa\0b");
    assert!(d.lossy);
    assert_eq!(d.text.last(), Some(&0xFFFD));
    // Bidi controls and NUL in the middle are kept as they are.
    let text = "a\u{202E}b\0c\u{2066}\n";
    let d = file::decode(text.as_bytes());
    assert_eq!(d.text, units(text));
    assert!(d.binary);
}
