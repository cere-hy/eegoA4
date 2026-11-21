#pragma once

#include <cstdint>

// S3cPF8d: CJK adjacent-whitespace cleaning decision table.
//
// EPUB sources insert spaces between adjacent CJK characters — inside a bare text
// node ("祥 子 尽 头") or at an inline <span> boundary ("</span> <span>"). Treating
// every such space as a word boundary produces visible gaps (~6 chars/line instead
// of ~15). The cleaner decides, at a single funnel in characterData(), whether a
// whitespace run should create a gap (nextWordContinues=false) or be absorbed
// (nextWordContinues=true).
//
// PF8d design principle — "nearest neighbour", NOT a punctuation whitelist:
//   PF8b tried to enumerate "CJK text" as a codepoint set and MISSED the entire
//   0x2000-0x206F General Punctuation block (curly quotes 0x201C/201D/2018/2019,
//   ellipsis 0x2026, em dash 0x2014). A whitelist can never be exhaustive.
//   Instead: if EITHER side is a Han ideograph, absorb the space. This covers
//   Han|Han, Han|punctuation and punctuation|Han uniformly. A space next to an
//   ASCII letter/digit is kept as a single real space (Latin word separation).
//
// Header-only and dependency-free so host tests can exercise the exact rule the
// parser uses. U+00A0 / U+202F (no-break spaces) and U+3000 (ideographic space)
// never reach shouldStrip(): characterData() has dedicated branches for the first
// two and isWhitespace() does not classify U+3000, so it flows as content.
//
// SINGLE SOURCE OF TRUTH (addendum clause 54): this header is the ONLY CJK
// codepoint-range definition in the repository. Do NOT duplicate the Han ranges
// (isCjkIdeograph) anywhere else — firmware, host tests, tools and any port must
// include this header instead. Upstream pico-plus carried two copies of the set
// and they drifted apart in the 0x2000 punctuation block, producing divergent
// line breaking between builds. Any future range change belongs here, once.
namespace CjkWhitespace {

// Han ideographs. Narrower than "all CJK punctuation" on purpose: punctuation is
// handled by the nearest-neighbour rule (a punctuation mark beside a Han char is
// absorbed), so no punctuation set needs to be maintained here.
//   0x3007    〇 IDEOGRAPHIC NUMBER ZERO
//   0x3400-4DBF   CJK Extension A
//   0x4E00-9FFF   CJK Unified Ideographs
//   0xF900-FAFF   CJK Compatibility Ideographs
//   0x20000-2A6DF Extension B
//   0x2A700-2B73F Extension C
//   0x2B740-2B81F Extension D
//   0x2B820-2CEAF Extension E/F
//   0x2CEB0-2EBEF Extension G  (S3cPF8d addendum clause 37)
// Extensions H/I (0x2EBF0+) are intentionally NOT added in this batch.
constexpr bool isCjkIdeograph(const uint32_t cp) {
  return cp == 0x3007 || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2A6DF) ||
         (cp >= 0x2A700 && cp <= 0x2B73F) || (cp >= 0x2B740 && cp <= 0x2B81F) ||
         (cp >= 0x2B820 && cp <= 0x2CEAF) || (cp >= 0x2CEB0 && cp <= 0x2EBEF);
}

// ASCII letters/digits: a space adjacent to one of these is a real word space and
// must be kept (CJK|Latin and Latin|CJK both keep a single space).
constexpr bool isAsciiAlnum(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

// Decision for a whitespace run sitting between leftCp and rightCp.
//   returns true  -> strip the space (nextWordContinues=true, no gap)
//   returns false -> keep a single space (nextWordContinues=false, real boundary)
//
// leftCp == 0 means no visible neighbour on the left (line/paragraph start):
// decide from the right side alone so a leading space + first-line indent does not
// double up. Such a leading space before a Han char is absorbed; before Latin it
// is kept as a single space.
constexpr bool shouldStrip(const uint32_t leftCp, const uint32_t rightCp) {
  if (rightCp == 0) return false;  // no right neighbour: conservative keep
  if (leftCp == 0) return isCjkIdeograph(rightCp);
  if (isAsciiAlnum(leftCp) || isAsciiAlnum(rightCp)) return false;
  if (isCjkIdeograph(leftCp) || isCjkIdeograph(rightCp)) return true;
  return false;  // neither side CJK ideograph nor ASCII alnum (e.g. punct|punct, emoji|emoji): keep
}

}  // namespace CjkWhitespace
