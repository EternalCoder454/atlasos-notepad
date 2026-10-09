//! Property tests for the C ABI the C++ side calls (docs/SECURITY.md,
//! "Tests"): whatever pointers, lengths, enum numbers and capacities the other
//! side passes, the Rust side returns an error or an answer inside the
//! buffers it was given, and never panics across the boundary. Named
//! `props::...` so `cargo test -- props` runs them (CI: 20,000 cases).

use std::ffi::CString;

use notepad_core::markdown::{Line, Run};
use proptest::collection::vec;
use proptest::prelude::*;

use crate::*;

fn units() -> impl Strategy<Value = Vec<u16>> {
    prop_oneof![
        vec(any::<u16>(), 0..256),
        any::<String>().prop_map(|s| s.encode_utf16().collect()),
    ]
}

proptest! {
    /// `np_md_line` with any capacity (0 and a null buffer included): it
    /// reports the number of runs, writes at most `cap` of them, all inside
    /// the line, and the line summary is written.
    #[test]
    fn md_line_respects_the_buffer_it_was_given(
        text in units(),
        state in any::<i32>(),
        cap in 0usize..16,
        null_runs in any::<bool>(),
    ) {
        // A canary after the `cap` runs the caller gave: it must stay.
        let canary = Run { start: 0xDEAD, len: 0xBEEF, flags: 0xF00D };
        let mut buf = vec![canary; cap + 4];
        let mut line = Line::default();
        // SAFETY: `text` has `len` units, `buf` room for `cap` runs, `line` is writable.
        let n = unsafe {
            np_md_line(
                text.as_ptr(),
                text.len(),
                state,
                if null_runs { std::ptr::null_mut() } else { buf.as_mut_ptr() },
                cap,
                &mut line,
            )
        };
        for r in &buf[cap..] {
            prop_assert_eq!(*r, canary, "a write past `cap`");
        }
        let written = if null_runs { 0 } else { n.min(cap) };
        for r in &buf[..written] {
            prop_assert!(r.len > 0 && (r.start as usize + r.len as usize) <= text.len());
        }
        if null_runs {
            prop_assert!(buf.iter().all(|r| *r == canary));
        }
        prop_assert!(line.content_start as usize <= text.len());
    }

    /// `np_md_link_at`: a range inside the line, or false; null out pointers
    /// are refused.
    #[test]
    fn md_link_at_is_inside_the_line(text in units(), pos in 0usize..300) {
        let (mut a, mut b) = (usize::MAX, usize::MAX);
        // SAFETY: `text` has `len` units; `a` and `b` are writable.
        let found = unsafe { np_md_link_at(text.as_ptr(), text.len(), pos, &mut a, &mut b) };
        if found {
            prop_assert!(a <= b && b <= text.len());
        } else {
            prop_assert_eq!((a, b), (usize::MAX, usize::MAX));
        }
        // SAFETY: null out pointers are part of the contract.
        let refused =
            unsafe { np_md_link_at(text.as_ptr(), text.len(), pos, std::ptr::null_mut(), &mut b) };
        prop_assert!(!refused);
    }

    /// Bytes through `np_file_decode` and the text back through
    /// `np_file_encode`: no error for what was just decoded, the enum numbers
    /// the other side makes up are refused, and nothing leaks (Miri-free: each
    /// result is freed once).
    #[test]
    fn decode_encode_through_the_abi(
        bytes in vec(any::<u8>(), 0..512),
        enc in any::<u8>(),
        eol in any::<u8>(),
    ) {
        // SAFETY: `bytes` has `len` bytes; each result is freed once.
        unsafe {
            let f = np_file_decode(bytes.as_ptr(), bytes.len());
            prop_assert_eq!((*f).error, 0);
            prop_assert!(!(*f).text.is_null() || (*f).len == 0);
            prop_assert!((*f).encoding <= 4 && (*f).line_ending <= 2);
            let text = if (*f).len == 0 {
                Vec::new()
            } else {
                std::slice::from_raw_parts((*f).text, (*f).len).to_vec()
            };
            let (e0, l0) = ((*f).encoding, (*f).line_ending);
            np_file_free(f);

            let b = np_file_encode(text.as_ptr(), text.len(), e0, l0);
            prop_assert_eq!((*b).error, 0);
            np_bytes_free(b);

            // Any enum numbers: an answer for the known ones, EINVAL for the rest.
            let b = np_file_encode(text.as_ptr(), text.len(), enc, eol);
            if enc > 4 || eol > 2 {
                prop_assert_eq!((*b).error, EINVAL);
                prop_assert!((*b).data.is_null());
            } else {
                prop_assert!((*b).error == 0 || (*b).error == -1);
            }
            np_bytes_free(b);
        }
    }

    /// Paths that are not files or not paths: an errno in the result, never a
    /// panic, never a null result.
    #[test]
    fn read_of_any_path_is_an_error_or_a_file(name in "[^\\x00]{0,64}") {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(name.trim_start_matches('/'));
        let c = CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
        // SAFETY: a valid C string; the result is freed once.
        unsafe {
            let f = np_file_read(c.as_ptr(), 1 << 20);
            prop_assert!(!f.is_null());
            prop_assert!((*f).error != 0 || !(*f).text.is_null() || (*f).len == 0);
            np_file_free(f);
        }
    }
}

