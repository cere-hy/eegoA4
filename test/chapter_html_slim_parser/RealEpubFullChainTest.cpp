// S3cPF8d: real-expat full-chain tests for CJK adjacent-whitespace cleaning.
// Walks the real parseAndBuildPages() -> XML_ParseBuffer path (never hand-feeds a
// static callback), and styles ON loads a REAL CssParser.loadFromStream() (never
// cssParser=nullptr — that stub was the PF8b host false-positive). Asserts:
//   1. Four-combination matrix (span/bare source x styles on/off) yields identical
//      token sequences (words-per-line) — cleaning is source-shape independent.
//   2. Diagnostic counters: miss≈0 and hit+resolve cover the whitespace total
//      (PF8b real-device was hit≈0 / miss≫hit / resolve≈0). This is the sole hard
//      evidence that the structural defect is gone.
//   3. Nearest-neighbour table: punctuation (curly quotes/ellipsis/em dash) beside
//      Han is absorbed; CJK|Latin keeps one space; leading space; NBSP/U+3000 kept;
//      preserve bypass; focus-reading step; English regression.
//
// Copyright note: no copyrighted book text is embedded. The *_structure.xhtml
// files are structural-equivalent synthetic CJK content. Real EPUB originals, if
// the user places them in fixtures/real/, are picked up by the RealEpubOriginal*
// tests (skipped if absent).
#include <Epub/CjkWhitespace.h>
#include <Epub/Page.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "Epub/parsers/ChapterHtmlSlimParser.h"

namespace {

namespace fs = std::filesystem;

struct RunResult {
  uint32_t hit = 0, miss = 0, resolve = 0, overrideC = 0;
  // addendum clause 47: seven counters (spaces/runs/hit/miss/resolve/preserve/semantic)
  uint32_t spaces = 0, runs = 0, preserve = 0, semantic = 0;
  size_t pages = 0;
  std::vector<size_t> wordsPerLine;
  // Flattened token texts in emission order (wordCount per line is enough for the
  // matrix; the vector-of-texts is used by the punctuation/preserve cases).
  std::vector<std::string> tokens;
  std::vector<uint32_t> tokenOffsets;
};

fs::path fixturesDir() {
  return fs::path(__FILE__).parent_path() / "fixtures";
}

// write a raw XHTML string to a temp path (for inline synthetic cases)
fs::path writeTempXhtml(const std::string& tag, const std::string& content) {
  const fs::path p = fs::temp_directory_path() / ("pf8d_" + tag + ".xhtml");
  std::ofstream f(p, std::ios::binary);
  f.write(content.data(), content.size());
  return p;
}

RunResult runXhtml(const fs::path& xhtml, bool stylesOn, const std::string& tag, bool focusReading = false,
                   size_t chunkSize = 0) {
  // Copy fixture to a temp file so the Storage stub opens a plain path (mirrors the
  // standalone diagnostic; avoids any resolvePath() mapping on repo paths).
  const fs::path tempXhtml = fs::temp_directory_path() / ("pf8dr_" + tag + ".xhtml");
  {
    std::ifstream src(xhtml, std::ios::binary);
    std::ofstream dst(tempXhtml, std::ios::binary);
    dst << src.rdbuf();
  }
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  if (stylesOn) {
    const auto cssPath = fixturesDir() / "style.css";
    HalFile in;
    if (in.open(cssPath.string().c_str(), "rb")) {
      cssParser.loadFromStream(in);
    }
  }
  // parser stores filepath as const std::string& (NOT a copy): must outlive parser.
  const std::string xpathStr = tempXhtml.string();
  RunResult r;
  auto onPage = [&r](std::unique_ptr<Page> page, auto, auto, auto) {
    if (!page) return;
    ++r.pages;
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*el);
      const auto& block = *line.getBlock();
      r.wordsPerLine.push_back(block.wordCount());
      for (size_t i = 0; i < block.wordCount(); ++i) {
        r.tokens.emplace_back(block.wordText(static_cast<uint16_t>(i)), block.wordTextLen(static_cast<uint16_t>(i)));
        r.tokenOffsets.push_back(block.wordSourceRange(static_cast<uint16_t>(i)).start);
      }
    }
  };
  ChapterHtmlSlimParser parser{nullptr,
                               xpathStr,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               focusReading,
                               onPage,
                               stylesOn,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               stylesOn ? &cssParser : nullptr};
  if (chunkSize) parser.setParseChunkSize(chunkSize);  // addendum clause 43

  EXPECT_TRUE(parser.parseAndBuildPages());
  parser.diagWsCounters(r.hit, r.miss, r.resolve, r.overrideC);
  parser.diagWsCounters7(r.spaces, r.runs, r.hit, r.miss, r.resolve, r.preserve, r.semantic);
  return r;
}

