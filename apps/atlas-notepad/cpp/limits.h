// Size limits, from the S2 measurements (docs/DESIGN.md, "S2: large files").
#pragma once

#include <QtGlobal>

namespace Limits {
// Markdown bigger than this opens as plain text, with a banner.
inline constexpr qint64 formattedBytes = 1 << 20;
// A line longer than this (UTF-16 units) opens the file read-only.
inline constexpr int lineLength = 100'000;
// Files bigger than this are refused.
inline constexpr qint64 fileBytes = 10 << 20;
}