/// Null and nonsense arguments, one by one.
#[test]
fn abi_refuses_nulls_and_unknown_numbers() {
    let mut stamp = NpStamp::default();
    let mut bad = 0usize;
    let text: Vec<u16> = "x".encode_utf16().collect();
    let path = CString::new("/nonexistent-dir-for-the-test/f").unwrap();
    // SAFETY: every call passes pointers the contract allows to be null, or valid ones.
    unsafe {
        assert_eq!(np_file_stamp(std::ptr::null(), &mut stamp), EINVAL);
        assert_eq!(np_file_stamp(path.as_ptr(), std::ptr::null_mut()), EINVAL);
        let f = np_file_read(std::ptr::null(), 10);
        assert_eq!((*f).error, EINVAL);
        np_file_free(f);
        np_file_free(std::ptr::null_mut());
        np_bytes_free(std::ptr::null_mut());
        assert_eq!(
            np_file_save(
                std::ptr::null(),
                text.as_ptr(),
                1,
                0,
                0,
                &mut stamp,
                &mut bad
            ),
            EINVAL
        );
        assert_eq!(
            np_file_save(
                path.as_ptr(),
                std::ptr::null(),
                1,
                0,
                0,
                &mut stamp,
                &mut bad
            ),
            EINVAL
        );
        assert_eq!(
            np_file_save(path.as_ptr(), text.as_ptr(), 1, 9, 0, &mut stamp, &mut bad),
            EINVAL
        );
        assert_eq!(
            np_file_save(path.as_ptr(), text.as_ptr(), 1, 0, 9, &mut stamp, &mut bad),
            EINVAL
        );
        assert_eq!(
            np_file_save(
                path.as_ptr(),
                text.as_ptr(),
                1,
                0,
                0,
                std::ptr::null_mut(),
                &mut bad
            ),
            EINVAL
        );
        assert_eq!(
            np_file_save_private(std::ptr::null(), b"x".as_ptr(), 1),
            EINVAL
        );
        assert_eq!(
            np_file_save_private(path.as_ptr(), std::ptr::null(), 1),
            EINVAL
        );
        // A folder that isn't there: an errno, not a panic.
        assert_ne!(np_file_save_private(path.as_ptr(), b"x".as_ptr(), 1), 0);
        assert_ne!(
            np_file_save(path.as_ptr(), text.as_ptr(), 1, 0, 0, &mut stamp, &mut bad),
            0
        );
    }
}
