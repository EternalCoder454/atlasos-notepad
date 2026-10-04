//! Files: reading them into UTF-16 text, writing text back the way the file
//! was, and saving without ever losing the old file.
//!
//! The editor keeps text as UTF-16 with `\n` between lines, whatever the file
//! used. [`decode`] reads the file's encoding and line endings and remembers
//! them; [`encode`] turns the text back, so a file that was opened and saved
//! without edits comes out byte for byte the same (unless it mixed line
//! endings, had malformed UTF-16 or looked binary: see [`Decoded`]).

use std::ffi::{CString, OsStr};
use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

/// How a file's text is stored. The numbers are part of the C ABI.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Encoding {
    Utf8 = 0,
    /// UTF-8 with a byte order mark.
    Utf8Bom = 1,
    /// UTF-16 little endian, with a byte order mark.
    Utf16Le = 2,
    /// UTF-16 big endian, with a byte order mark.
    Utf16Be = 3,
    /// Windows-1252 (the fallback for bytes that aren't UTF-8). Every byte
    /// maps to a character, so reading and writing it back is exact.
    Windows1252 = 4,
}

impl Encoding {
    pub fn from_u8(n: u8) -> Option<Self> {
        Some(match n {
            0 => Self::Utf8,
            1 => Self::Utf8Bom,
            2 => Self::Utf16Le,
            3 => Self::Utf16Be,
            4 => Self::Windows1252,
            _ => return None,
        })
    }
}

/// What separates lines in a file. The numbers are part of the C ABI.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LineEnding {
    Lf = 0,
    CrLf = 1,
    Cr = 2,
}

impl LineEnding {
    pub fn from_u8(n: u8) -> Option<Self> {
        Some(match n {
            0 => Self::Lf,
            1 => Self::CrLf,
            2 => Self::Cr,
            _ => return None,
        })
    }
}

/// What a file looked like when we read or wrote it. If the stamp of the file
/// on disk differs later, something else changed it.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Stamp {
    pub mtime_ns: i64,
    pub size: u64,
    pub dev: u64,
    pub ino: u64,
}

/// A file's text and how it was stored.
#[derive(Clone, Debug)]
pub struct Decoded {
    /// UTF-16, every line ending turned into `\n`, no byte order mark.
    pub text: Vec<u16>,
    pub encoding: Encoding,
    /// The most common line ending (LF if there is a tie or no line break).
    pub line_ending: LineEnding,
    /// More than one kind of line ending occurred. Saving would change some.
    pub mixed: bool,
    /// A NUL byte in the first 64 KiB of a file that isn't UTF-16: this is
    /// probably not text.
    pub binary: bool,
    /// Malformed UTF-16 (or a cut-off last unit) was replaced with U+FFFD.
    pub lossy: bool,
}

/// [`encode`] met a character the encoding can't hold.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Unencodable {
    /// Where it is, in UTF-16 units.
    pub utf16_offset: usize,
}

impl fmt::Display for Unencodable {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "character at offset {} can't be encoded",
            self.utf16_offset
        )
    }
}

impl std::error::Error for Unencodable {}

const BINARY_SCAN: usize = 64 * 1024;
const LF: u16 = b'\n' as u16;
const CR: u16 = b'\r' as u16;

/// Reads a file's bytes as text. Never fails: bytes that aren't UTF-8 are
/// Windows-1252.
pub fn decode(bytes: &[u8]) -> Decoded {
    let mut lossy = false;
    let mut binary = false;
    let (encoding, mut text) = match bytes {
        [0xEF, 0xBB, 0xBF, rest @ ..] if std::str::from_utf8(rest).is_ok() => {
            (Encoding::Utf8Bom, utf8_or_1252(rest).1)
        }
        [0xFF, 0xFE, rest @ ..] => {
            let (t, bad) = utf16(rest, u16::from_le_bytes);
            lossy = bad;
            (Encoding::Utf16Le, t)
        }
        [0xFE, 0xFF, rest @ ..] => {
            let (t, bad) = utf16(rest, u16::from_be_bytes);
            lossy = bad;
            (Encoding::Utf16Be, t)
        }
        _ => {
            binary = bytes[..bytes.len().min(BINARY_SCAN)].contains(&0);
            utf8_or_1252(bytes)
        }
    };
    let (crlf, lf, cr) = normalize(&mut text);
    let kinds = [crlf, lf, cr].iter().filter(|&&n| n > 0).count();
    let mut line_ending = LineEnding::Lf;
    let mut best = lf;
    if crlf > best {
        line_ending = LineEnding::CrLf;
        best = crlf;
    }
    if cr > best {
        line_ending = LineEnding::Cr;
    }
    Decoded {
        text,
        encoding,
        line_ending,
        mixed: kinds > 1,
        binary,
        lossy,
    }
}