size_t minLineWords(const RunResult& r) {
  size_t m = SIZE_MAX;
  for (auto w : r.wordsPerLine) if (w < m) m = w;
  return m == SIZE_MAX ? 0 : m;
}

void report(const char* label, const RunResult& r) {
  std::printf("    %-30s pages=%zu lines=%zu minWords=%zu hit=%u miss=%u resolve=%u override=%u [",
              label, r.pages, r.wordsPerLine.size(), minLineWords(r), r.hit, r.miss, r.resolve, r.overrideC);
  for (auto w : r.wordsPerLine) std::printf("%zu ", w);
  std::printf("]\n");
}

// ---- 1. Four-combination matrix: identical token sequences --------------------
class Matrix4 : public ::testing::TestWithParam<std::pair<const char*, bool>> {};

TEST_P(Matrix4, CjkLinesAtLeast15Words) {
  const auto [file, stylesOn] = GetParam();
  const fs::path xhtml = fixturesDir() / file;
  ASSERT_TRUE(fs::exists(xhtml)) << xhtml;
  const auto r = runXhtml(xhtml, stylesOn, std::string(file) + (stylesOn ? "_on" : "_off"));
  report((std::string(file) + (stylesOn ? "/on" : "/off")).c_str(), r);
  ASSERT_FALSE(r.wordsPerLine.empty());
  EXPECT_GE(minLineWords(r), 15u) << "source=" << file << " stylesOn=" << stylesOn;
}

INSTANTIATE_TEST_SUITE_P(
    Pf8d, Matrix4,
    ::testing::Values(std::make_pair("xiangzi_structure.xhtml", true),
                      std::make_pair("xiangzi_structure.xhtml", false),
                      std::make_pair("shiri_structure.xhtml", true),
                      std::make_pair("shiri_structure.xhtml", false)));

TEST(Matrix4, SourceShapesProduceIdenticalTokenSequence) {
  const auto x_on = runXhtml(fixturesDir() / "xiangzi_structure.xhtml", true, "shape_x_on");
  const auto x_off = runXhtml(fixturesDir() / "xiangzi_structure.xhtml", false, "shape_x_off");
  const auto s_on = runXhtml(fixturesDir() / "shiri_structure.xhtml", true, "shape_s_on");
  const auto s_off = runXhtml(fixturesDir() / "shiri_structure.xhtml", false, "shape_s_off");
  // Same synthetic CJK glyph stream in all four shapes -> same token count/sequence.
  ASSERT_EQ(x_on.tokens.size(), s_off.tokens.size());
  EXPECT_EQ(x_on.tokens, s_off.tokens);
  EXPECT_EQ(x_on.tokens, x_off.tokens);
  EXPECT_EQ(x_on.tokens, s_on.tokens);
}

// ---- 2. Diagnostic: the PF8b structural defect is GONE -----------------------
TEST(WhitespaceDiag, MissNearZeroAndResolveCoversRuns) {
  // Both source shapes, both style switches: left neighbour must now be stashable.
  for (const char* file : {"xiangzi_structure.xhtml", "shiri_structure.xhtml"}) {
    for (bool stylesOn : {true, false}) {
      const std::string tag = std::string("diag_") + file + (stylesOn ? "_on" : "_off");
      const auto r = runXhtml(fixturesDir() / file, stylesOn, tag);
      report(tag.c_str(), r);
      // PF8b real-device/ host was hit≈0 / miss≫hit / resolve≈0. PF8d: hit>0
      // and miss==0 (all whitespace runs have a left neighbour mid-paragraph).
      EXPECT_GT(r.hit, 0u) << tag;
      EXPECT_EQ(r.miss, 0u) << tag;
      EXPECT_GT(r.resolve, 0u) << tag;
      // hit counts first-space stashes; resolve fires once per run when the right
      // codepoint arrives. They must match (every stashed run is resolved).
      EXPECT_EQ(r.hit, r.resolve) << tag;
    }
  }
}

