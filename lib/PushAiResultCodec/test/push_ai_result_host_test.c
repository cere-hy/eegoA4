/*
 * 0x07 AI 释义接收核 host 断言（batch B5，纯主机侧判据，无 ESP-IDF/无硬件）。
 * 覆盖：白皮书 §2.2 权威字段解码（带头/裸帧/拒收边界）、UTF-8 边界、
 * 上屏追加/回滚、hex 留证，以及双槽+代际号的 pthread 并发不撕裂断言。
 * ASan + UBSan 全开。
 */
#include "push_ai_result.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
static int red = 0;
#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { ++red; fprintf(stderr, "RED: %s (line %d)\n", (msg), __LINE__); } \
} while (0)

static uint16_t wr_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
    return v;
}

/* 按白皮书 §2.2 造一帧带头 241B 0x07：head + wlen + word[64] + dlen + def[170]。 */
static size_t make_frame(uint8_t* f, const char* word, const char* def, int bare) {
    memset(f, 0, PAR_FRAME_LEN);
    uint8_t* p = f;
    size_t off = 0;
    if (!bare) {
        p[0] = PAR_MAGIC0; p[1] = PAR_MAGIC1;
        p[2] = PAR_OPCODE; p[3] = PAR_VERSION;
        off = PAR_HDR_LEN;
    }
    const size_t wn = strlen(word);
    const size_t dn = strlen(def);
    p[off + PAR_F_WORD_LEN] = (uint8_t)wn;
    memcpy(p + off + PAR_F_WORD, word, wn);
    wr_u16(p + off + PAR_F_DEF_LEN, (uint16_t)dn);
    memcpy(p + off + PAR_F_DEF, def, dn);
    return bare ? (PAR_FIELD_LEN) : (PAR_FRAME_LEN);
}

static void test_decode(void) {
    uint8_t f[PAR_FRAME_LEN];
    par_result_t r;

    /* 向量1：带头帧 hello / definition。 */
    size_t n = make_frame(f, "hello", "definition", 0);
    size_t fo = par_frame_offset(f, n);
    CHECK(fo == PAR_HDR_LEN, "headed frame offset==4");
    CHECK(par_decode(f, n, fo, &r) == 1, "headed frame decodes");
    CHECK(strcmp(r.word, "hello") == 0, "word==hello");
    CHECK(strcmp(r.def, "definition") == 0, "def==definition");
    CHECK(r.word_len == 5 && r.def_len == 10, "lengths 5/10");

    /* 向量2：裸帧（老路/直喂），从 0 起。 */
    n = make_frame(f, "apple", "a fruit", 1);
    fo = par_frame_offset(f, n);
    CHECK(fo == 0, "bare frame offset==0");
    CHECK(par_decode(f, n, fo, &r) == 1, "bare frame decodes");
    CHECK(strcmp(r.word, "apple") == 0 && strcmp(r.def, "a fruit") == 0, "bare word/def");

    /* 向量3：坏魔数拒收（带头模式下 decode 校验 frame[0..3]）。 */
    n = make_frame(f, "hello", "definition", 0);
    f[0] = 0x00;
    CHECK(par_decode(f, n, PAR_HDR_LEN, &r) == 0, "bad magic rejected by decode");

    /* 向量4：坏 opcode / 版本拒收。 */
    n = make_frame(f, "hello", "definition", 0);
    f[2] = 0x08;
    CHECK(par_decode(f, n, PAR_HDR_LEN, &r) == 0, "bad opcode rejected");
    f[2] = PAR_OPCODE; f[3] = 0x02;
    CHECK(par_decode(f, n, PAR_HDR_LEN, &r) == 0, "bad version rejected");

    /* 向量5：word_len==64 拒收（>=slot）。 */
    n = make_frame(f, "w", "d", 0);
    f[PAR_HDR_LEN + PAR_F_WORD_LEN] = 64;
    CHECK(par_decode(f, n, PAR_HDR_LEN, &r) == 0, "word_len==64 rejected");

    /* 向量6：def_len==170 拒收（装不下 NUL，D-1 事故同款）。 */
    n = make_frame(f, "w", "d", 0);
    wr_u16(f + PAR_HDR_LEN + PAR_F_DEF_LEN, 170);
    CHECK(par_decode(f, n, PAR_HDR_LEN, &r) == 0, "def_len==170 rejected");

    /* 向量7：短帧拒收。 */
    CHECK(par_frame_offset(f, PAR_HDR_LEN - 1) == (size_t)-1, "short frame rejected");
}