fn utf8_or_1252(bytes: &[u8]) -> (Encoding, Vec<u16>) {
    match std::str::from_utf8(bytes) {
        Ok(s) if s.is_ascii() => {
            // A plain byte-to-unit widening: the compiler vectorizes it.
            let text = bytes.iter().map(|&b| u16::from(b)).collect();
            (Encoding::Utf8, text)
        }
        Ok(s) => (Encoding::Utf8, s.encode_utf16().collect()),
        Err(_) => {
            let (s, _) = encoding_rs::WINDOWS_1252.decode_without_bom_handling(bytes);
            (Encoding::Windows1252, s.encode_utf16().collect())
        }
    }
}

/// UTF-16 bytes (after the BOM) to units; unpaired surrogates become U+FFFD.
/// The bool says whether any were replaced.
fn utf16(bytes: &[u8], unit: fn([u8; 2]) -> u16) -> (Vec<u16>, bool) {
    let (pairs, rest) = bytes.as_chunks::<2>();
    let mut lossy = !rest.is_empty();
    let mut text: Vec<u16> = pairs.iter().map(|&c| unit(c)).collect();
    let mut i = 0;
    while i < text.len() {
        let u = text[i];
        if (0xD800..0xDC00).contains(&u)
            && text
                .get(i + 1)
                .is_some_and(|n| (0xDC00..0xE000).contains(n))
        {
            i += 2;
            continue;
        }
        if (0xD800..0xE000).contains(&u) {
            text[i] = 0xFFFD;
            lossy = true;
        }
        i += 1;
    }
    if lossy && bytes.len() % 2 == 1 {
        text.push(0xFFFD);
    }
    (text, lossy)
}

/// Turns CRLF and lone CR into LF in place. Returns how many CRLF, lone LF
/// and lone CR there were.
fn normalize(text: &mut Vec<u16>) -> (usize, usize, usize) {
    if !text.contains(&CR) {
        let lf = text.iter().filter(|&&u| u == LF).count();
        return (0, lf, 0);
    }
    let (mut crlf, mut lf, mut cr) = (0, 0, 0);
    let mut w = 0;
    let mut r = 0;
    let n = text.len();
    while r < n {
        let u = text[r];
        r += 1;
        if u == CR {
            if r < n && text[r] == LF {
                r += 1;
                crlf += 1;
            } else {
                cr += 1;
            }
            text[w] = LF;
        } else {
            if u == LF {
                lf += 1;
            }
            text[w] = u;
        }
        w += 1;
    }
    text.truncate(w);
    (crlf, lf, cr)
}

