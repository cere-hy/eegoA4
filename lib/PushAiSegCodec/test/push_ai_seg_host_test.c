/*
 * PushAiSegCodec host 断言（方案乙 S1，纯主机侧判据，无 ESP-IDF/无硬件）。
 * 覆盖：0x08 权威字段解码（头/长度/序号边界）、编解码往返、ASCII 与 UTF-8
 * 片段边界、0x18 CAPS / 0x13 ERR 编码。ASan + UBSan 全开。
 */
#include "push_ai_seg.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks = 0;
static int red = 0;
#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { ++red; fprintf(stderr, "RED: %s (line %d)\n", (msg), __LINE__); } \
} while (0)

static uint16_t wr_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8); return v;
}

static void mk_frame(uint8_t* f, const char* word, uint16_t idx, uint16_t total, const char* seg) {
    memset(f, 0, PAS_FRAME_LEN);
    f[0] = PAS_MAGIC0; f[1] = PAS_MAGIC1; f[2] = PAS_OPCODE; f[3] = PAS_VERSION;
    size_t wn = strlen(word); f[4] = (uint8_t)wn; memcpy(f + 5, word, wn);
    const size_t off = 69;
    wr_u16(f + off, idx); wr_u16(f + off + 2, total);
    size_t dn = strlen(seg); wr_u16(f + off + 4, (uint16_t)dn);
    memcpy(f + off + 6, seg, dn);
}

int main(void) {
    uint8_t f[PAS_FRAME_LEN];
    pas_seg_t s;

    /* 1. 正常解码：word + idx + total + seg */
    mk_frame(f, "Personal", 1, 4, "\xe4\xb8\xaa\xe4\xba\xba"); /* 中文 UTF-8 片段 */
    CHECK(pas_seg_decode(f, sizeof(f), &s), "normal decode ok");
    CHECK(strcmp(s.word, "Personal") == 0, "word decoded");
    CHECK(s.seg_idx == 1 && s.seg_total == 4, "idx/total decoded");
    CHECK(s.dlen == 6, "dlen decoded");
    CHECK(strcmp(s.seg, "\xe4\xb8\xaa\xe4\xba\xba") == 0, "seg bytes decoded");

    /* 2. seq0 + total==1 单帧（等价 0x07） */
    mk_frame(f, "mindset", 0, 1, "a single definition");
    CHECK(pas_seg_decode(f, sizeof(f), &s), "single-frame decode ok");
    CHECK(s.seg_idx == 0 && s.seg_total == 1, "seq0 total1");
    CHECK(strcmp(s.seg, "a single definition") == 0, "single seg text");

    /* 3. 拒收：长度错 / 魔数错 / opcode 错 / version 错 */
    CHECK(!pas_seg_decode(f, PAS_FRAME_LEN - 1, &s), "reject short frame");
    f[0] = 0x00; CHECK(!pas_seg_decode(f, sizeof(f), &s), "reject bad magic0");
    f[0] = PAS_MAGIC0; f[2] = 0x07; CHECK(!pas_seg_decode(f, sizeof(f), &s), "reject wrong opcode");
    f[2] = PAS_OPCODE; f[3] = 0x02; CHECK(!pas_seg_decode(f, sizeof(f), &s), "reject bad version");

    /* 4. 拒收：序号越界 idx>=total / total==0 */
    mk_frame(f, "w", 2, 2, "x"); CHECK(!pas_seg_decode(f, sizeof(f), &s), "reject idx>=total");
    mk_frame(f, "w", 0, 0, "x"); CHECK(!pas_seg_decode(f, sizeof(f), &s), "reject total==0");

    /* 5. 拒收：dlen 越界（>=PAS_SEG_SLOT）*/
    mk_frame(f, "w", 0, 1, "x"); wr_u16(f + 73, PAS_SEG_SLOT);
    CHECK(!pas_seg_decode(f, sizeof(f), &s), "reject dlen overflow");

    /* 6. 拒收：word_len 越界 */
    mk_frame(f, "w", 0, 1, "x"); f[4] = PAS_WORD_SLOT;
    CHECK(!pas_seg_decode(f, sizeof(f), &s), "reject word_len overflow");

    /* 7. 编解码往返：ASCII + 满槽边界 */
    {
        pas_seg_t in;
        memset(&in, 0, sizeof(in));
        strcpy(in.word, "challenges");
        in.seg_idx = 3; in.seg_total = 5; in.dlen = 0;
        strcpy(in.seg, "the last UTF-8 safe slice here");
        uint8_t out[PAS_FRAME_LEN];
        CHECK(pas_seg_encode(&in, out, sizeof(out)) == PAS_FRAME_LEN, "encode returns 241");
        pas_seg_t back;
        CHECK(pas_seg_decode(out, sizeof(out), &back), "roundtrip decode");
        CHECK(strcmp(back.word, in.word) == 0, "roundtrip word");
        CHECK(back.seg_idx == in.seg_idx && back.seg_total == in.seg_total, "roundtrip idx/total");
        CHECK(strcmp(back.seg, in.seg) == 0, "roundtrip seg");
    }

    /* 8. 编码参数校验 */
    {
        pas_seg_t in; memset(&in, 0, sizeof(in));
        in.seg_total = 0;
        uint8_t out[PAS_FRAME_LEN];
        CHECK(pas_seg_encode(&in, out, sizeof(out)) == 0, "reject encode total==0");
    }

    /* 9. 0x18 CAPS / 0x13 ERR 编码 */
    {
        uint8_t st[8];
        CHECK(pas_caps_encode(PAS_CAP_AI_MULTI, st, sizeof(st)) == 5, "caps len 5");
        CHECK(st[0] == PAS_ST_CAPS, "caps opcode 0x18");
        CHECK(st[1] == (PAS_CAP_AI_MULTI & 0xFFu), "caps bit multi LSB");
        CHECK(pas_err_encode(2, st, sizeof(st)) == 2, "err len 2");
        CHECK(st[0] == PAS_ST_ERR && st[1] == 2, "err opcode+code");
        CHECK(pas_caps_encode(PAS_CAP_AI_MULTI, st, 4) == 0, "caps cap too small");
    }

    printf("push_ai_seg host test: %d checks, %d red\n", checks, red);
    return red == 0 ? 0 : 1;
}