static void test_utf8(void) {
    const char* han = "\xE4\xB8\xAD\xE6\x96\x87"; /* 中文 6B */
    CHECK(par_utf8_clamp(han, 6, 6) == 6, "utf8 clamp full 6B");
    CHECK(par_utf8_clamp(han, 6, 5) == 3, "utf8 clamp cuts at 3B boundary");
    CHECK(par_utf8_clamp(han, 6, 4) == 3, "utf8 clamp 4->3 boundary");
    CHECK(par_utf8_clamp(han, 6, 2) == 0, "utf8 clamp 2->0 (half han rejected)");
    const char* ascii = "abcdef";
    CHECK(par_utf8_clamp(ascii, 6, 3) == 3, "ascii clamp 3");
}

static void test_screen(void) {
    par_screen_t s;
    par_screen_init(&s);
    par_result_t r;
    memset(&r, 0, sizeof(r));

    strcpy(r.def, "first"); r.def_len = 5;
    CHECK(par_screen_apply(&s, 1, &r) == 1, "screen gen1 append ok");
    CHECK(strcmp(s.text, "first") == 0, "screen text==first");
    CHECK(s.reorders == 1, "reorder count 1");
    CHECK(par_screen_apply(&s, 1, &r) == 0, "screen replay gen1 dropped");

    strcpy(r.def, "second"); r.def_len = 6;
    CHECK(par_screen_apply(&s, 2, &r) == 1, "screen gen2 append ok");
    CHECK(strcmp(s.text, "firstsecond") == 0, "screen text appended");

    /* 回滚：把 last_gen 推高后塞超长 def 不可能，改测 def_len==0。 */
    r.def_len = 0;
    CHECK(par_screen_apply(&s, 3, &r) == 0, "screen empty def rollback");
    CHECK(strcmp(s.text, "firstsecond") == 0, "screen text intact after rollback");
    CHECK(s.rollbacks == 1, "rollback count 1");
}

static void test_hexdump(void) {
    uint8_t f[64];
    for (size_t i = 0; i < sizeof(f); ++i) f[i] = (uint8_t)i;
    char out[512];
    size_t n = par_hexdump(f, sizeof(f), out, sizeof(out));
    CHECK(n > 0 && out[n] == '\0', "hexdump nul-terminated");
    CHECK(strlen(out) == 2 * 64 + 1, "hexdump 2 hex/byte + 1 newline");
}

/* ---- 并发不撕裂断言 ---- */
#define CONCURRENT_FRAMES 20000
#define CONSUMER_ROUNDS   60000
static par_dslot_t* g_slot;
static volatile int g_producer_done = 0;

static void* producer(void* arg) {
    (void)arg;
    uint8_t f[PAR_FRAME_LEN];
    for (int i = 1; i <= CONCURRENT_FRAMES; ++i) {
        /* 每帧 241B 全填同一个哨值 = i&0xFF；撕裂会出现两种哨值混杂。 */
        memset(f, (int)(i & 0xFFu), sizeof(f));
        par_dslot_publish(g_slot, f, sizeof(f));
    }
    g_producer_done = 1;
    return NULL;
}

static void test_concurrent(void) {
    g_slot = par_dslot_create();
    CHECK(g_slot != NULL, "dslot create");
    if (g_slot == NULL) return;

    pthread_t th;
    pthread_create(&th, NULL, producer, NULL);
    uint8_t snap[PAR_FRAME_LEN];
    size_t len = 0;
    uint32_t gen = 0, prev = 0;
    int torn = 0, nonmono = 0, reads = 0;
    for (int i = 0; i < CONSUMER_ROUNDS; ++i) {
        const uint32_t g = par_dslot_take_latest(g_slot, snap, &len, &gen);
        if (g == 0) continue;
        ++reads;
        if (len != PAR_FRAME_LEN) { ++torn; continue; }
        /* 自洽：整帧每字节必须等于首字节（单哨值帧）。 */
        const uint8_t sentinel = snap[0];
        for (size_t k = 1; k < len; ++k) {
            if (snap[k] != sentinel) { ++torn; break; }
        }
        if (gen < prev) ++nonmono;
        prev = gen;
    }
    pthread_join(th, NULL);
    CHECK(g_producer_done == 1, "producer finished");
    CHECK(reads > 0, "consumer read frames");
    CHECK(torn == 0, "no torn frame in concurrent read");
    CHECK(nonmono == 0, "generation monotonic");
    CHECK(gen == CONCURRENT_FRAMES, "final gen == published count");
    par_dslot_destroy(g_slot);
}

int main(void) {
    test_decode();
    test_utf8();
    test_screen();
    test_hexdump();
    test_concurrent();
    printf("push_ai_result host test: %d checks, %d red\n", checks, red);
    return red == 0 ? 0 : 1;
}
