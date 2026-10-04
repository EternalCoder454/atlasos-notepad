//! Markdown, one line at a time, for the editor's highlighter.
//!
//! The editor keeps the file's Markdown as its text, and draws it formatted:
//! the highlighter asks this module what each character of a line is, and
//! picks a format for it. A fenced code block is the only thing that spans
//! lines, so the state carried from one line to the next is one number.
//!
//! Lines are UTF-16, as Qt keeps them, and every position here counts UTF-16
//! units, so the highlighter can pass a block's text straight through.
//!
//! What is read: ATX headings, block quotes, bullets, numbered items, task
//! items, rules, fenced code; inline code, emphasis (`*`, `_`), strikethrough
//! (`~~`), links, autolinks, bare URLs and backslash escapes. Tables, HTML,
//! setext headings and indented code are left as text.

/// What a character is. A character can be several (bold inside a heading).
pub mod flags {
    /// Markdown syntax that takes no room in the Formatted view (`**`, `# `).
    pub const HIDDEN: u32 = 1;
    /// A block's prefix that keeps its width but isn't drawn as text in the
    /// Formatted view: the editor draws a bullet, a checkbox or a bar there.
    pub const PAD: u32 = 1 << 1;
    pub const STRONG: u32 = 1 << 2;
    pub const EMPH: u32 = 1 << 3;
    pub const STRIKE: u32 = 1 << 4;
    /// Inline code.
    pub const CODE: u32 = 1 << 5;
    /// A link's text, an autolink or a bare URL.
    pub const LINK: u32 = 1 << 6;
    /// A heading's text; the level is in [`Line::heading`].
    pub const HEADING: u32 = 1 << 7;
    /// Text inside a block quote.
    pub const QUOTE: u32 = 1 << 8;
    /// A line inside a fenced code block.
    pub const CODE_BLOCK: u32 = 1 << 9;
    /// A fence line (```` ``` ````), drawn small and dim in the Formatted view.
    pub const FENCE: u32 = 1 << 10;
    /// A thematic break (`---`), drawn as a line in the Formatted view.
    pub const RULE: u32 = 1 << 11;
    /// The number of a numbered item, with its `.` or `)`.
    pub const LIST_NUMBER: u32 = 1 << 12;
    /// The text of a checked task item.
    pub const DONE: u32 = 1 << 13;
}

/// A stretch of characters with the same flags. Only stretches with some flag
/// are listed.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Run {
    pub start: u32,
    pub len: u32,
    pub flags: u32,
}

/// What kind of line this is, for the drawing beside and behind the text.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Kind {
    #[default]
    Blank = 0,
    Paragraph = 1,
    Heading = 2,
    Bullet = 3,
    Numbered = 4,
    Task = 5,
    Fence = 6,
    Code = 7,
    Rule = 8,
}

/// The line as a whole. C++ reads it as a plain struct.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Line {
    pub kind: Kind,
    /// 1-6 for a heading, else 0.
    pub heading: u8,
    /// How many `>` the line starts with.
    pub quote_depth: u8,
    /// A task item's box is ticked.
    pub checked: u8,
    /// Where the bullet or the task's `[ ]` is (for drawing over it).
    pub marker_start: u32,
    pub marker_len: u32,
    /// Where the text starts after the line's prefixes.
    pub content_start: u32,
    /// For the next line: 0 outside a fenced code block.
    pub state: i32,
}