/// Writes text as bytes in the given encoding, with `\n` turned into the
/// line ending. Text with an unpaired surrogate can't be written as UTF-8
/// (UTF-16 writes it as it is), and a character outside Windows-1252 can't
/// be written as Windows-1252: the error says where the first one is.
pub fn encode(
    text: &[u16],
    encoding: Encoding,
    line_ending: LineEnding,
) -> Result<Vec<u8>, Unencodable> {
    let eol: &[u16] = match line_ending {
        LineEnding::Lf => &[LF],
        LineEnding::CrLf => &[CR, LF],
        LineEnding::Cr => &[CR],
    };
    match encoding {
        Encoding::Utf8 | Encoding::Utf8Bom => {
            let mut out = Vec::with_capacity(text.len() + 3);
            if encoding == Encoding::Utf8Bom {
                out.extend_from_slice(&[0xEF, 0xBB, 0xBF]);
            }
            let mut pos = 0;
            let mut buf = [0u8; 4];
            for c in char::decode_utf16(text.iter().copied()) {
                match c {
                    Ok('\n') => {
                        out.extend(eol.iter().map(|&e| e as u8));
                        pos += 1;
                    }
                    Ok(c) => {
                        out.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
                        pos += c.len_utf16();
                    }
                    Err(_) => return Err(Unencodable { utf16_offset: pos }),
                }
            }
            Ok(out)
        }
        Encoding::Utf16Le | Encoding::Utf16Be => {
            let put: fn(u16) -> [u8; 2] = if encoding == Encoding::Utf16Le {
                u16::to_le_bytes
            } else {
                u16::to_be_bytes
            };
            let mut out = Vec::with_capacity(text.len() * 2 + 2);
            out.extend_from_slice(&put(0xFEFF));
            for &u in text {
                if u == LF {
                    for &e in eol {
                        out.extend_from_slice(&put(e));
                    }
                } else {
                    out.extend_from_slice(&put(u));
                }
            }
            Ok(out)
        }
        Encoding::Windows1252 => {
            let table = table_1252();
            let mut out = Vec::with_capacity(text.len());
            for (i, &u) in text.iter().enumerate() {
                if u == LF {
                    out.extend(eol.iter().map(|&e| e as u8));
                } else if u < 0x80 || (0xA0..0x100).contains(&u) {
                    out.push(u as u8);
                } else if let Some(b) = table[0x80..0xA0].iter().position(|&t| t == u) {
                    out.push(0x80 + b as u8);
                } else {
                    return Err(Unencodable { utf16_offset: i });
                }
            }
            Ok(out)
        }
    }
}

/// What each Windows-1252 byte decodes to, from encoding_rs, so reading and
/// writing can't disagree.
fn table_1252() -> &'static [u16; 256] {
    static TABLE: OnceLock<[u16; 256]> = OnceLock::new();
    TABLE.get_or_init(|| {
        let mut t = [0u16; 256];
        for (b, slot) in t.iter_mut().enumerate() {
            let byte = [b as u8];
            let (s, _) = encoding_rs::WINDOWS_1252.decode_without_bom_handling(&byte);
            *slot = s.encode_utf16().next().unwrap_or(0xFFFD);
        }
        t
    })
}

fn stamp_of(meta: &fs::Metadata) -> Stamp {
    Stamp {
        mtime_ns: meta
            .mtime()
            .saturating_mul(1_000_000_000)
            .saturating_add(meta.mtime_nsec()),
        size: meta.size(),
        dev: meta.dev(),
        ino: meta.ino(),
    }
}

/// The stamp of the file at `path` (through symlinks).
pub fn stamp(path: &Path) -> io::Result<Stamp> {
    fs::metadata(path).map(|m| stamp_of(&m))
}

/// Reads and decodes a file. The stamp is taken from the open file, so it
/// belongs to the bytes that were read.
pub fn read(path: &Path) -> io::Result<(Decoded, Stamp)> {
    let mut file = File::open(path)?;
    let meta = file.metadata()?;
    let mut bytes = Vec::with_capacity(usize::try_from(meta.len()).unwrap_or(0));
    file.read_to_end(&mut bytes)?;
    Ok((decode(&bytes), stamp_of(&meta)))
}

/// Follows symlinks to the file that really holds the data. A link to
/// something that doesn't exist resolves to that something, so saving
/// creates it.
fn resolve(path: &Path) -> io::Result<PathBuf> {
    let mut current = path.to_path_buf();
    for _ in 0..40 {
        match fs::symlink_metadata(&current) {
            Ok(m) if m.file_type().is_symlink() => {
                let target = fs::read_link(&current)?;
                current = match current.parent() {
                    Some(dir) => dir.join(target),
                    None => target,
                };
            }
            _ => return Ok(current),
        }
    }
    Err(io::Error::from_raw_os_error(libc::ELOOP))
}

fn cstring(path: &Path) -> io::Result<CString> {
    CString::new(path.as_os_str().as_bytes())
        .map_err(|_| io::Error::from_raw_os_error(libc::EINVAL))
}

/// Removes the temp file unless told to keep it.
struct TempGuard<'a> {
    path: &'a Path,
    keep: bool,
}

impl Drop for TempGuard<'_> {
    fn drop(&mut self) {
        if !self.keep {
            let _ = fs::remove_file(self.path);
        }
    }
}

