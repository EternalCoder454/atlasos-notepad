//! Notepad's logic that doesn't need Qt: reading Markdown for the editor,
//! and (later) files, encodings and the session.

pub mod file;
pub mod markdown;

/// Property tests: `cargo test -- props` (CI runs them with 20,000 cases).
#[cfg(test)]
mod props;