/// Reads one line. `state` is the previous line's [`Line::state`] (0 for the
/// first line). `flags` is filled with one entry per UTF-16 unit; reuse it
/// between calls to save the allocation.
pub fn parse_line(text: &[u16], state: i32, flags: &mut Vec<u32>) -> Line {
    flags.clear();
    flags.resize(text.len(), 0);
    let n = text.len();
    let mut line = Line::default();

    // Any indent: lists nest by it, and indented code (4+ spaces) isn't read.
    let mut i = skip_spaces(text, 0, usize::MAX);

    // Inside a fenced code block: only the closing fence ends it.
    if state > 0 {
        let (fence_char, fence_len) = decode_fence(state);
        let run = run_len(text, i, fence_char);
        if run >= fence_len && is_blank(&text[i + run..]) {
            set(flags, 0, n, flags::FENCE);
            line.kind = Kind::Fence;
            return line;
        }
        set(flags, 0, n, flags::CODE_BLOCK);
        line.kind = Kind::Code;
        line.state = state;
        return line;
    }

    if is_blank(text) {
        return line;
    }

    // An opening fence.
    for fence_char in [b'`' as u16, b'~' as u16] {
        let run = run_len(text, i, fence_char);
        if run >= 3 && (fence_char == b'~' as u16 || !text[i + run..].contains(&fence_char)) {
            set(flags, 0, n, flags::FENCE);
            line.kind = Kind::Fence;
            line.state = encode_fence(fence_char, run);
            return line;
        }
    }

    // Block quotes, any depth: `> > text`.
    let mut base = 0;
    while i < n && text[i] == b'>' as u16 {
        let end = if i + 1 < n && text[i + 1] == b' ' as u16 {
            i + 2
        } else {
            i + 1
        };
        set(flags, i, end, flags::PAD);
        line.quote_depth = line.quote_depth.saturating_add(1);
        base = flags::QUOTE;
        i = skip_spaces(text, end, usize::MAX);
    }

    line.kind = Kind::Paragraph;

    if let Some(level) = heading_level(text, i) {
        let level_end = i + level as usize;
        let content = skip_spaces(text, level_end, usize::MAX);
        set(flags, i, content, flags::HIDDEN);
        // An optional closing run of #, after a space: "## Title ##".
        let mut end = n;
        while end > content && is_space(text[end - 1]) {
            end -= 1;
        }
        let mut close = end;
        while close > content && text[close - 1] == b'#' as u16 {
            close -= 1;
        }
        if close < end && (close == content || is_space(text[close - 1])) {
            let mut trail = close;
            while trail > content && is_space(text[trail - 1]) {
                trail -= 1;
            }
            set(flags, trail, n, flags::HIDDEN);
            end = trail;
        }
        line.kind = Kind::Heading;
        line.heading = level;
        line.content_start = content as u32;
        inline(text, content, end, base | flags::HEADING, flags);
        return line;
    }

    if is_rule(&text[i..]) {
        set(flags, i, n, flags::RULE | base);
        line.kind = Kind::Rule;
        line.content_start = n as u32;
        return line;
    }

    let mut content = i;
    // A bullet needs its space too, or "-5" would become a bullet while typed.
    if i + 1 < n && is_bullet(text[i]) && is_space(text[i + 1]) {
        let after = (i + 2).min(n);
        line.kind = Kind::Bullet;
        line.marker_start = i as u32;
        line.marker_len = 1;
        content = after;
        // A task item: "- [ ] text" or "- [x] text".
        if let Some(checked) = task_box(text, after) {
            let box_end = (after + 4).min(n);
            set(flags, i, after, flags::HIDDEN);
            set(flags, after, box_end, flags::PAD);
            line.kind = Kind::Task;
            line.checked = checked as u8;
            line.marker_start = after as u32;
            line.marker_len = 3;
            content = box_end;
            if checked {
                base |= flags::DONE;
            }
        } else {
            set(flags, i, after, flags::PAD);
        }
    } else if let Some(end) = number_marker(text, i) {
        set(flags, i, end, flags::LIST_NUMBER);
        line.kind = Kind::Numbered;
        line.marker_start = i as u32;
        line.marker_len = (end - i) as u32;
        content = (end + 1).min(n);
    }

    line.content_start = content as u32;
    inline(text, content, n, base, flags);
    line
}

/// Turns per-character flags into runs, leaving out characters with none.
pub fn runs(flags: &[u32], out: &mut Vec<Run>) {
    out.clear();
    let mut i = 0;
    while i < flags.len() {
        let f = flags[i];
        let start = i;
        while i < flags.len() && flags[i] == f {
            i += 1;
        }
        if f != 0 {
            out.push(Run {
                start: start as u32,
                len: (i - start) as u32,
                flags: f,
            });
        }
    }
}