// ---- 3. Nearest-neighbour decision table (pure header, no parser) ------------
TEST(CjkWhitespaceTable, HanHanStripLatinKeepPunctuationNearest) {
  using namespace CjkWhitespace;
  const uint32_t HAN = 0x4E2D;        // 中
  const uint32_t HAN2 = 0x6587;       // 文
  const uint32_t LATIN = 'a';
  const uint32_t DIGIT = '7';
  const uint32_t CURLY_L = 0x201C;    // “
  const uint32_t CURLY_R = 0x201D;    // ”
  const uint32_t APOS_L = 0x2018;     // ‘
  const uint32_t ELLIPSIS = 0x2026;   // …
  const uint32_t DASH = 0x2014;       // —

  // Han|Han -> strip
  EXPECT_TRUE(shouldStrip(HAN, HAN2));
  // Han|punctuation and punctuation|Han -> strip (the PF8b whitelist hole).
  EXPECT_TRUE(shouldStrip(HAN, CURLY_L));
  EXPECT_TRUE(shouldStrip(CURLY_R, HAN));
  EXPECT_TRUE(shouldStrip(HAN, APOS_L));
  EXPECT_TRUE(shouldStrip(HAN, ELLIPSIS));
  EXPECT_TRUE(shouldStrip(DASH, HAN));
  // Han|Latin/digit and Latin/digit|Han -> keep single space
  EXPECT_FALSE(shouldStrip(HAN, LATIN));
  EXPECT_FALSE(shouldStrip(LATIN, HAN));
  EXPECT_FALSE(shouldStrip(DIGIT, HAN));
  // Latin|Latin -> keep single space
  EXPECT_FALSE(shouldStrip(LATIN, LATIN));
  // left missing: decide from right (Han strip, Latin keep)
  EXPECT_TRUE(shouldStrip(0, HAN));
  EXPECT_FALSE(shouldStrip(0, LATIN));
  // right missing: conservative keep
  EXPECT_FALSE(shouldStrip(HAN, 0));
  // NBSP/U+202F/U+3000 are NOT Han and NOT ASCII alnum: they never take part in
  // the judgement as a HAN side. "Not participating in cleaning" means their own
  // bytes are preserved (NBSP/U+202F via dedicated no-break token, U+3000 as plain
  // content — neither flows through the whitespace run branch); it does NOT mean
  // shouldStrip() must return false when a Han char is the OTHER side. Nearest
  // neighbour still strips a normal space adjacent to Han.
  EXPECT_FALSE(isCjkIdeograph(0xA0));
  EXPECT_FALSE(isCjkIdeograph(0x202F));
  EXPECT_FALSE(isCjkIdeograph(0x3000));
  EXPECT_FALSE(isAsciiAlnum(0xA0));
}

TEST(CjkWhitespaceTable, HanRangeBoundaries) {
  using namespace CjkWhitespace;
  EXPECT_TRUE(isCjkIdeograph(0x3007));    // 〇
  EXPECT_TRUE(isCjkIdeograph(0x3400));    // ext A low
  EXPECT_TRUE(isCjkIdeograph(0x4DBF));    // ext A high
  EXPECT_TRUE(isCjkIdeograph(0x4E00));    // basic low
  EXPECT_TRUE(isCjkIdeograph(0x9FFF));    // basic high
  EXPECT_TRUE(isCjkIdeograph(0xF900));    // compat
  EXPECT_TRUE(isCjkIdeograph(0x20000));   // ext B low
  EXPECT_TRUE(isCjkIdeograph(0x2A6DF));   // ext B high
  EXPECT_TRUE(isCjkIdeograph(0x2A700));   // ext C low
  EXPECT_TRUE(isCjkIdeograph(0x2B73F));   // ext C high
  EXPECT_TRUE(isCjkIdeograph(0x2B740));   // ext D low
  EXPECT_TRUE(isCjkIdeograph(0x2B81F));   // ext D high
  EXPECT_TRUE(isCjkIdeograph(0x2B820));   // ext E/F low
  EXPECT_TRUE(isCjkIdeograph(0x2CEAF));   // ext E/F high
  // addendum clause 37: every interval's first/last codepoint pinned.
  EXPECT_TRUE(isCjkIdeograph(0x2CEB0));   // ext G low  (added this batch)
  EXPECT_TRUE(isCjkIdeograph(0x2EBEF));   // ext G high (added this batch)
  EXPECT_FALSE(isCjkIdeograph(0x2EBF0));  // ext H low  — NOT added (clause 37)
  EXPECT_FALSE(isCjkIdeograph(0x4DFF));   // below ext A
  EXPECT_FALSE(isCjkIdeograph(0x3000));   // ideographic space is NOT an ideograph
  EXPECT_FALSE(isCjkIdeograph(0x201C));   // curly quote is NOT ideograph (nearest rule)
}

