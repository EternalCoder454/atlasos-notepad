//! Opt-in crash reports through atlas-framework, as in the other Atlas apps.
//! Off unless the user turned them on in Telamon Updater, which is also where
//! reports are reviewed and sent (as public GitHub issues); Notepad only
//! saves them.

use std::ffi::{CStr, c_char};

use telamon_framework_core::{AppInfo, app_info};
use telamon_framework_system::crash;

pub const APP_ID: &str = "net.eterneon.telamon.notepad";
pub const REPO: &str = "atlasos-notepad";

pub fn app_info() -> AppInfo {
    app_info! { name: "Notepad", id: APP_ID, repo: REPO }
}

/// Called first thing from `main.cpp`: panics save a report, but only when
/// the user enabled crash reports (atlas-framework checks the setting).
#[unsafe(no_mangle)]
pub extern "C" fn telamon_crash_install() {
    crash::install(app_info());
}

/// Called from the C++ Qt message handler on `QtFatalMsg`, before abort.
///
/// # Safety
/// `msg` must be null or a valid NUL-terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn telamon_crash_fatal(msg: *const c_char) {
    let text = if msg.is_null() {
        String::from("Qt fatal message")
    } else {
        // SAFETY: the caller passes a NUL-terminated string.
        unsafe { CStr::from_ptr(msg) }
            .to_string_lossy()
            .into_owned()
    };
    let _ = crash::record_fatal(&text);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn app_info_names_this_app() {
        let a = app_info();
        assert_eq!(a.id, "net.eterneon.telamon.notepad");
        assert_eq!(a.repo, "atlasos-notepad");
        assert!(!a.version.is_empty());
    }

    #[test]
    fn crash_state_follows_the_rename() {
        // The framework names the crash settings after these, and reads
        // the 1.x names while the new ones are absent.
        let a = app_info();
        assert_eq!(a.short_name(), "telamon-notepad");
        assert_eq!(a.legacy_short_name(), "atlas-notepad");
    }
}
