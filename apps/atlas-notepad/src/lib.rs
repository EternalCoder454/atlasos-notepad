//! Notepad, Rust side. `cpp/` holds the Qt glue; the editor's highlighter
//! calls in here once per line through the plain C functions below.

use std::cell::RefCell;
use std::ffi::{CStr, OsStr, c_char};
use std::os::unix::ffi::OsStrExt;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;

use notepad_core::file;
use notepad_core::markdown::{self, Line, Run};

mod crash;

thread_local! {
    static BUFFERS: RefCell<(Vec<u32>, Vec<Run>)> = const { RefCell::new((Vec::new(), Vec::new())) };
}

/// Reads one line of Markdown for the highlighter (cpp/markdown.h). `text` is
/// the line in UTF-16, `state` the previous line's `Line::state` (0 for the
/// first). Writes up to `cap` runs and the line's summary, and returns how
/// many runs there are: when that is more than `cap`, call again with room.
///
/// # Safety
///
/// `text` points to `len` UTF-16 units (or `len` is 0), `runs` to room for
/// `cap` runs (or `cap` is 0), and `line` to a writable `Line`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn np_md_line(
    text: *const u16,
    len: usize,
    state: i32,
    runs: *mut Run,
    cap: usize,
    line: *mut Line,
) -> usize {
    if line.is_null() {
        return 0;
    }
    let cap = if runs.is_null() { 0 } else { cap };
    // SAFETY: the caller passes `len` readable units.
    let Some(text) = (unsafe { slice(text, len) }) else {
        // SAFETY: the caller passes a writable Line.
        unsafe { line.write(Line::default()) };
        return 0;
    };
    // A panic must not cross into C++ (it would abort with the unsaved tabs).
    let parsed = catch_unwind(AssertUnwindSafe(|| {
        BUFFERS.with(|buffers| {
            let Ok(mut buffers) = buffers.try_borrow_mut() else {
                return None;
            };
            let (flags, found) = &mut *buffers;
            let summary = markdown::parse_line(text, state, flags);
            markdown::runs(flags, found);
            let n = found.len().min(cap);
            if n > 0 {
                // SAFETY: the caller gives room for `cap` runs; n <= cap.
                unsafe { std::ptr::copy_nonoverlapping(found.as_ptr(), runs, n) };
            }
            Some((summary, found.len()))
        })
    }));
    let (summary, n) = match parsed {
        Ok(Some(found)) => found,
        _ => (Line::default(), 0),
    };
    // SAFETY: the caller passes a writable Line.
    unsafe { line.write(summary) };
    n
}

/// The link at UTF-16 position `pos` of a line: writes where its URL is and
/// returns true, or returns false.
///
/// # Safety
///
/// `text` points to `len` UTF-16 units (or `len` is 0); `start` and `end` are
/// writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn np_md_link_at(
    text: *const u16,
    len: usize,
    pos: usize,
    start: *mut usize,
    end: *mut usize,
) -> bool {
    if start.is_null() || end.is_null() {
        return false;
    }
    // SAFETY: as documented.
    let Some(text) = (unsafe { slice(text, len) }) else {
        return false;
    };
    match catch_unwind(|| markdown::link_at(text, pos)).ok().flatten() {
        Some((a, b)) => {
            // SAFETY: the caller passes writable pointers.
            unsafe {
                start.write(a);
                end.write(b);
            }
            true
        }
        None => false,
    }
}

/// A file's stamp (cpp/notepad_core.h).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NpStamp {
    pub mtime_ns: i64,
    pub size: u64,
    pub dev: u64,
    pub ino: u64,
}

impl From<file::Stamp> for NpStamp {
    fn from(s: file::Stamp) -> Self {
        Self {
            mtime_ns: s.mtime_ns,
            size: s.size,
            dev: s.dev,
            ino: s.ino,
        }
    }
}

/// A file read by `np_file_read`: its text in UTF-16 with `\n` line breaks,
/// and how it was stored. `error` is 0 or an errno, and then `text` is null.
#[repr(C)]
pub struct NpFile {
    pub text: *mut u16,
    pub len: usize,
    pub encoding: u8,
    pub line_ending: u8,
    pub mixed: u8,
    pub binary: u8,
    pub lossy: u8,
    pub stamp: NpStamp,
    pub error: i32,
}

const EIO: i32 = 5;
const EINVAL: i32 = 22;

/// What the C side sees of an I/O error: its errno, or EIO.
fn errno(e: &std::io::Error) -> i32 {
    e.raw_os_error().unwrap_or(EIO)
}