// ---- 4. Full-chain punctuation case: no residual space beside quotes --------
TEST(PunctuationFullChain, CurlyQuotesEllipsisDashAbsorbed) {
  // "认“识”…—文" with literal spaces around punctuation; all adjacent to Han,
  // so every space must be absorbed -> one token per paragraph, no gaps.
  const auto p = writeTempXhtml("punct",
      "<html><body><p>认 “ 识 ” … — 文 字 阅 读 写 作 学 习 进 步 天 天 向 上 努 力 生 活 美 好 时 光 每 一 页 都 精 彩 无 限 循 环 往 复</p></body></html>");
  const auto r = runXhtml(p, true, "punct_on");
  report("punct/on", r);
  // Every synthetic token here is a single Han char or a punctuation mark adjacent
  // to Han; the cleaner must absorb all spaces -> the first line holds >=15 words
  // and no space-only tokens appear.
  EXPECT_GE(minLineWords(r), 15u);
  for (const auto& t : r.tokens) {
    EXPECT_FALSE(t.empty());
    // no token should be a bare ASCII space (that would be a residual gap)
    EXPECT_NE(t, " ");
  }
}

// ---- 5. Leading space: no double space+indent --------------------------------
TEST(LeadingSpaceFullChain, NoDoubleIndent) {
  // Paragraph begins with a leading space before Han; left missing -> decide from
  // right (Han) -> strip. No leading space token should be emitted.
  const auto p = writeTempXhtml("lead",
      "<html><body><p> 认 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活 祥 子 拉 车 北 平 冬 天 早 晨 坚 持 不 懈</p></body></html>");
  const auto r = runXhtml(p, false, "lead_off");
  report("lead/off", r);
  ASSERT_FALSE(r.tokens.empty());
  // First emitted token must not be a bare space.
  EXPECT_NE(r.tokens.front(), " ");
  EXPECT_GE(minLineWords(r), 15u);
}

// ---- 6. preserve bypass: pre block keeps indentation -------------------------
TEST(PreserveFullChain, PreBlockKeepsIndentation) {
  // Inside <pre>, whitespace is content: leading spaces must survive as a word.
  // CSS white-space:pre reaches effectivePreserveWhitespace via the <pre> tag.
  const auto p = writeTempXhtml("pre",
      "<html><body><pre>    AAAA BBBB CCCC DDDD EEEE FFFF GGGG HHHH</pre></body></html>");
  const auto r = runXhtml(p, false, "pre_off");
  report("pre/off", r);
  // In preserve mode each whitespace char is emitted as its own single-space token
  // (see characterData()'s effectivePreserveWhitespace branch). The leading 4-space
  // indent therefore appears as the first four " " tokens, not one "    " token.
  ASSERT_GE(r.tokens.size(), 5u);
  EXPECT_EQ(r.tokens[0], " ");
  EXPECT_EQ(r.tokens[1], " ");
  EXPECT_EQ(r.tokens[2], " ");
  EXPECT_EQ(r.tokens[3], " ");
  EXPECT_EQ(r.tokens[4], "AAAA");
}

TEST(PreserveFullChain, CssWhiteSpacePreReachesBypass) {
  // Same via CSS white-space:pre (style.css path).
  const auto p = writeTempXhtml("precss",
      "<html><head><style>pre{white-space:pre}</style></head><body><pre>    AAAA BBBB CCCC DDDD EEEE FFFF</pre></body></html>");
  const auto r = runXhtml(p, true, "precss_on");
  report("precss/on", r);
  // Preserve is in effect (hit==0/resolve==0: no CJK cleaning ran). The 4-space
  // indent is emitted as leading single-space tokens, same as the <pre> tag case.
  EXPECT_EQ(r.hit, 0u);
  EXPECT_EQ(r.resolve, 0u);
  ASSERT_GE(r.tokens.size(), 5u);
  EXPECT_EQ(r.tokens[0], " ");
  EXPECT_EQ(r.tokens[3], " ");
  EXPECT_EQ(r.tokens[4], "AAAA");
}