/// The process's umask, read from /proc (setting it to read it would race
/// with other threads creating files). 022 if that fails.
fn umask() -> u32 {
    fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| {
            s.lines()
                .find_map(|l| l.strip_prefix("Umask:"))
                .and_then(|v| u32::from_str_radix(v.trim(), 8).ok())
        })
        .unwrap_or(0o022)
}

fn random_name() -> u64 {
    use std::sync::atomic::{AtomicU64, Ordering};
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let t = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos() as u64);
    let mut x = t ^ (u64::from(std::process::id()) << 32) ^ COUNTER.fetch_add(1, Ordering::Relaxed);
    // splitmix64 finish, so neighbouring inputs give unrelated names.
    x = (x ^ (x >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    x ^ (x >> 31)
}

/// Copies every extended attribute of `from` onto the open file `to`. What
/// the file system or our privileges don't allow is skipped.
fn copy_xattrs(from: &Path, to: &File) {
    let Ok(from) = cstring(from) else { return };
    // SAFETY: `from` is a valid C string; a null buffer with size 0 asks for the size.
    let size = unsafe { libc::listxattr(from.as_ptr(), std::ptr::null_mut(), 0) };
    if size <= 0 {
        return;
    }
    let mut names = vec![0u8; size as usize];
    // SAFETY: `names` has room for its length in bytes.
    let got = unsafe { libc::listxattr(from.as_ptr(), names.as_mut_ptr().cast(), names.len()) };
    if got <= 0 {
        return;
    }
    names.truncate(got as usize);
    for name in names.split(|&b| b == 0).filter(|n| !n.is_empty()) {
        let Ok(name) = CString::new(name) else {
            continue;
        };
        // SAFETY: valid C strings; a null buffer with size 0 asks for the size.
        let len = unsafe { libc::getxattr(from.as_ptr(), name.as_ptr(), std::ptr::null_mut(), 0) };
        if len < 0 {
            continue;
        }
        let mut value = vec![0u8; len as usize];
        // SAFETY: `value` has room for its length in bytes.
        let got = unsafe {
            libc::getxattr(
                from.as_ptr(),
                name.as_ptr(),
                value.as_mut_ptr().cast(),
                value.len(),
            )
        };
        if got < 0 {
            continue;
        }
        // SAFETY: `to` is open and `value` holds `got` bytes. An error
        // (ENOTSUP, EPERM, ...) just skips this attribute.
        unsafe {
            libc::fsetxattr(
                to.as_raw_fd(),
                name.as_ptr(),
                value.as_ptr().cast(),
                got as usize,
                0,
            )
        };
    }
}

/// Saves `bytes` as the file at `path` without losing the old file if
/// anything goes wrong, and returns the new stamp.
///
/// - Symlinks are followed: the link stays a link and its target is written.
/// - A file we can't write to is refused (`PermissionDenied`), even though
///   replacing it through rename would work in a writable directory.
/// - A file with other hard links is written in place, so the links see it.
/// - Otherwise the bytes go to a temp file next to the target, which keeps
///   the original's mode, owner and extended attributes and then replaces it
///   with one rename.
pub fn save(path: &Path, bytes: &[u8]) -> io::Result<Stamp> {
    let target = resolve(path)?;
    let existing = match fs::metadata(&target) {
        Ok(m) => Some(m),
        Err(e) if e.kind() == io::ErrorKind::NotFound => None,
        Err(e) => return Err(e),
    };
    if let Some(meta) = &existing {
        if !meta.is_file() {
            // Raw errnos, so the app can say why.
            return Err(io::Error::from_raw_os_error(if meta.is_dir() {
                libc::EISDIR
            } else {
                libc::EINVAL
            }));
        }
        let c = cstring(&target)?;
        // SAFETY: `c` is a valid C string.
        if unsafe { libc::access(c.as_ptr(), libc::W_OK) } != 0 {
            return Err(io::Error::from_raw_os_error(libc::EACCES));
        }
        if meta.nlink() > 1 {
            let mut f = OpenOptions::new()
                .write(true)
                .truncate(true)
                .open(&target)?;
            f.write_all(bytes)?;
            f.sync_all()?;
            return stamp(&target);
        }
    }

    let dir = match target.parent() {
        Some(d) if !d.as_os_str().is_empty() => d.to_path_buf(),
        _ => PathBuf::from("."),
    };
    let name = target
        .file_name()
        .ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?;
    let mut attempt = 0;
    let (mut file, temp) = loop {
        let mut tmp_name = b".".to_vec();
        tmp_name.extend_from_slice(name.as_bytes());
        tmp_name.extend_from_slice(format!(".{:016x}.tmp", random_name()).as_bytes());
        let temp = dir.join(OsStr::from_bytes(&tmp_name));
        match OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&temp)
        {
            Ok(f) => break (f, temp),
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists && attempt < 8 => attempt += 1,
            Err(e) => return Err(e),
        }
    };
    let mut guard = TempGuard {
        path: &temp,
        keep: false,
    };

    match &existing {
        Some(meta) => {
            // Owner first: chown clears the setuid bits that chmod then sets.
            // SAFETY: the fd is open.
            let r = unsafe { libc::fchown(file.as_raw_fd(), meta.uid(), meta.gid()) };
            if r == -1 {
                let e = io::Error::last_os_error();
                if e.raw_os_error() != Some(libc::EPERM) {
                    return Err(e);
                }
            }
            file.set_permissions(fs::Permissions::from_mode(meta.mode() & 0o7777))?;
            copy_xattrs(&target, &file);
        }
        None => {
            file.set_permissions(fs::Permissions::from_mode(0o666 & !umask()))?;
        }
    }
    file.write_all(bytes)?;
    file.sync_all()?;
    drop(file);
    fs::rename(&temp, &target)?;
    guard.keep = true;
    if let Ok(d) = File::open(&dir) {
        let _ = d.sync_all();
    }
    stamp(&target)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;

    fn u(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    /// Decodes, and when decode says it is safe, checks the exact round trip.
    fn round(bytes: &[u8]) -> Decoded {
        let d = decode(bytes);
        if !d.mixed && !d.binary && !d.lossy {
            let back = encode(&d.text, d.encoding, d.line_ending).unwrap();
            assert_eq!(back, bytes, "round trip of {bytes:02x?}");
        }
        d
    }

    #[test]
    fn fixtures() {
        let d = round(b"");
        assert_eq!(
            (d.encoding, d.line_ending, d.mixed),
            (Encoding::Utf8, LineEnding::Lf, false)
        );
        assert!(d.text.is_empty());

        let d = round(b"a\nb");
        assert_eq!(d.text, u("a\nb"));

        let d = round(b"a\r\nb\r\n");
        assert_eq!((d.line_ending, d.text), (LineEnding::CrLf, u("a\nb\n")));

        let d = round(b"a\rb\r");
        assert_eq!((d.line_ending, d.text), (LineEnding::Cr, u("a\nb\n")));

        let d = round(b"abc\r");
        assert_eq!((d.line_ending, d.text), (LineEnding::Cr, u("abc\n")));

        let d = decode(b"a\r\r\nb");
        assert!(d.mixed);
        assert_eq!(d.text, u("a\n\nb"));

        let d = round(b"\xEF\xBB\xBFhi\n");
        assert_eq!((d.encoding, d.text), (Encoding::Utf8Bom, u("hi\n")));
        let d = round(b"\xEF\xBB\xBF");
        assert_eq!((d.encoding, d.text.len()), (Encoding::Utf8Bom, 0));

        let d = round(b"\xFF\xFEh\0i\0\n\0");
        assert_eq!((d.encoding, d.text), (Encoding::Utf16Le, u("hi\n")));

        let d = round(b"\xFE\xFF\0h\0i\0\n");
        assert_eq!((d.encoding, d.text), (Encoding::Utf16Be, u("hi\n")));

        let d = round("a\u{1F600}b\n".as_bytes());
        assert_eq!(d.text, u("a\u{1F600}b\n"));
        assert_eq!(d.text.len(), 5);

        let all: Vec<u8> = (0..=255).collect();
        let d = decode(&all);
        assert_eq!(d.encoding, Encoding::Windows1252);
        assert!(d.binary);
        let tricky = b"caf\xE9 \x81\x8D\x8F\x90\x9D \x80\n";
        let d = round(tricky);
        assert_eq!(d.encoding, Encoding::Windows1252);
        assert!(!d.binary);
    }

    #[test]
    fn mixed_and_majority() {
        let d = decode(b"a\r\nb\r\nc\nd");
        assert_eq!((d.line_ending, d.mixed), (LineEnding::CrLf, true));
        let d = decode(b"a\r\nb\n");
        assert_eq!((d.line_ending, d.mixed), (LineEnding::Lf, true));
        let d = decode(b"a\rb\r\nc\r");
        assert_eq!((d.line_ending, d.mixed), (LineEnding::Cr, true));
    }

    #[test]
    fn binary_and_lossy() {
        assert!(decode(b"ab\0cd").binary);
        let mut late = vec![b'a'; BINARY_SCAN];
        late.push(0);
        assert!(!decode(&late).binary);
        assert!(!decode(b"\xFF\xFEa\0\0\0").binary);
        // A lone high surrogate, then a, little endian.
        let d = decode(b"\xFF\xFE\x00\xD8a\0");
        assert!(d.lossy);
        assert_eq!(d.text, vec![0xFFFD, u16::from(b'a')]);
        assert!(decode(b"\xFF\xFEa\0b").lossy);
    }

    #[test]
    fn encode_errors() {
        assert_eq!(
            encode(&u("ab\u{4E2D}"), Encoding::Windows1252, LineEnding::Lf),
            Err(Unencodable { utf16_offset: 2 })
        );
        assert_eq!(
            encode(&u("\u{1F600}"), Encoding::Windows1252, LineEnding::Lf),
            Err(Unencodable { utf16_offset: 0 })
        );
        assert_eq!(
            encode(
                &[0x61, 0x62, 0xD800, 0x63],
                Encoding::Utf8,
                LineEnding::CrLf
            ),
            Err(Unencodable { utf16_offset: 2 })
        );
        assert_eq!(
            encode(&u("a\u{1F600}\n"), Encoding::Utf8, LineEnding::CrLf).unwrap(),
            "a\u{1F600}\r\n".as_bytes()
        );
        assert_eq!(
            encode(&u("a\u{20AC}"), Encoding::Windows1252, LineEnding::Lf).unwrap(),
            b"a\x80"
        );
    }

    #[test]
    fn windows_1252_is_exact() {
        for b in 0..=255u8 {
            // 0xFF after the byte makes UTF-8 invalid, so this is always 1252.
            let bytes = [b'x', b, 0xFF, b'\n'];
            let d = decode(&bytes);
            assert_eq!(d.encoding, Encoding::Windows1252);
            assert_eq!(d.text[1] == LF, b == b'\n' || b == b'\r');
            if b != b'\r' {
                assert_eq!(encode(&d.text, d.encoding, d.line_ending).unwrap(), bytes);
            }
        }
    }

    /// Random bytes from small alphabets that reach every branch: whatever
    /// decode says is safe must round-trip exactly.
    #[test]
    fn property_round_trip() {
        let mut state = 0x2545_F491_4F6C_DD1Du64;
        let mut next = move || {
            state = state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            (state >> 33) as usize
        };
        let alphabets: [&[u8]; 4] = [
            b"ab\n\r ",
            &[
                0x61, 0x0A, 0x0D, 0xC3, 0xA9, 0x81, 0xFF, 0xFE, 0x00, 0xEF, 0xBB, 0xBF,
            ],
            &[0x0A, 0x0D, 0x00, 0x61, 0xD8, 0xDC, 0xFF, 0xFE],
            &[0x61, 0x0A, 0x0D, 0xF0, 0x9F, 0x98, 0x80, 0xE2, 0x82, 0xAC],
        ];
        let prefixes: [&[u8]; 4] = [b"", b"\xEF\xBB\xBF", b"\xFF\xFE", b"\xFE\xFF"];
        let (mut safe, mut kinds) = (0, [0usize; 5]);
        for _ in 0..60_000 {
            let alpha = alphabets[next() % alphabets.len()];
            let mut bytes = prefixes[next() % 4].to_vec();
            for _ in 0..next() % 14 {
                bytes.push(alpha[next() % alpha.len()]);
            }
            let d = decode(&bytes);
            if !d.mixed && !d.binary && !d.lossy {
                safe += 1;
                kinds[d.encoding as usize] += 1;
                assert_eq!(
                    encode(&d.text, d.encoding, d.line_ending).unwrap(),
                    bytes,
                    "{bytes:02x?}"
                );
            }
            assert!(!d.text.contains(&CR));
        }
        assert!(safe > 5_000, "only {safe} safe inputs");
        assert!(kinds.iter().all(|&n| n > 0), "{kinds:?}");
    }

    /// Text to bytes to text, for every encoding that can hold it.
    #[test]
    fn property_text_round_trip() {
        let pieces = [
            "a",
            "\n",
            " ",
            "\u{E9}",
            "\u{20AC}",
            "\u{1F600}",
            "\u{4E2D}",
        ];
        let encodings = [
            Encoding::Utf8,
            Encoding::Utf8Bom,
            Encoding::Utf16Le,
            Encoding::Utf16Be,
            Encoding::Windows1252,
        ];
        let mut state = 7u64;
        let mut next = move || {
            state = state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            (state >> 33) as usize
        };
        for _ in 0..5_000 {
            let mut s = String::new();
            for _ in 0..next() % 12 {
                s.push_str(pieces[next() % pieces.len()]);
            }
            let text = u(&s);
            let le = [LineEnding::Lf, LineEnding::CrLf, LineEnding::Cr][next() % 3];
            for enc in encodings {
                if let Ok(bytes) = encode(&text, enc, le) {
                    let d = decode(&bytes);
                    if enc == Encoding::Windows1252 && std::str::from_utf8(&bytes).is_ok() {
                        // Bytes that happen to be valid UTF-8 are read as UTF-8.
                        assert_eq!(d.encoding, Encoding::Utf8);
                        continue;
                    }
                    assert_eq!(d.text, text);
                    assert_eq!(d.encoding, enc);
                }
            }
        }
    }

    #[test]
    #[ignore = "timing: run with --release -- --ignored --nocapture"]
    fn decode_100_mb() {
        let line = b"The quick brown fox jumps over the lazy dog, again and again.\n";
        let mut bytes = Vec::with_capacity(100 << 20);
        while bytes.len() < 100 << 20 {
            bytes.extend_from_slice(line);
        }
        let t = std::time::Instant::now();
        let d = decode(&bytes);
        let ascii = t.elapsed();
        assert_eq!(d.text.len(), bytes.len());
        // The same with non-ASCII text.
        let mut bytes = Vec::with_capacity(100 << 20);
        while bytes.len() < 100 << 20 {
            bytes.extend_from_slice(
                "Za\u{17C}\u{F3}\u{142}\u{107} na\u{EF}ve caf\u{E9} \u{1F600}\n".as_bytes(),
            );
        }
        let t = std::time::Instant::now();
        let d = decode(&bytes);
        let other = t.elapsed();
        assert!(!d.text.is_empty());
        println!("100 MB decode: ASCII LF {ascii:?}, non-ASCII LF {other:?}");
        assert!(ascii.as_millis() < 1000);
    }

    fn dir() -> tempfile::TempDir {
        tempfile::tempdir().unwrap()
    }

    #[test]
    fn save_new_file() {
        let d = dir();
        let p = d.path().join("new.txt");
        let s = save(&p, b"hello").unwrap();
        assert_eq!(fs::read(&p).unwrap(), b"hello");
        assert_eq!(s.size, 5);
        assert_eq!(s, stamp(&p).unwrap());
        let mode = fs::metadata(&p).unwrap().mode() & 0o777;
        assert_eq!(mode, 0o666 & !umask());
        assert_eq!(fs::read_dir(d.path()).unwrap().count(), 1, "temp left");
        let (dec, st) = read(&p).unwrap();
        assert_eq!(dec.text, u("hello"));
        assert_eq!(st, s);
    }

    #[test]
    fn save_keeps_mode() {
        let d = dir();
        let p = d.path().join("f");
        fs::write(&p, b"old").unwrap();
        fs::set_permissions(&p, fs::Permissions::from_mode(0o640)).unwrap();
        let ino = fs::metadata(&p).unwrap().ino();
        save(&p, b"new content").unwrap();
        assert_eq!(fs::read(&p).unwrap(), b"new content");
        let m = fs::metadata(&p).unwrap();
        assert_eq!(m.mode() & 0o7777, 0o640);
        assert_ne!(m.ino(), ino, "an atomic save replaces the inode");
        assert_eq!(fs::read_dir(d.path()).unwrap().count(), 1);
    }

    #[test]
    fn save_through_symlink() {
        let d = dir();
        let real = d.path().join("real.txt");
        fs::write(&real, b"old").unwrap();
        let link = d.path().join("link.txt");
        symlink(&real, &link).unwrap();
        save(&link, b"via link").unwrap();
        assert!(fs::symlink_metadata(&link).unwrap().is_symlink());
        assert_eq!(fs::read(&real).unwrap(), b"via link");
    }

    #[test]
    fn save_through_relative_symlink() {
        let d = dir();
        fs::create_dir(d.path().join("sub")).unwrap();
        fs::write(d.path().join("sub/real.txt"), b"old").unwrap();
        fs::create_dir(d.path().join("other")).unwrap();
        symlink("../sub/real.txt", d.path().join("other/link")).unwrap();
        let link2 = d.path().join("link2");
        symlink("other/link", &link2).unwrap();
        save(&link2, b"rel").unwrap();
        assert!(fs::symlink_metadata(&link2).unwrap().is_symlink());
        assert_eq!(fs::read(d.path().join("sub/real.txt")).unwrap(), b"rel");
    }

    #[test]
    fn save_dangling_symlink_creates_target() {
        let d = dir();
        let link = d.path().join("link");
        symlink("made.txt", &link).unwrap();
        save(&link, b"x").unwrap();
        assert_eq!(fs::read(d.path().join("made.txt")).unwrap(), b"x");
        assert!(fs::symlink_metadata(&link).unwrap().is_symlink());
    }

    #[test]
    fn save_hard_link_in_place() {
        let d = dir();
        let a = d.path().join("a");
        let b = d.path().join("b");
        fs::write(&a, b"old old old").unwrap();
        fs::hard_link(&a, &b).unwrap();
        let ino = fs::metadata(&a).unwrap().ino();
        save(&a, b"new").unwrap();
        assert_eq!(fs::read(&a).unwrap(), b"new");
        assert_eq!(fs::read(&b).unwrap(), b"new");
        assert_eq!(fs::metadata(&a).unwrap().ino(), ino);
        assert_eq!(fs::metadata(&b).unwrap().ino(), ino);
    }

    #[test]
    fn save_refuses_read_only() {
        // Root passes access(W_OK) whatever the mode, so there is nothing to test.
        // SAFETY: a plain syscall.
        if unsafe { libc::geteuid() } == 0 {
            return;
        }
        let d = dir();
        let p = d.path().join("ro");
        fs::write(&p, b"keep").unwrap();
        fs::set_permissions(&p, fs::Permissions::from_mode(0o444)).unwrap();
        let ino = fs::metadata(&p).unwrap().ino();
        let e = save(&p, b"changed").unwrap_err();
        assert_eq!(e.kind(), io::ErrorKind::PermissionDenied);
        assert_eq!(fs::read(&p).unwrap(), b"keep");
        assert_eq!(fs::metadata(&p).unwrap().ino(), ino);
        assert_eq!(fs::read_dir(d.path()).unwrap().count(), 1);
    }

    #[test]
    fn save_keeps_xattr() {
        let d = dir();
        let p = d.path().join("x");
        fs::write(&p, b"old").unwrap();
        let c = cstring(&p).unwrap();
        let name = c"user.test";
        // SAFETY: valid C strings and a 3-byte value.
        let r = unsafe { libc::setxattr(c.as_ptr(), name.as_ptr(), b"val".as_ptr().cast(), 3, 0) };
        if r != 0 {
            let e = io::Error::last_os_error().raw_os_error();
            assert!(
                matches!(e, Some(libc::ENOTSUP) | Some(libc::EPERM)),
                "{e:?}"
            );
            return;
        }
        save(&p, b"new").unwrap();
        let mut buf = [0u8; 16];
        // SAFETY: valid C strings; `buf` has 16 bytes.
        let n = unsafe { libc::getxattr(c.as_ptr(), name.as_ptr(), buf.as_mut_ptr().cast(), 16) };
        assert_eq!(&buf[..n.max(0) as usize], b"val");
    }

    #[test]
    fn save_fails_cleanly() {
        let d = dir();
        assert!(save(&d.path().join("missing-dir/f"), b"x").is_err());
        assert_eq!(fs::read_dir(d.path()).unwrap().count(), 0);
        assert!(save(d.path(), b"x").is_err());
    }
}