fn failed(error: i32) -> *mut NpFile {
    Box::into_raw(Box::new(NpFile {
        text: std::ptr::null_mut(),
        len: 0,
        encoding: 0,
        line_ending: 0,
        mixed: 0,
        binary: 0,
        lossy: 0,
        stamp: NpStamp::default(),
        error,
    }))
}

/// # Safety
///
/// `path` is a NUL-terminated string.
unsafe fn path_arg(path: *const c_char) -> Option<PathBuf> {
    if path.is_null() {
        return None;
    }
    // SAFETY: as documented.
    let bytes = unsafe { CStr::from_ptr(path) }.to_bytes();
    Some(PathBuf::from(OsStr::from_bytes(bytes)))
}

/// Reads and decodes a regular file of at most `max_bytes`. Never returns
/// null: on failure `error` is the errno (`EFBIG` past the limit, `EINVAL`
/// for a FIFO, device or directory). Free the result with `np_file_free`.
///
/// # Safety
///
/// `path` is a NUL-terminated string (UTF-8, or any bytes the file system
/// takes).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn np_file_read(path: *const c_char, max_bytes: u64) -> *mut NpFile {
    // SAFETY: as documented.
    let Some(path) = (unsafe { path_arg(path) }) else {
        return failed(EINVAL);
    };
    let read = catch_unwind(|| file::read(&path, max_bytes));
    match read {
        Ok(Ok((d, stamp))) => {
            let len = d.text.len();
            let text = Box::into_raw(d.text.into_boxed_slice()).cast::<u16>();
            Box::into_raw(Box::new(NpFile {
                text,
                len,
                encoding: d.encoding as u8,
                line_ending: d.line_ending as u8,
                mixed: d.mixed.into(),
                binary: d.binary.into(),
                lossy: d.lossy.into(),
                stamp: stamp.into(),
                error: 0,
            }))
        }
        Ok(Err(e)) => failed(errno(&e)),
        Err(_) => failed(EIO),
    }
}

/// Frees what `np_file_read` returned. Null is fine.
///
/// # Safety
///
/// `file` came from `np_file_read` and isn't used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn np_file_free(file: *mut NpFile) {
    if file.is_null() {
        return;
    }
    // SAFETY: as documented.
    let file = unsafe { Box::from_raw(file) };
    if !file.text.is_null() {
        // SAFETY: `text` and `len` came from a boxed slice in np_file_read.
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(file.text, file.len)) });
    }
}

/// Saves text as a file, in the given encoding and line ending, without
/// losing the old file on failure. Returns 0 and sets `*stamp`; or an errno;
/// or -1 when the text can't be written in that encoding, with `*bad_offset`
/// set to the first such position in UTF-16 units.
///
/// # Safety
///
/// `path` is a NUL-terminated string, `text` points to `len` UTF-16 units (or
/// `len` is 0), and `stamp` and `bad_offset` are writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn np_file_save(
    path: *const c_char,
    text: *const u16,
    len: usize,
    encoding: u8,
    line_ending: u8,
    stamp: *mut NpStamp,
    bad_offset: *mut usize,
) -> i32 {
    // SAFETY: as documented.
    let Some(path) = (unsafe { path_arg(path) }) else {
        return EINVAL;
    };
    if stamp.is_null() || bad_offset.is_null() {
        return EINVAL;
    }
    // SAFETY: as documented. Null text with a length would save an empty file.
    let Some(text) = (unsafe { slice(text, len) }) else {
        return EINVAL;
    };
    let (Some(encoding), Some(line_ending)) = (
        file::Encoding::from_u8(encoding),
        file::LineEnding::from_u8(line_ending),
    ) else {
        return EINVAL;
    };
    let saved = catch_unwind(|| match file::encode(text, encoding, line_ending) {
        Err(bad) => Err(Err(bad.utf16_offset)),
        Ok(bytes) => file::save(&path, &bytes).map_err(Ok),
    });
    match saved {
        Ok(Ok(s)) => {
            // SAFETY: the caller passes a writable stamp.
            unsafe { stamp.write(s.into()) };
            0
        }
        Ok(Err(Ok(e))) => errno(&e),
        Ok(Err(Err(offset))) => {
            // SAFETY: the caller passes a writable offset.
            unsafe { bad_offset.write(offset) };
            -1
        }
        Err(_) => EIO,
    }
}

/// Saves `len` bytes as a file only we may read (the session's): 0 or an
/// errno. See `file::save_private`.
///
/// # Safety
///
/// `path` is a NUL-terminated string and `bytes` points to `len` bytes (or
/// `len` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn np_file_save_private(
    path: *const c_char,
    bytes: *const u8,
    len: usize,
) -> i32 {
    // SAFETY: as documented.
    let Some(path) = (unsafe { path_arg(path) }) else {
        return EINVAL;
    };
    let bytes = if len == 0 {
        &[][..]
    } else if bytes.is_null() {
        return EINVAL;
    } else {
        // SAFETY: as documented.
        unsafe { std::slice::from_raw_parts(bytes, len) }
    };
    match catch_unwind(|| file::save_private(&path, bytes)) {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => errno(&e),
        Err(_) => EIO,
    }
}