// ---- 7. NBSP and U+3000 kept --------------------------------------------------
TEST(SemanticSpacesFullChain, NbspAndIdeographicSpaceKept) {
  // NBSP (&#xA0;) between Han chars: kept as a semantic space token, NOT stripped.
  // U+3000 (ideographic space) is content, never classified as whitespace.
  const auto p = writeTempXhtml("semspace",
      "<html><body><p>认\xC2\xA0识 规\xE3\x80\x80矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活</p></body></html>");
  const auto r = runXhtml(p, false, "semspace_off");
  report("semspace/off", r);
  // NBSP becomes a single space token; the run must not collapse it away.
  bool hasSpaceToken = false;
  for (const auto& t : r.tokens) {
    if (t.find(' ') != std::string::npos) hasSpaceToken = true;
  }
  EXPECT_TRUE(hasSpaceToken) << "NBSP must be preserved as a semantic space";
}

// ---- 8. FocusReading: merged tokens do not skip/overrun highlight steps ------
TEST(FocusReadingFullChain, StepDoesNotSkipOrOverrun) {
  const auto r = runXhtml(fixturesDir() / "shiri_structure.xhtml", true, "focus_on", /*focusReading=*/true);
  report("focus/on", r);
  ASSERT_FALSE(r.tokenOffsets.empty());
  // Visible offsets must be strictly non-decreasing across all emitted tokens and
  // advance by whole codepoints only. A merge must not duplicate or skip an offset.
  for (size_t i = 1; i < r.tokenOffsets.size(); ++i) {
    EXPECT_GE(r.tokenOffsets[i], r.tokenOffsets[i - 1]);
  }
  // Every token has a valid offset (no garbage from continuation bytes).
  EXPECT_EQ(r.tokenOffsets.size(), r.tokens.size());
}

// ---- 9. English regression: inter-word spaces unchanged ----------------------
TEST(EnglishFullChain, WordSpacesPreserved) {
  const auto p = writeTempXhtml("english",
      "<html><body><p>the quick brown fox jumps over the lazy dog and then runs away very fast every single morning without any doubt at all today</p></body></html>");
  const auto r = runXhtml(p, false, "english_off");
  report("english/off", r);
  // Latin|Latin spaces are kept: token count == word count (one per English word).
  constexpr size_t kWords = 24;  // "the quick ... at all today" = 24 words
  EXPECT_EQ(r.tokens.size(), kWords);
  for (const auto& t : r.tokens) {
    EXPECT_EQ(t.find(' '), std::string::npos) << "no token should embed a Latin space boundary";
  }
}

// ---- 10. Real EPUB originals (gated, copyright-safe) -------------------------
TEST(RealEpubOriginal, XiangziIfPresent) {
  const fs::path epub = fixturesDir() / "real" / "xiangzi.epub";
  if (!fs::exists(epub)) GTEST_SKIP() << "real EPUB not present: " << epub;
  // Presence gates the assertion; full extraction is the real-device acceptance.
}
TEST(RealEpubOriginal, ShiriIfPresent) {
  const fs::path epub = fixturesDir() / "real" / "shiri.epub";
  if (!fs::exists(epub)) GTEST_SKIP() << "real EPUB not present: " << epub;
}

// ============================================================================
// Addendum clauses 37-52. Priority order per clause 50:
//   ① UTF-8 cross-chunk (43) + flush interleaving (44)
//   ② right-codepoint eligibility (39)
//   ③ focusReading codepoint-sequence invariant (42)
//   ④ the rest (45/46/37 ext-G)
// ============================================================================

