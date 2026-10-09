// Size limits, from the S2 measurements (docs/DESIGN.md, "S2: large files").
#pragma once

#include <QtGlobal>

namespace Limits {
// Markdown bigger than this opens as plain text, with a banner.
inline constexpr qint64 formattedBytes = 1 << 20;
// A line longer than this (UTF-16 units) opens the file read-only.
inline constexpr int lineLength = 100'000;
// A line longer than this (UTF-16 units) is not syntax highlighted. The
// definitions' regular expressions are quadratic on some lines (a line of
// "<<" in TSX took 19 s at 100,000 characters, of "%" in Crystal 18 s), and
// the highlighter runs on the window's thread. At this length the worst of
// the 460 definitions measured stayed under 0.3 s. docs/SECURITY.md.
inline constexpr int highlightedLineLength = 10'000;
// Files bigger than this are refused.
inline constexpr qint64 fileBytes = 10 << 20;
}