/// Inline syntax in `text[start..end]`; every character there gets `base` too.
fn inline(text: &[u16], start: usize, end: usize, base: u32, flags: &mut [u32]) {
    if start >= end {
        return;
    }
    if base != 0 {
        for f in &mut flags[start..end] {
            *f |= base;
        }
    }
    // Characters that can't be emphasis delimiters: escapes, code spans,
    // link syntax, URLs.
    let mut opaque = vec![false; end - start];
    let op = |o: &mut Vec<bool>, a: usize, b: usize| {
        for x in &mut o[a - start..b - start] {
            *x = true;
        }
    };

    let mut j = start;
    while j < end {
        if opaque[j - start] {
            j += 1;
            continue;
        }
        let c = text[j];
        if c == b'\\' as u16 && j + 1 < end && is_ascii_punct(text[j + 1]) {
            flags[j] |= flags::HIDDEN;
            op(&mut opaque, j, j + 2);
            j += 2;
        } else if c == b'`' as u16 {
            let r = run_len(text, j, c).min(end - j);
            match find_backtick_run(text, j + r, end, r) {
                Some(k) => {
                    set(flags, j, j + r, flags::HIDDEN);
                    set(flags, k, k + r, flags::HIDDEN);
                    set(flags, j + r, k, flags::CODE);
                    op(&mut opaque, j, k + r);
                    j = k + r;
                }
                None => {
                    op(&mut opaque, j, j + r);
                    j += r;
                }
            }
        } else if c == b'<' as u16 {
            match autolink_end(text, j + 1, end) {
                Some(k) => {
                    flags[j] |= flags::HIDDEN;
                    flags[k] |= flags::HIDDEN;
                    set(flags, j + 1, k, flags::LINK);
                    op(&mut opaque, j, k + 1);
                    j = k + 1;
                }
                None => j += 1,
            }
        } else if c == b'[' as u16
            || (c == b'!' as u16 && j + 1 < end && text[j + 1] == b'[' as u16)
        {
            let open = if c == b'!' as u16 { j + 1 } else { j };
            match link_parts(text, open, end) {
                Some((close, url_end)) => {
                    set(flags, j, open + 1, flags::HIDDEN);
                    set(flags, close, url_end, flags::HIDDEN);
                    set(flags, open + 1, close, flags::LINK);
                    op(&mut opaque, j, open + 1);
                    op(&mut opaque, close, url_end);
                    j = open + 1;
                }
                None => j += 1,
            }
        } else if (c == b'h' as u16 || c == b'H' as u16)
            && (j == start || is_space(text[j - 1]) || text[j - 1] == b'(' as u16)
        {
            match bare_url_end(text, j, end) {
                Some(k) => {
                    set(flags, j, k, flags::LINK);
                    op(&mut opaque, j, k);
                    j = k;
                }
                None => j += 1,
            }
        } else {
            j += 1;
        }
    }

    emphasis(text, start, end, &opaque, flags);
}

struct Delim {
    pos: usize,
    len: usize,
    orig: usize,
    ch: u16,
    open: bool,
    close: bool,
}

/// CommonMark's delimiter runs, simplified: no links in the stack (they were
/// done before), the rule of three kept.
fn emphasis(text: &[u16], start: usize, end: usize, opaque: &[bool], flags: &mut [u32]) {
    let mut delims: Vec<Delim> = Vec::new();
    let mut j = start;
    while j < end {
        let c = text[j];
        if opaque[j - start] || !(c == b'*' as u16 || c == b'_' as u16 || c == b'~' as u16) {
            j += 1;
            continue;
        }
        let mut k = j;
        while k < end && text[k] == c && !opaque[k - start] {
            k += 1;
        }
        let len = k - j;
        let before = if j == start { b' ' as u16 } else { text[j - 1] };
        let after = if k == end { b' ' as u16 } else { text[k] };
        let left = !is_ws(after) && (!is_punct(after) || is_ws(before) || is_punct(before));
        let right = !is_ws(before) && (!is_punct(before) || is_ws(after) || is_punct(after));
        let (open, close) = if c == b'_' as u16 {
            (
                left && (!right || is_punct(before)),
                right && (!left || is_punct(after)),
            )
        } else if c == b'~' as u16 {
            if len == 2 {
                (left, right)
            } else {
                (false, false)
            }
        } else {
            (left, right)
        };
        if open || close {
            delims.push(Delim {
                pos: j,
                len,
                orig: len,
                ch: c,
                open,
                close,
            });
        }
        j = k;
    }

    for ci in 0..delims.len() {
        if !delims[ci].close {
            continue;
        }
        while delims[ci].len > 0 {
            let closer = &delims[ci];
            let found = (0..ci).rev().find(|&oi| {
                let o = &delims[oi];
                o.open
                    && o.len > 0
                    && o.ch == closer.ch
                    && !((o.close || closer.open)
                        && (o.orig + closer.orig).is_multiple_of(3)
                        && !(o.orig.is_multiple_of(3) && closer.orig.is_multiple_of(3)))
            });
            let Some(oi) = found else { break };
            let ch = delims[ci].ch;
            let used = if ch == b'~' as u16 || (delims[oi].len >= 2 && delims[ci].len >= 2) {
                2
            } else {
                1
            };
            let style = if ch == b'~' as u16 {
                flags::STRIKE
            } else if used == 2 {
                flags::STRONG
            } else {
                flags::EMPH
            };
            let o_end = delims[oi].pos + delims[oi].len;
            let c_pos = delims[ci].pos;
            set(flags, o_end - used, o_end, flags::HIDDEN);
            set(flags, c_pos, c_pos + used, flags::HIDDEN);
            set(flags, o_end, c_pos, style);
            delims[oi].len -= used;
            delims[ci].pos += used;
            delims[ci].len -= used;
            // Openers between the pair can no longer open.
            for d in &mut delims[oi + 1..ci] {
                d.open = false;
            }
        }
    }
}