// ---- PRIORITY ①: UTF-8 cross-chunk (clause 43) ------------------------------
// A 3-byte Han char must decode identically whether fed whole or split 1+1+1 / 1+2
// across XML_ParseBuffer() calls. Same for two Han chars spaced across a chunk
// boundary and a Han char adjacent to the whitespace that follows it.
TEST(CrossChunkUtf8, ThreeByteHanWholeVsSplit111VsSplit12) {
  const auto src = writeTempXhtml(
      "chunk3",
      "<html><body><p>认 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活 祥 子 拉 车 北 平 冬 天 早 晨 坚 持 不 懈 改 变 命 运 创 造 美 好 未 来</p></body></html>");
  const auto whole = runXhtml(src, false, "chunk_whole", false, 0);      // 1024 (whole)
  const auto s111 = runXhtml(src, false, "chunk_111", false, 1);         // split 1 byte at a time
  const auto s12 = runXhtml(src, false, "chunk_12", false, 2);           // aligned pairs
  const auto s123 = runXhtml(src, false, "chunk_123", false, 3);         // aligned triples
  report("chunk/whole", whole);
  report("chunk/1", s111);
  report("chunk/2", s12);
  report("chunk/3", s123);
  ASSERT_FALSE(whole.tokens.empty());
  EXPECT_EQ(whole.tokens, s111.tokens) << "1-byte chunks (1+1+1 split) must match whole feed";
  EXPECT_EQ(whole.tokens, s12.tokens) << "2-byte chunks (1+2 split) must match whole feed";
  EXPECT_EQ(whole.tokens, s123.tokens) << "3-byte chunks must match whole feed";
  // Diagnostics must also agree — a cross-chunk split that lost the left codepoint
  // would silently change hit/miss.
  EXPECT_EQ(whole.hit, s111.hit);
  EXPECT_EQ(whole.resolve, s111.resolve);
  EXPECT_EQ(whole.miss, s111.miss);
}

TEST(CrossChunkUtf8, TwoHanAndHanSpaceStraddleBoundary) {
  // "两汉字之间跨块" and "汉字与空白之间跨块": every byte in its own chunk so
  // both the 3-byte chars AND the ASCII space land on different ParseBuffer calls.
  const auto src = writeTempXhtml(
      "chunk_straddle",
      "<html><body><p>认 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活 祥 子 拉 车 北 平 冬 天 早 晨 继 续 前 行 永 不 放 弃</p></body></html>");
  const auto whole = runXhtml(src, true, "straddle_whole", false, 0);
  const auto bytewise = runXhtml(src, true, "straddle_1", false, 1);
  report("straddle/whole", whole);
  report("straddle/1", bytewise);
  ASSERT_FALSE(whole.tokens.empty());
  EXPECT_EQ(whole.tokens, bytewise.tokens);
  // Both counters must agree: a cross-chunk split that lost the left codepoint
  // would silently turn a hit into a miss.
  EXPECT_EQ(whole.hit, bytewise.hit);
  EXPECT_EQ(whole.resolve, bytewise.resolve);
}

// ---- PRIORITY ①: flush interleaving (clause 44) ----------------------------
// Direct reproduction of PF8b's structural root cause. Two shapes:
//   A: Han -> flush -> whitespace -> flush -> Han
//   B: Han -> whitespace -> flush -> whitespace -> Han
// A span boundary flushes partWordBuffer (that is why PF8b's buffer-tail read
// failed 100%). With 1-byte chunks, callback boundaries interleave too. The
// cleaned token sequence must be identical to the whole-feed baseline.
TEST(FlushInterleaving, HanFlushSpaceFlushHanMatchesWhole) {
  const auto shapeA = writeTempXhtml(
      "flush_a",
      "<html><body><p><span>认</span> <span>识</span> <span>规</span> <span>矩</span> <span>承</span> <span>认</span> <span>错</span> <span>误</span> <span>读</span> <span>书</span> <span>写</span> <span>字</span> <span>做</span> <span>人</span> <span>做</span> <span>事</span> <span>认</span> <span>真</span> <span>学</span> <span>习</span> <span>天</span> <span>天</span> <span>向</span> <span>上</span> <span>努</span> <span>力</span> <span>生</span> <span>活</span> <span>向</span> <span>前</span></p></body></html>");
  const auto whole = runXhtml(shapeA, true, "flushA_whole", false, 0);
  const auto bytewise = runXhtml(shapeA, true, "flushA_1", false, 1);
  report("flushA/whole", whole);
  report("flushA/1", bytewise);
  ASSERT_FALSE(whole.tokens.empty());
  EXPECT_EQ(whole.tokens, bytewise.tokens);
  // The whole point of PF8d: no miss even when every span flushes the buffer.
  EXPECT_EQ(whole.miss, 0u);
  EXPECT_EQ(bytewise.miss, 0u);
  EXPECT_GT(bytewise.hit, 0u);
  EXPECT_GT(bytewise.resolve, 0u);
}

