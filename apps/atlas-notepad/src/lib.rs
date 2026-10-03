//! Notepad, Rust side. `cpp/` holds the Qt glue; the editor's highlighter
//! calls in here once per line through the plain C functions below.

use std::cell::RefCell;

use notepad_core::markdown::{self, Line, Run};

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
    // SAFETY: the caller passes `len` readable units.
    let text = unsafe { slice(text, len) };
    BUFFERS.with_borrow_mut(|(flags, found)| {
        let summary = markdown::parse_line(text, state, flags);
        markdown::runs(flags, found);
        let n = found.len().min(cap);
        if n > 0 {
            // SAFETY: the caller gives room for `cap` runs; n <= cap.
            unsafe { std::ptr::copy_nonoverlapping(found.as_ptr(), runs, n) };
        }
        // SAFETY: the caller passes a writable Line.
        unsafe { line.write(summary) };
        found.len()
    })
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
    // SAFETY: as documented.
    let text = unsafe { slice(text, len) };
    match markdown::link_at(text, pos) {
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

/// # Safety
///
/// `p` points to `len` readable units, or `len` is 0.
unsafe fn slice<'a>(p: *const u16, len: usize) -> &'a [u16] {
    if len == 0 || p.is_null() {
        &[]
    } else {
        // SAFETY: as documented.
        unsafe { std::slice::from_raw_parts(p, len) }
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
}