fn set(flags: &mut [u32], a: usize, b: usize, f: u32) {
    for x in &mut flags[a..b] {
        *x |= f;
    }
}

fn encode_fence(ch: u16, len: usize) -> i32 {
    let tilde = (ch == b'~' as u16) as i32;
    (tilde << 16) | (len.min(0xffff) as i32)
}

fn decode_fence(state: i32) -> (u16, usize) {
    let ch = if state >> 16 & 1 == 1 { b'~' } else { b'`' } as u16;
    (ch, (state & 0xffff) as usize)
}

fn run_len(text: &[u16], at: usize, ch: u16) -> usize {
    text[at.min(text.len())..]
        .iter()
        .take_while(|&&c| c == ch)
        .count()
}

/// Skips up to `max` spaces and tabs.
fn skip_spaces(text: &[u16], mut i: usize, max: usize) -> usize {
    let mut k = 0;
    while i < text.len() && k < max && is_space(text[i]) {
        i += 1;
        k += 1;
    }
    i
}

fn is_space(c: u16) -> bool {
    c == b' ' as u16 || c == b'\t' as u16
}

fn is_blank(text: &[u16]) -> bool {
    text.iter().all(|&c| is_space(c))
}

fn is_ws(c: u16) -> bool {
    is_space(c) || char::from_u32(c as u32).is_some_and(char::is_whitespace)
}

fn is_ascii_punct(c: u16) -> bool {
    c < 128 && (c as u8).is_ascii_punctuation()
}

fn is_punct(c: u16) -> bool {
    if c < 128 {
        return (c as u8).is_ascii_punctuation();
    }
    // Outside ASCII: anything that isn't a letter, digit or space. Surrogate
    // halves count as letters.
    match char::from_u32(c as u32) {
        Some(ch) => !ch.is_alphanumeric() && !ch.is_whitespace(),
        None => false,
    }
}

fn is_bullet(c: u16) -> bool {
    c == b'-' as u16 || c == b'*' as u16 || c == b'+' as u16
}

fn heading_level(text: &[u16], i: usize) -> Option<u8> {
    // A space must follow (CommonMark also takes a bare "#"): typing "#tag"
    // shouldn't hide the "#" while the tag isn't typed yet.
    let run = run_len(text, i, b'#' as u16);
    if (1..=6).contains(&run) && i + run < text.len() && is_space(text[i + run]) {
        Some(run as u8)
    } else {
        None
    }
}

/// Three or more of one of `-`, `*`, `_`, with nothing else but spaces.
fn is_rule(rest: &[u16]) -> bool {
    let Some(&first) = rest.iter().find(|&&c| !is_space(c)) else {
        return false;
    };
    if !(first == b'-' as u16 || first == b'*' as u16 || first == b'_' as u16) {
        return false;
    }
    let mut count = 0;
    for &c in rest {
        if c == first {
            count += 1;
        } else if !is_space(c) {
            return false;
        }
    }
    count >= 3
}

