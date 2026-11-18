/*
 * ai_segasm host 断言（方案乙 S2）。覆盖：seq0 新条、顺序续帧完成、单帧
 * (total==1)、乱序 REJECT、词变 REJECT、total 变 REJECT、丢帧后错位 REJECT、
 * 重发整条不拼接、8192 边界 DONE、超 8192 OVERFLOW、OVERFLOW 后状态清零。
 * ASan + UBSan 全开。
 */
#include "ai_segasm.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks = 0;
static int red = 0;
#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { ++red; fprintf(stderr, "RED: %s (line %d)\n", (msg), __LINE__); } \
} while (0)

static pas_seg_t mk(const char* word, uint16_t idx, uint16_t total, const char* seg) {
    pas_seg_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.word, sizeof(s.word), "%s", word);
    s.seg_idx = idx;
    s.seg_total = total;
    snprintf(s.seg, sizeof(s.seg), "%s", seg);
    return s;
}

int main(void) {
    ai_segasm_t a;
    const char* text;
    size_t out_len;

    /* 1. 顺序 3 帧 → DONE，全文按字节拼接 */
    ai_segasm_reset(&a);
    {
        pas_seg_t s0 = mk("Personal", 0, 3, "AAA");
        CHECK(ai_segasm_feed(&a, &s0, &text, &out_len) == AI_SEGASM_PENDING, "s0 pending");
        CHECK(a.active && a.next == 1 && a.total == 3, "s0 state");
        pas_seg_t s1 = mk("Personal", 1, 3, "BBB");
        CHECK(ai_segasm_feed(&a, &s1, &text, &out_len) == AI_SEGASM_PENDING, "s1 pending");
        pas_seg_t s2 = mk("Personal", 2, 3, "CCC");
        CHECK(ai_segasm_feed(&a, &s2, &text, &out_len) == AI_SEGASM_DONE, "s2 done");
        CHECK(strcmp(text, "AAABBBCCC") == 0, "concatenated text");
        CHECK(out_len == 9, "concatenated len");
    }

    /* 2. total==1 单帧立即完成 */
    ai_segasm_reset(&a);
    {
        pas_seg_t s = mk("mindset", 0, 1, "only one slice");
        CHECK(ai_segasm_feed(&a, &s, &text, &out_len) == AI_SEGASM_DONE, "single frame done");
        CHECK(strcmp(text, "only one slice") == 0, "single frame text");
    }

    /* 3. seq==0 重发整条不拼接旧版 */
    ai_segasm_reset(&a);
    {
        pas_seg_t s0 = mk("w", 0, 2, "OLD");
        ai_segasm_feed(&a, &s0, &text, &out_len);
        pas_seg_t s1 = mk("w", 1, 2, "OLD2");
        ai_segasm_feed(&a, &s1, &text, &out_len);  /* 完成 OLDOLD2 */
        CHECK(strcmp(text, "OLDOLD2") == 0, "first item complete");
        pas_seg_t n0 = mk("w", 0, 1, "NEW");      /* 重发，idx=0 开新条 */
        CHECK(ai_segasm_feed(&a, &n0, &text, &out_len) == AI_SEGASM_DONE, "resend new item done");
        CHECK(strcmp(text, "NEW") == 0, "resend not spliced onto old");
        CHECK(a.len == 3, "resend len reset");
    }

    /* 4. 乱序 REJECT（跳帧后错位） */
    ai_segasm_reset(&a);
    {
        pas_seg_t s0 = mk("w", 0, 3, "A");
        ai_segasm_feed(&a, &s0, &text, &out_len);
        pas_seg_t s2 = mk("w", 2, 3, "C");  /* 期望 1，实到 2 */
        CHECK(ai_segasm_feed(&a, &s2, &text, &out_len) == AI_SEGASM_REJECT, "out-of-order reject");
        CHECK(!a.active && a.len == 0, "reject resets state");
    }

    /* 5. 词变 REJECT */
    ai_segasm_reset(&a);
    {
        pas_seg_t s0 = mk("wordA", 0, 2, "A");
        ai_segasm_feed(&a, &s0, &text, &out_len);
        pas_seg_t s1 = mk("wordB", 1, 2, "B");
        CHECK(ai_segasm_feed(&a, &s1, &text, &out_len) == AI_SEGASM_REJECT, "word-change reject");
    }

    /* 6. total 变 REJECT */
    ai_segasm_reset(&a);
    {
        pas_seg_t s0 = mk("w", 0, 3, "A");
        ai_segasm_feed(&a, &s0, &text, &out_len);
        pas_seg_t s1 = mk("w", 1, 4, "B");
        CHECK(ai_segasm_feed(&a, &s1, &text, &out_len) == AI_SEGASM_REJECT, "total-change reject");
    }

    /* 7. 8192 边界：恰好填满 → DONE，不出 OVERFLOW */
    ai_segasm_reset(&a);
    {
        char chunk[PAS_SEG_SLOT];
        memset(chunk, 'x', sizeof(chunk));
        chunk[PAS_SEG_EFF_MAX] = 0;             /* 165 有效字节 */
        uint16_t total = 49;
        unsigned filled = 0;
        ai_segasm_status_t st = AI_SEGASM_PENDING;
        for (uint16_t i = 0; i < total; ++i) {
            pas_seg_t s = mk("w", i, total, chunk);
            st = ai_segasm_feed(&a, &s, &text, &out_len);
            filled += PAS_SEG_EFF_MAX;
            if (st == AI_SEGASM_DONE) break;
        }
        /* 49*165 = 8085 <= 8192，应完成 */
        CHECK(st == AI_SEGASM_DONE, "8085B under 8192 completes");
        CHECK(out_len == 8085u, "8085B exact length");
        CHECK((void*)text == (void*)a.buf, "text points to buffer");
    }

    /* 8. 超 8192 → OVERFLOW 并重置（50 帧 *165 = 8250 > 8192） */
    ai_segasm_reset(&a);
    {
        char chunk[PAS_SEG_SLOT];
        memset(chunk, 'y', sizeof(chunk));
        chunk[PAS_SEG_EFF_MAX] = 0;
        uint16_t total = 50;
        ai_segasm_status_t st = AI_SEGASM_PENDING;
        for (uint16_t i = 0; i < total; ++i) {
            pas_seg_t s = mk("w", i, total, chunk);
            st = ai_segasm_feed(&a, &s, &text, &out_len);
            if (st == AI_SEGASM_OVERFLOW) break;
        }
        CHECK(st == AI_SEGASM_OVERFLOW, "8250B over 8192 overflows");
        CHECK(!a.active && a.len == 0 && a.total == 0, "overflow resets state");
    }

    /* 9. OVERFLOW 后新条可正常重建 */
    ai_segasm_reset(&a);
    {
        char chunk[PAS_SEG_SLOT];
        memset(chunk, 'y', sizeof(chunk)); chunk[PAS_SEG_EFF_MAX] = 0;
        for (uint16_t i = 0; i < 50; ++i) {
            pas_seg_t s = mk("w", i, 50, chunk);
            if (ai_segasm_feed(&a, &s, &text, &out_len) == AI_SEGASM_OVERFLOW) break;
        }
        pas_seg_t fresh = mk("w", 0, 1, "OK");
        CHECK(ai_segasm_feed(&a, &fresh, &text, &out_len) == AI_SEGASM_DONE, "new item after overflow");
        CHECK(strcmp(text, "OK") == 0, "new item text after overflow");
    }

    printf("ai_segasm host test: %d checks, %d red\n", checks, red);
    return red == 0 ? 0 : 1;
}