/// The stamp of a file: 0 and `*stamp` set, or an errno.
///
/// # Safety
///
/// `path` is a NUL-terminated string and `stamp` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn np_file_stamp(path: *const c_char, stamp: *mut NpStamp) -> i32 {
    // SAFETY: as documented.
    let Some(path) = (unsafe { path_arg(path) }) else {
        return EINVAL;
    };
    if stamp.is_null() {
        return EINVAL;
    }
    match catch_unwind(|| file::stamp(&path)) {
        Ok(Ok(s)) => {
            // SAFETY: the caller passes a writable stamp.
            unsafe { stamp.write(s.into()) };
            0
        }
        Ok(Err(e)) => errno(&e),
        Err(_) => EIO,
    }
}

/// # Safety
///
/// `p` points to `len` readable units, or `len` is 0. None for a null `p`
/// with a length.
unsafe fn slice<'a>(p: *const u16, len: usize) -> Option<&'a [u16]> {
    if len == 0 {
        Some(&[])
    } else if p.is_null() {
        None
    } else {
        // SAFETY: as documented.
        Some(unsafe { std::slice::from_raw_parts(p, len) })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn retries_with_room() {
        let text: Vec<u16> = "**a** *b* `c`".encode_utf16().collect();
        let mut line = Line::default();
        // SAFETY: valid pointers, cap 0.
        let n = unsafe {
            np_md_line(
                text.as_ptr(),
                text.len(),
                0,
                std::ptr::null_mut(),
                0,
                &mut line,
            )
        };
        assert!(n > 0);
        let mut runs = vec![Run::default(); n];
        // SAFETY: room for n runs.
        let again = unsafe {
            np_md_line(
                text.as_ptr(),
                text.len(),
                0,
                runs.as_mut_ptr(),
                n,
                &mut line,
            )
        };
        assert_eq!(again, n);
        assert!(runs.iter().all(|r| r.len > 0));
    }

    #[test]
    fn file_round_trip() {
        let dir = std::env::temp_dir().join(format!("np-abi-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("a.txt");
        let cpath = std::ffi::CString::new(path.to_str().unwrap()).unwrap();
        let text: Vec<u16> = "h\u{e9}llo\n\u{1F600}\n".encode_utf16().collect();
        let mut stamp = NpStamp::default();
        let mut bad = 0usize;
        // SAFETY: valid C string, text and out pointers.
        let r = unsafe {
            np_file_save(
                cpath.as_ptr(),
                text.as_ptr(),
                text.len(),
                2,
                1,
                &mut stamp,
                &mut bad,
            )
        };
        assert_eq!(r, 0);
        assert_eq!(std::fs::read(&path).unwrap()[..2], [0xFF, 0xFE]);

        let mut again = NpStamp::default();
        // SAFETY: valid C string and out pointer.
        assert_eq!(unsafe { np_file_stamp(cpath.as_ptr(), &mut again) }, 0);
        assert_eq!((again.size, again.ino), (stamp.size, stamp.ino));

        // SAFETY: valid C string; the result is freed once.
        unsafe {
            let f = np_file_read(cpath.as_ptr(), u64::MAX);
            assert_eq!((*f).error, 0);
            assert_eq!(std::slice::from_raw_parts((*f).text, (*f).len), &text[..]);
            assert_eq!(((*f).encoding, (*f).line_ending, (*f).mixed), (2, 1, 0));
            assert_eq!((*f).stamp.size, stamp.size);
            np_file_free(f);
        }

        // Windows-1252 can't hold the emoji: the offset is reported.
        // SAFETY: as above.
        let r = unsafe {
            np_file_save(
                cpath.as_ptr(),
                text.as_ptr(),
                text.len(),
                4,
                0,
                &mut stamp,
                &mut bad,
            )
        };
        assert_eq!((r, bad), (-1, 6));

        let missing = std::ffi::CString::new(dir.join("none").to_str().unwrap()).unwrap();
        // SAFETY: valid C string; the result is freed once.
        unsafe {
            let f = np_file_read(missing.as_ptr(), u64::MAX);
            assert_eq!((*f).error, 2);
            assert!((*f).text.is_null());
            np_file_free(f);
            assert_eq!(np_file_stamp(missing.as_ptr(), &mut again), 2);
        }
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