/// `[ ]`, `[x]` or `[X]`, then a space or the end of the line.
fn task_box(text: &[u16], at: usize) -> Option<bool> {
    if at + 3 > text.len() || text[at] != b'[' as u16 || text[at + 2] != b']' as u16 {
        return None;
    }
    if at + 3 < text.len() && !is_space(text[at + 3]) {
        return None;
    }
    match text[at + 1] {
        c if c == b' ' as u16 => Some(false),
        c if c == b'x' as u16 || c == b'X' as u16 => Some(true),
        _ => None,
    }
}

/// `1.` or `1)` (up to 9 digits), then a space or the end. Returns where the
/// marker ends.
fn number_marker(text: &[u16], i: usize) -> Option<usize> {
    let digits = text[i..]
        .iter()
        .take_while(|&&c| (b'0' as u16..=b'9' as u16).contains(&c))
        .count();
    if digits == 0 || digits > 9 {
        return None;
    }
    let p = i + digits;
    if p < text.len()
        && (text[p] == b'.' as u16 || text[p] == b')' as u16)
        && (p + 1 == text.len() || is_space(text[p + 1]))
    {
        Some(p + 1)
    } else {
        None
    }
}

fn find_backtick_run(text: &[u16], from: usize, end: usize, len: usize) -> Option<usize> {
    let mut k = from;
    while k < end {
        if text[k] == b'`' as u16 {
            let r = run_len(text, k, b'`' as u16).min(end - k);
            if r == len {
                return Some(k);
            }
            k += r;
        } else {
            k += 1;
        }
    }
    None
}

fn starts_with_ascii(text: &[u16], at: usize, end: usize, prefix: &str) -> bool {
    let p = prefix.as_bytes();
    at + p.len() <= end
        && text[at..at + p.len()]
            .iter()
            .zip(p)
            .all(|(&c, &b)| c < 128 && (c as u8).eq_ignore_ascii_case(&b))
}

/// `<https://…>` or `<mailto:…>`: where the `>` is.
fn autolink_end(text: &[u16], from: usize, end: usize) -> Option<usize> {
    if !(starts_with_ascii(text, from, end, "https://")
        || starts_with_ascii(text, from, end, "http://")
        || starts_with_ascii(text, from, end, "mailto:"))
    {
        return None;
    }
    (from..end)
        .find(|&k| text[k] == b'>' as u16)
        .filter(|&k| !text[from..k].iter().any(|&c| is_ws(c) || c == b'<' as u16))
}

/// `[text](url)` starting at the `[`: where the `]` is, and the end of `)`.
fn link_parts(text: &[u16], open: usize, end: usize) -> Option<(usize, usize)> {
    let mut depth = 0;
    let mut k = open;
    let close = loop {
        if k >= end {
            return None;
        }
        let c = text[k];
        if c == b'\\' as u16 {
            k += 2;
            continue;
        }
        if c == b'[' as u16 {
            depth += 1;
        } else if c == b']' as u16 {
            depth -= 1;
            if depth == 0 {
                break k;
            }
        }
        k += 1;
    };
    if close + 1 >= end || text[close + 1] != b'(' as u16 {
        return None;
    }
    let mut parens = 0;
    let mut k = close + 1;
    while k < end {
        let c = text[k];
        if c == b'\\' as u16 {
            k += 2;
            continue;
        }
        if c == b'(' as u16 {
            parens += 1;
        } else if c == b')' as u16 {
            parens -= 1;
            if parens == 0 {
                return Some((close, k + 1));
            }
        }
        k += 1;
    }
    None
}

/// A bare `https://…` or `http://…`: where it ends, without the punctuation
/// that usually follows a URL in a sentence.
fn bare_url_end(text: &[u16], at: usize, end: usize) -> Option<usize> {
    let scheme = if starts_with_ascii(text, at, end, "https://") {
        8
    } else if starts_with_ascii(text, at, end, "http://") {
        7
    } else {
        return None;
    };
    let mut k = at + scheme;
    while k < end && !is_ws(text[k]) && text[k] != b'<' as u16 {
        k += 1;
    }
    while k > at + scheme
        && matches!(
            text[k - 1],
            0x2e | 0x2c | 0x3b | 0x3a | 0x21 | 0x3f | 0x27 | 0x22 | 0x29
        )
    {
        // A ")" closes the URL only if the URL didn't open one.
        if text[k - 1] == b')' as u16 {
            let opens = text[at..k].iter().filter(|&&c| c == b'(' as u16).count();
            let closes = text[at..k].iter().filter(|&&c| c == b')' as u16).count();
            if closes <= opens {
                break;
            }
        }
        k -= 1;
    }
    (k > at + scheme).then_some(k)
}