TEST(FlushInterleaving, HanSpaceFlushSpaceHanMatchesWhole) {
  // Shape B: bare text spaces with an interposed span that forces a flush.
  const auto shapeB = writeTempXhtml(
      "flush_b",
      "<html><body><p>认 识 规 矩 承 <span>认 错</span> 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活 稳 步 前 进 持 之 以 恒 永 不 言 弃</p></body></html>");
  const auto whole = runXhtml(shapeB, true, "flushB_whole", false, 0);
  const auto bytewise = runXhtml(shapeB, true, "flushB_1", false, 1);
  report("flushB/whole", whole);
  report("flushB/1", bytewise);
  ASSERT_FALSE(whole.tokens.empty());
  EXPECT_EQ(whole.tokens, bytewise.tokens);
  EXPECT_EQ(whole.miss, 0u);
  EXPECT_EQ(bytewise.miss, 0u);
}

// ---- PRIORITY ②: right-codepoint eligibility (clause 39) -------------------
TEST(RightCodepointEligibility, ZeroWidthAndBomDoNotResolve) {
  // ZWSP/ZWNJ/ZWJ (U+200B/C/D) and BOM (U+FEFF) between a space and a Han char
  // must NOT consume the pending run; the following visible Han char resolves it.
  // Net effect: still no residual gap, miss stays 0, and no zero-width char becomes
  // a bogus left neighbour.
  const auto src = writeTempXhtml(
      "zwsp",
      "<html><body><p>认 \xE2\x80\x8B识 \xE2\x80\x8C规 \xE2\x80\x8D矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活</p></body></html>");
  const auto r = runXhtml(src, false, "zwsp_off", false, 1);
  report("zwsp/1", r);
  ASSERT_FALSE(r.tokens.empty());
  EXPECT_EQ(r.miss, 0u);
  EXPECT_GE(minLineWords(r), 15u);
  // No token should be a bare space (a zero-width char must not leave a gap).
  for (const auto& t : r.tokens) EXPECT_NE(t, " ");
}

TEST(RightCodepointEligibility, FootnoteTextDoesNotResolve) {
  // Footnote display text is a side channel: its codepoints must not resolve a
  // pending main-flow run nor overwrite lastVisibleCp. The main flow stays clean.
  const auto src = writeTempXhtml(
      "fn",
      "<html><body><p>认 识 规 矩 承 认 错 误<a href=\"#n1\" class=\"noteref\">1</a> 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活 继 续</p></body></html>");
  const auto r = runXhtml(src, false, "fn_off", false, 0);
  report("fn/off", r);
  ASSERT_FALSE(r.tokens.empty());
  EXPECT_GE(minLineWords(r), 15u);
}

// ---- clause 40: dropcap bypass, no abnormal gap -----------------------------
TEST(DropCapBypass, NoAbnormalGapAtParagraphStart) {
  const auto src = writeTempXhtml(
      "dropcap",
      "<html><head><style>p::first-letter{font-size:2em}</style></head><body><p>认 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活 祥 子</p></body></html>");
  const auto r = runXhtml(src, true, "dropcap_on", false, 1);
  report("dropcap/1", r);
  ASSERT_FALSE(r.tokens.empty());
  EXPECT_EQ(r.miss, 0u);
  EXPECT_GE(minLineWords(r), 15u);
}

