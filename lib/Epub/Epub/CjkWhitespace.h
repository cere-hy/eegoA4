#pragma once

#include <cstdint>

// S3cPF8b: CJK adjacent-whitespace cleaning.
//
// EPUB sources frequently insert spaces between adjacent CJK characters — either
// inside a bare text node ("祥 子 尽 头") or at an inline <span> boundary
// ("</span> <span>"). The content-tree builder used to treat every such space as a
// real word boundary (continues=false), producing a visible gap between CJK glyphs
// and collapsing a line from ~15 chars to ~6. Embedded-style ON happened to mask
// this because styled spans re-assert continues=true at their boundary; that was a
// side effect, not a cleaning capability.
//
// These helpers are header-only and dependency-free so host tests can exercise the
// exact codepoint rules the parser uses. The parser calls shouldStripBetween() at
// the single funnel where characterData() turns whitespace into a word boundary.

namespace CjkWhitespace {

// True for codepoints that count as "CJK text" for whitespace stripping: ideographs
// (all common + extension blocks), kana, Hangul, CJK punctuation/symbols, fullwidth
// forms, plus the three lone CJK marks 「々」「ー」「〇」 that sit just outside the
// main CJK Unified block. Latin letters, ASCII digits and ASCII punctuation are NOT
// included: a space between a CJK char and a Latin/digit must be kept.
//
// Codepoint ranges (mirrors Utf8.h's utf8IsCjkBreakable, plus explicit lone marks):
//   0x3005  々 IDEOGRAPHIC ITERATION MARK
//   0x30FC  ー KATAKANA-HIRAGANA PROLONGED SOUND MARK
//   0x3007  〇 IDEOGRAPHIC NUMBER ZERO
//   0x1100-0x11FF    Hangul Jamo
//   0x3000-0x303F    CJK Symbols and Punctuation (includes 3005/3007)
//   0x3040-0x309F    Hiragana (includes 30FC)
//   0x30A0-0x30FF    Katakana
//   0x3130-0x318F    Hangul Compatibility Jamo
//   0x3400-0x4DBF    CJK Extension A
//   0x4E00-0x9FFF    CJK Unified Ideographs
//   0xA960-0xA97F    Hangul Jamo Extended-A
//   0xAC00-0xD7AF    Hangul Syllables
//   0xD7B0-0xD7FF    Hangul Jamo Extended-B
//   0xF900-0xFAFF    CJK Compatibility Ideographs
//   0xFE10-0xFE1F    Vertical Forms
//   0xFE30-0xFE4F    CJK Compatibility Forms
//   0xFF01-0xFF60    Fullwidth Latin / Punctuation
//   0xFF65-0xFFEF    Halfwidth Katakana / Hangul
//   0x20000-0x2A6DF  CJK Extension B
//   0x2A700-0x2B73F  CJK Extension C
//   0x2B740-0x2B81F  CJK Extension D
//   0x2B820-0x2CEAF  CJK Extension E/F
constexpr bool isCjkText(const uint32_t cp) {
  return cp == 0x3005 || cp == 0x30FC || cp == 0x3007 ||
         (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3000 && cp <= 0x303F) ||
         (cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
         (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA960 && cp <= 0xA97F) ||
         (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xD7B0 && cp <= 0xD7FF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE1F) ||
         (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF01 && cp <= 0xFF60) ||
         (cp >= 0xFF65 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2A6DF) ||
         (cp >= 0x2A700 && cp <= 0x2B73F) || (cp >= 0x2B740 && cp <= 0x2B81F) ||
         (cp >= 0x2B820 && cp <= 0x2CEAF);
}

// Decision for a whitespace run sitting between leftCp and rightCp.
//   - both sides CJK (incl. CJK punctuation)     -> strip (return true)
//   - either side Latin/digit/ASCII punctuation  -> keep a single space (false)
//   - note: U+00A0 (NBSP) never reaches here — characterData() has a dedicated
//     branch that keeps it as semantic whitespace; 0 is used as "no neighbour" and
//     conservatively keeps the space.
constexpr bool shouldStripBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (leftCp == 0 || rightCp == 0) return false;
  return isCjkText(leftCp) && isCjkText(rightCp);
}

}  // namespace CjkWhitespace