/// The destination of the link at `pos` in the line, as a range, if there is
/// one (`[text](url)`, `<url>` or a bare URL). A `<...>` destination is
/// returned whole, brackets and inner spaces included.
pub fn link_at(text: &[u16], pos: usize) -> Option<(usize, usize)> {
    let n = text.len();
    let mut j = 0;
    while j < n {
        let c = text[j];
        if c == b'[' as u16 {
            if let Some((close, url_end)) = link_parts(text, j, n) {
                if pos >= j && pos < url_end {
                    let mut a = close + 2;
                    while a < url_end && is_space(text[a]) {
                        a += 1;
                    }
                    let mut b = a;
                    if a < url_end - 1 && text[a] == b'<' as u16 {
                        // <...> may hold spaces; the span includes the brackets.
                        if let Some(gt) = (a + 1..url_end - 1).find(|&k| text[k] == b'>' as u16) {
                            return Some((a, gt + 1));
                        }
                    }
                    while b < url_end - 1 && !is_space(text[b]) {
                        b += 1;
                    }
                    return Some((a, b));
                }
                j = url_end;
                continue;
            }
        } else if c == b'<' as u16 {
            if let Some(k) = autolink_end(text, j + 1, n) {
                if pos >= j && pos <= k {
                    return Some((j, k + 1));
                }
                j = k + 1;
                continue;
            }
        } else if (c == b'h' as u16 || c == b'H' as u16)
            && (j == 0 || is_space(text[j - 1]) || text[j - 1] == b'(' as u16)
            && let Some(k) = bare_url_end(text, j, n)
        {
            if pos >= j && pos < k {
                return Some((j, k));
            }
            j = k;
            continue;
        }
        j += 1;
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn u(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    /// The line as the Formatted view shows it: hidden characters left out.
    fn shown(s: &str, state: i32) -> (String, Line) {
        let text = u(s);
        let mut f = Vec::new();
        let line = parse_line(&text, state, &mut f);
        let kept: Vec<u16> = text
            .iter()
            .zip(&f)
            .filter(|&(_, &x)| x & flags::HIDDEN == 0)
            .map(|(&c, _)| c)
            .collect();
        (String::from_utf16_lossy(&kept), line)
    }

    fn flags_of(s: &str) -> Vec<u32> {
        let mut f = Vec::new();
        parse_line(&u(s), 0, &mut f);
        f
    }

    #[test]
    fn plain_text_has_no_flags() {
        let f = flags_of("Just a line, with 2 * 3 = 6 and snake_case_name.");
        assert!(f.iter().all(|&x| x == 0), "{f:?}");
    }

    #[test]
    fn bold_italic_strike() {
        assert_eq!(shown("a **b** c", 0).0, "a b c");
        assert_eq!(shown("a *b* c", 0).0, "a b c");
        assert_eq!(shown("a _b_ c", 0).0, "a b c");
        assert_eq!(shown("a ~~b~~ c", 0).0, "a b c");
        assert_eq!(shown("***both***", 0).0, "both");
        let f = flags_of("***x***");
        assert_eq!(
            f[3] & (flags::STRONG | flags::EMPH),
            flags::STRONG | flags::EMPH
        );
        let f = flags_of("~~x~~");
        assert_ne!(f[2] & flags::STRIKE, 0);
    }

    #[test]
    fn unmatched_and_intraword_delimiters_stay() {
        assert_eq!(shown("**open only", 0).0, "**open only");
        assert_eq!(shown("snake_case_name", 0).0, "snake_case_name");
        assert_eq!(shown("2 * 3 * 4", 0).0, "2 * 3 * 4");
        assert_eq!(shown("~one~", 0).0, "~one~");
        assert_eq!(shown("in*ner*word", 0).0, "innerword");
    }

    #[test]
    fn nested_emphasis() {
        let text = "**bold *both* bold**";
        assert_eq!(shown(text, 0).0, "bold both bold");
        let f = flags_of(text);
        let i = text.find("both").unwrap();
        assert_eq!(
            f[i] & (flags::STRONG | flags::EMPH),
            flags::STRONG | flags::EMPH
        );
    }

    #[test]
    fn code_spans_hide_backticks_and_protect_contents() {
        assert_eq!(shown("use `a*b*c` here", 0).0, "use a*b*c here");
        assert_eq!(shown("``a ` b``", 0).0, "a ` b");
        assert_eq!(shown("`open", 0).0, "`open");
        let f = flags_of("x `**y**` z");
        assert_ne!(f[4] & flags::CODE, 0);
        assert_eq!(f[4] & flags::STRONG, 0);
    }

    #[test]
    fn escapes() {
        assert_eq!(shown(r"\*not\* bold", 0).0, "*not* bold");
        assert_eq!(shown(r"a\b", 0).0, r"a\b");
    }

    #[test]
    fn links() {
        assert_eq!(
            shown("see [the docs](https://x.org/a_(b)) now", 0).0,
            "see the docs now"
        );
        assert_eq!(shown("[**bold** link](u)", 0).0, "bold link");
        assert_eq!(shown("![alt](img.png)", 0).0, "alt");
        assert_eq!(shown("<https://x.org>", 0).0, "https://x.org");
        assert_eq!(shown("[not a link] (x)", 0).0, "[not a link] (x)");
        let f = flags_of("go https://x.org/a_b_c. ok");
        assert_ne!(f[3] & flags::LINK, 0);
        assert_eq!(
            f[3 + "https://x.org/a_b_c".len()] & flags::LINK,
            0,
            "the full stop isn't the link's"
        );
        assert!(
            f.iter().all(|&x| x & flags::HIDDEN == 0),
            "underscores in a URL aren't emphasis"
        );
    }

    #[test]
    fn link_at_finds_the_url() {
        let text = u("a [b](https://x.org/p) c <https://y.org> d https://z.org.");
        let url = |p: usize| link_at(&text, p).map(|(a, b)| String::from_utf16_lossy(&text[a..b]));
        assert_eq!(url(3).as_deref(), Some("https://x.org/p"));
        assert_eq!(url(0), None);
        assert_eq!(url(27).as_deref(), Some("<https://y.org>"));
        assert_eq!(url(text.len() - 3).as_deref(), Some("https://z.org"));
    }

    #[test]
    fn link_at_takes_the_whole_angle_destination() {
        let text = u("[a](<b c>) and [d](<e>  \"t\") [f](<g");
        let url = |p: usize| link_at(&text, p).map(|(a, b)| String::from_utf16_lossy(&text[a..b]));
        assert_eq!(url(1).as_deref(), Some("<b c>"));
        assert_eq!(url(16).as_deref(), Some("<e>"));
        let plain = u("[a](b \"t\")");
        let (a, b) = link_at(&plain, 1).unwrap();
        assert_eq!(String::from_utf16_lossy(&plain[a..b]), "b");
    }

    #[test]
    fn headings() {
        let (s, l) = shown("## Title", 0);
        assert_eq!(s, "Title");
        assert_eq!((l.kind, l.heading, l.content_start), (Kind::Heading, 2, 3));
        assert_eq!(shown("# Title ##", 0).0, "Title");
        assert_eq!(shown("# C# notes", 0).0, "C# notes");
        assert_eq!(shown("#hashtag", 0).1.kind, Kind::Paragraph);
        assert_eq!(shown("####### seven", 0).1.kind, Kind::Paragraph);
        assert_eq!(shown("#", 0).1.kind, Kind::Paragraph);
        let (s, l) = shown("# ", 0);
        assert_eq!((s.as_str(), l.kind), ("", Kind::Heading));
        let f = flags_of("# A **b**");
        assert_eq!(
            f[6] & (flags::HEADING | flags::STRONG),
            flags::HEADING | flags::STRONG
        );
    }

    #[test]
    fn lists_and_tasks() {
        let (s, l) = shown("- item", 0);
        assert_eq!(s, "- item");
        assert_eq!(
            (l.kind, l.marker_start, l.content_start),
            (Kind::Bullet, 0, 2)
        );
        assert_eq!(flags_of("- item")[0], flags::PAD);

        let (s, l) = shown("  * [x] done", 0);
        assert_eq!(s, "  [x] done");
        assert_eq!(
            (l.kind, l.checked, l.marker_start, l.marker_len),
            (Kind::Task, 1, 4, 3)
        );
        assert_ne!(flags_of("- [x] done")[7] & flags::DONE, 0);

        let (_, l) = shown("    - deeper", 0);
        assert_eq!((l.kind, l.marker_start), (Kind::Bullet, 4));

        let (_, l) = shown("12. twelve", 0);
        assert_eq!(
            (l.kind, l.marker_len, l.content_start),
            (Kind::Numbered, 3, 4)
        );
        assert_eq!(shown("1.5 million", 0).1.kind, Kind::Paragraph);
        assert_eq!(shown("-not a bullet", 0).1.kind, Kind::Paragraph);
        assert_eq!(shown("-", 0).1.kind, Kind::Paragraph);
        assert_eq!(shown("- ", 0).1.kind, Kind::Bullet);
    }

    #[test]
    fn quotes_and_rules() {
        let (s, l) = shown("> > - quoted", 0);
        assert_eq!(s, "> > - quoted");
        assert_eq!((l.quote_depth, l.kind), (2, Kind::Bullet));
        assert_ne!(flags_of("> hi")[2] & flags::QUOTE, 0);
        assert_eq!(shown("---", 0).1.kind, Kind::Rule);
        assert_eq!(shown("* * *", 0).1.kind, Kind::Rule);
        assert_eq!(shown("--", 0).1.kind, Kind::Paragraph);
    }

    #[test]
    fn fenced_code_carries_state() {
        let lines = ["```rust", "let x = **y**;", "", "```", "**bold**"];
        let mut state = 0;
        let mut kinds = Vec::new();
        let mut f = Vec::new();
        for l in lines {
            let line = parse_line(&u(l), state, &mut f);
            state = line.state;
            kinds.push(line.kind);
        }
        assert_eq!(
            kinds,
            [
                Kind::Fence,
                Kind::Code,
                Kind::Code,
                Kind::Fence,
                Kind::Paragraph
            ]
        );
        assert_eq!(state, 0);
        // A backtick fence can't close a tilde one, nor a shorter one a longer.
        let open = parse_line(&u("~~~~"), 0, &mut f).state;
        assert_eq!(parse_line(&u("```"), open, &mut f).kind, Kind::Code);
        assert_eq!(parse_line(&u("~~~"), open, &mut f).kind, Kind::Code);
        assert_eq!(parse_line(&u("~~~~~"), open, &mut f).kind, Kind::Fence);
    }

    #[test]
    fn runs_merge_and_skip_unflagged() {
        let mut out = Vec::new();
        runs(&[0, 1, 1, 0, 4, 4, 4], &mut out);
        assert_eq!(
            out,
            [
                Run {
                    start: 1,
                    len: 2,
                    flags: 1
                },
                Run {
                    start: 4,
                    len: 3,
                    flags: 4
                }
            ]
        );
    }

    #[test]
    fn non_ascii_text() {
        assert_eq!(shown("**héllo** wörld 😀 *x*", 0).0, "héllo wörld 😀 x");
    }

    #[test]
    fn never_panics_on_odd_input() {
        let samples = [
            "",
            "*",
            "**",
            "***",
            "`",
            "``",
            "[",
            "[]",
            "[](",
            "[a](",
            "<",
            "<https://",
            "\\",
            "#",
            "- [",
            "- [ ]",
            "1.",
            "> ",
            "~~~",
            "```a`b",
            "*a**b*",
            "**a*",
            "_a__b_",
            "[a][b](c)",
            "![",
            "h",
            "http://",
            ")))",
            "https://)",
        ];
        let mut f = Vec::new();
        for s in samples {
            for state in [0, encode_fence(b'`' as u16, 3)] {
                let text = u(s);
                parse_line(&text, state, &mut f);
                for p in 0..=text.len() {
                    link_at(&text, p);
                }
            }
        }
    }
}