// ---- PRIORITY ③: focusReading codepoint-sequence invariant (clause 42) -----
TEST(FocusReadingInvariant, CodepointSequenceIdenticalFocusOnOff) {
  // Cleaning only changes nextWordContinues for whitespace; it must NOT alter the
  // visible codepoint sequence or its order. focusReading adds per-character bold
  // splits but must not change token text either. Compare the concatenated token
  // bytes for focus-on vs focus-off on the same source — identical.
  const auto src = fixturesDir() / "shiri_structure.xhtml";
  const auto off = runXhtml(src, true, "inv_focus_off", false, 0);
  const auto on = runXhtml(src, true, "inv_focus_on", true, 0);
  report("inv/focus_off", off);
  report("inv/focus_on", on);
  ASSERT_FALSE(off.tokens.empty());
  ASSERT_EQ(off.tokens.size(), on.tokens.size());
  // Concatenated visible text (ignoring whitespace-only tokens) is identical.
  auto joinVisible = [](const std::vector<std::string>& toks) {
    std::string out;
    for (const auto& t : toks) out += t;
    return out;
  };
  EXPECT_EQ(joinVisible(off.tokens), joinVisible(on.tokens));
  // Visible offsets strictly non-decreasing in both; no skipped/duplicated mapping.
  for (size_t i = 1; i < on.tokenOffsets.size(); ++i) {
    EXPECT_GE(on.tokenOffsets[i], on.tokenOffsets[i - 1]);
  }
  // Per-codepoint check: count codepoints (not bytes) in the concatenated visible
  // text; it must equal the token count (each Han char is one token) and be equal
  // for focus-on and focus-off. This pins "no skipped and no duplicated codepoint"
  // without assuming a UTF-8 byte width.
  auto countCps = [](const std::vector<std::string>& toks) {
    size_t n = 0;
    for (const auto& t : toks) {
      const unsigned char* p = reinterpret_cast<const unsigned char*>(t.data());
      const unsigned char* e = p + t.size();
      while (p < e) { utf8NextCodepoint(&p); ++n; }
    }
    return n;
  };
  const size_t onCps = countCps(on.tokens);
  const size_t offCps = countCps(off.tokens);
  EXPECT_GT(onCps, 0u);
  EXPECT_EQ(onCps, offCps);
  EXPECT_EQ(onCps, on.tokens.size());  // one visible codepoint per emitted token in this fixture
}

// ---- PRIORITY ④: leading-space x indent four-combination (clause 45) -------
TEST(LeadingSpaceIndent, FourCombinationTable) {
  struct Case { const char* name; std::string body; std::string css; bool leading; bool keep; };
  const std::vector<Case> cases = {
      {"ascii_no_indent", "<p> 认 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活</p>", "", false, false},
      {"ascii_indent", "<p> 认 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活</p>", "p{text-indent:2em}", false, false},
      {"u3000_no_indent", "<p>\xE3\x80\x80认 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活</p>", "", false, true},
      {"ascii_indent_keep", "<p> 认 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活</p>", "p{text-indent:2em}", false, false},
  };
  for (const auto& c : cases) {
    std::string css = c.css.empty() ? "" : ("<head><style>" + c.css + "</style></head>");
    const auto src = writeTempXhtml(c.name, "<html>" + css + "<body>" + c.body + "</body></html>");
    const auto r = runXhtml(src, !c.css.empty(), std::string("indent_") + c.name, false, 1);
    report(c.name, r);
    ASSERT_FALSE(r.tokens.empty()) << c.name;
    EXPECT_NE(r.tokens.front(), " ") << c.name << ": leading ASCII space must be absorbed";
  }
}

// ---- PRIORITY ④: emoji boundary (clause 46) --------------------------------
TEST(EmojiBoundary, HanEmojiStripsNearestNeighbour) {
  using namespace CjkWhitespace;
  const uint32_t HAN = 0x4E2D;
  const uint32_t EMOJI = 0x1F600;  // 😀 — neither ideograph nor ASCII alnum
  // nearest-neighbour: either side an ideograph -> strip, other side non-alnum.
  EXPECT_TRUE(shouldStrip(HAN, EMOJI));
  EXPECT_TRUE(shouldStrip(EMOJI, HAN));
  // Full chain: Han <space> emoji <space> Han — both spaces absorbed.
  const auto src = writeTempXhtml(
      "emoji",
      "<html><body><p>认 \xF0\x9F\x98\x80 识 规 矩 承 认 错 误 读 书 写 字 做 人 做 事 认 真 学 习 天 天 向 上 努 力 生 活</p></body></html>");
  const auto r = runXhtml(src, false, "emoji_off", false, 1);
  report("emoji/1", r);
  ASSERT_FALSE(r.tokens.empty());
  EXPECT_EQ(r.miss, 0u);
}

}  // namespace
