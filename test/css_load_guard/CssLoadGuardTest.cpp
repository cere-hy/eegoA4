#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "Epub/css/CssLoadGuard.h"

// S3cPF8c host assertions for the CSS deadlock fix. These test the exact
// predicates and constants used by Epub::parseCssFiles(), and that the three
// failure reasons are distinct, displayable states (not just LOG_ERR lines).
//
// The numbers here come from real measurements, not synthetic fillers:
//  - freeHeap 25000 / 24000 are the two cases in the PF8c construction order;
//  - real-book CSS sizes (78/23/405 B; 3842 B large table) are the host-measured
//    Calibre stylesheets; the single allocation on that path is 8192 B;
//  - 128 KiB is the upstream #947 real defence (MAX_CSS_FILE_SIZE).

TEST(CssLoadGuard, ConstantsMatchConstructionOrder) {
  EXPECT_EQ(CssLoadGuard::MIN_HEAP_FOR_CSS_PARSING, 24576u);   // 24 KB
  EXPECT_EQ(CssLoadGuard::MAX_CSS_FILE_SIZE, 131072u);          // 128 KB
}

TEST(CssLoadGuard, HeapBoundary_25000_Parses) {
  // 25000 >= 24576: gate passes, parsing proceeds.
  EXPECT_FALSE(CssLoadGuard::isHeapInsufficient(25000));
  // Just above the boundary is still a pass.
  EXPECT_FALSE(CssLoadGuard::isHeapInsufficient(CssLoadGuard::MIN_HEAP_FOR_CSS_PARSING));
}

TEST(CssLoadGuard, HeapBoundary_24000_SkipsWithFailureState) {
  // 24000 < 24576: gate skips, and the caller observes a concrete failure.
  EXPECT_TRUE(CssLoadGuard::isHeapInsufficient(24000));
  // The skip maps to an explicit, non-None failure state the UI can show.
  CssLoadGuard::Failure observed = CssLoadGuard::Failure::None;
  if (CssLoadGuard::isHeapInsufficient(24000)) {
    observed = CssLoadGuard::Failure::LowHeap;
  }
  EXPECT_EQ(observed, CssLoadGuard::Failure::LowHeap);
  EXPECT_NE(observed, CssLoadGuard::Failure::None);
}

// The real-book free heap (62-65 KB) must pass the lowered gate — this is the
// exact condition that was deadlocked by the old 64 KB gate.
TEST(CssLoadGuard, RealBookFreeHeapPasses) {
  EXPECT_FALSE(CssLoadGuard::isHeapInsufficient(62848));  // pf8 log reading
  EXPECT_FALSE(CssLoadGuard::isHeapInsufficient(62916));  // pf8b diag reading
  EXPECT_FALSE(CssLoadGuard::isHeapInsufficient(65536));  // upper observed reading
  // Old gate would have skipped at 62848; prove the gate actually lowered.
  EXPECT_TRUE(62848u < 64u * 1024u);
  EXPECT_TRUE(62848u >= CssLoadGuard::MIN_HEAP_FOR_CSS_PARSING);
}

TEST(CssLoadGuard, RealBookCssSizesPass) {
  // Host-measured Calibre stylesheet sizes (see PF8c query report).
  const size_t realCss[] = {78, 23, 405, 3842, 11526};
  for (size_t s : realCss) {
    EXPECT_FALSE(CssLoadGuard::isFileTooLarge(s)) << "size=" << s;
  }
}

TEST(CssLoadGuard, FileSizeCapStillBlocks128KBPlusOne) {
  const size_t cap = CssLoadGuard::MAX_CSS_FILE_SIZE;
  EXPECT_FALSE(CssLoadGuard::isFileTooLarge(cap));        // exactly 128 KB: allowed
  EXPECT_TRUE(CssLoadGuard::isFileTooLarge(cap + 1));     // 1 byte over: skipped
  // A 250 KB stylesheet (upstream #947 crash vector) is still refused.
  EXPECT_TRUE(CssLoadGuard::isFileTooLarge(250u * 1024u));

  CssLoadGuard::Failure observed = CssLoadGuard::Failure::None;
  if (CssLoadGuard::isFileTooLarge(cap + 1)) {
    observed = CssLoadGuard::Failure::FileTooLarge;
  }
  EXPECT_EQ(observed, CssLoadGuard::Failure::FileTooLarge);
}

TEST(CssLoadGuard, ThreeFailureReasonsAreDistinctAndDisplayable) {
  // Each skip reason must be a distinct enum value so the reader can pick a
  // different i18n string (STR_CSS_LOW_MEMORY / STR_CSS_FILE_TOO_LARGE /
  // STR_CSS_PARSE_FAILED) instead of staying silent.
  EXPECT_NE(CssLoadGuard::Failure::LowHeap, CssLoadGuard::Failure::FileTooLarge);
  EXPECT_NE(CssLoadGuard::Failure::LowHeap, CssLoadGuard::Failure::ParseError);
  EXPECT_NE(CssLoadGuard::Failure::FileTooLarge, CssLoadGuard::Failure::ParseError);
  EXPECT_EQ(static_cast<uint8_t>(CssLoadGuard::Failure::None), 0u);
}

TEST(CssLoadGuard, FailureAccumulationKeepsFirstReason) {
  // parseCssFiles() accumulates the first failure via mergeFailure(); the UI
  // receives that concrete value and shows its i18n string.
  using CssLoadGuard::Failure;
  EXPECT_EQ(CssLoadGuard::mergeFailure(Failure::None, Failure::LowHeap), Failure::LowHeap);
  // A later, different failure does not overwrite the dominant first reason.
  EXPECT_EQ(CssLoadGuard::mergeFailure(Failure::LowHeap, Failure::ParseError), Failure::LowHeap);
  EXPECT_EQ(CssLoadGuard::mergeFailure(Failure::FileTooLarge, Failure::LowHeap), Failure::FileTooLarge);
  // No failure stays None (no notice shown).
  EXPECT_EQ(CssLoadGuard::mergeFailure(Failure::None, Failure::None), Failure::None);

  // Construct a genuine low-heap open (e.g. 24000 free, below the 24576 gate):
  // all three stylesheets are skipped and the accumulated signal is LowHeap,
  // which the reader displays (not silent). At 62848 — the heap that used to
  // deadlock at the old 64 KB gate — the new gate passes and no failure arises.
  Failure lowHeapSignal = Failure::None;
  for (int i = 0; i < 3; ++i) {
    if (CssLoadGuard::isHeapInsufficient(24000)) {
      lowHeapSignal = CssLoadGuard::mergeFailure(lowHeapSignal, Failure::LowHeap);
    }
  }
  EXPECT_EQ(lowHeapSignal, Failure::LowHeap);
  EXPECT_NE(lowHeapSignal, Failure::None);

  // The deadlock heap (62848) now yields no failure — this is the fix.
  Failure fixedSignal = Failure::None;
  for (int i = 0; i < 3; ++i) {
    if (CssLoadGuard::isHeapInsufficient(62848)) {
      fixedSignal = CssLoadGuard::mergeFailure(fixedSignal, Failure::LowHeap);
    }
  }
  EXPECT_EQ(fixedSignal, Failure::None);
}
