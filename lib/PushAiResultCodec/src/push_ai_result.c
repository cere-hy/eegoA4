/* PushAiResultCodec — 0x07 receive core. See include/push_ai_result.h. */
#include "push_ai_result.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* 字段布局由白皮书 §2.2 冻结；逐字节闭合（字段区相对偏移）。 */
typedef char par_layout_check[
    (PAR_HDR_LEN + PAR_FIELD_LEN == PAR_FRAME_LEN &&
     PAR_F_WORD == PAR_F_WORD_LEN + 1u &&
     PAR_F_DEF_LEN == PAR_F_WORD + PAR_WORD_SLOT &&
     PAR_F_DEF == PAR_F_DEF_LEN + 2u &&
     PAR_F_DEF + PAR_DEF_SLOT == PAR_FIELD_LEN &&
     PAR_DEF_EFF_MAX + 1u == PAR_DEF_SLOT) ? 1 : -1];

static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* UTF-8 前导字节判读：返回该起始字节对应序列总长；非前导返回 0。 */
static size_t utf8_seq_len(uint8_t b) {
    if (b < 0x80u) return 1;
    if ((b & 0xE0u) == 0xC0u) return 2;
    if ((b & 0xF0u) == 0xE0u) return 3;
    if ((b & 0xF8u) == 0xF0u) return 4;
    return 0; /* continuation byte or invalid lead */
}

size_t par_utf8_clamp(const char* s, size_t n, size_t cap) {
    if (s == NULL || cap == 0) return 0;
    if (n > cap) n = cap;
    /* 向后退到不切断多字节序列的位置。 */
    size_t i = 0;
    while (i < n) {
        const size_t seq = utf8_seq_len((uint8_t)s[i]);
        if (seq == 0) return i;                 /* 孤立 continuation，截断于此 */
        if (i + seq > n) return i;              /* 序列跨过边界，截断于此 */
        i += seq;
    }
    return i;
}

size_t par_frame_offset(const uint8_t* frame, size_t len) {
    if (frame == NULL || len < PAR_HDR_LEN) return (size_t)-1;
    if (frame[0] == PAR_MAGIC0 && frame[1] == PAR_MAGIC1) {
        /* 带头帧必须整帧齐 241；opcode/version 在硬解里再校验。 */
        if (len < PAR_FRAME_LEN) return (size_t)-1;
        return PAR_HDR_LEN;
    }
    /* 裸帧：去掉 4B 头后的字段区 = 1+64+2+170 = 237B。 */
    if (len < PAR_FRAME_LEN - PAR_HDR_LEN) return (size_t)-1;
    return 0;
}

int par_decode(const uint8_t* frame, size_t len, size_t frame_off,
               par_result_t* out) {
    if (frame == NULL || out == NULL) return 0;
    if (frame_off != 0 && frame_off != PAR_HDR_LEN) return 0;
    const uint8_t* p = frame + frame_off;  /* 字段区起点 */
    const size_t avail = len - frame_off;

    if (frame_off == PAR_HDR_LEN) {
        /* 头在整帧起点 frame[0..3]，不在字段区 p。 */
        if (frame[0] != PAR_MAGIC0 || frame[1] != PAR_MAGIC1 ||
            frame[2] != PAR_OPCODE || frame[3] != PAR_VERSION) return 0;
    }
    if (avail < PAR_FIELD_LEN) return 0;

    const size_t wn = p[PAR_F_WORD_LEN];
    if (wn >= PAR_WORD_SLOT) return 0;
    const uint16_t dn = rd_u16(p + PAR_F_DEF_LEN);
    if (dn >= PAR_DEF_SLOT) return 0; /* dn==170 装不下 NUL，协议违规 */

    par_result_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.word, p + PAR_F_WORD, wn);
    tmp.word[wn] = '\0';
    memcpy(tmp.def, p + PAR_F_DEF, dn);
    tmp.def[dn] = '\0';
    tmp.word_len = (uint8_t)wn;
    tmp.def_len = dn;
    *out = tmp;
    return 1;
}

/* ---- 双槽 SPSC（per-slot seqlock）---- */
struct par_dslot {
    uint8_t slots[2][PAR_FRAME_LEN];
    size_t  lens[2];
    uint32_t gen[2];                 /* 受 seq[slot] 保护 */
    _Atomic uint32_t seq[2];         /* 偶=稳定，奇=写入中 */
    _Atomic uint32_t active;         /* 已提交槽索引 0/1 */
    _Atomic uint32_t generation;     /* 单调提交计数 */
};

par_dslot_t* par_dslot_create(void) {
    par_dslot_t* d = (par_dslot_t*)calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    atomic_init(&d->seq[0], 0u);
    atomic_init(&d->seq[1], 0u);
    atomic_init(&d->active, 0u);
    atomic_init(&d->generation, 0u);
    return d;
}

void par_dslot_destroy(par_dslot_t* d) { free(d); }

uint32_t par_dslot_publish(par_dslot_t* d, const uint8_t* frame, size_t len) {
    if (d == NULL || frame == NULL || len == 0 || len > PAR_FRAME_LEN) return 0;
    const uint32_t act = atomic_load_explicit(&d->active, memory_order_acquire);
    const uint32_t w = act ^ 1u;               /* 只写非活跃槽 */
    atomic_fetch_add_explicit(&d->seq[w], 1u, memory_order_relaxed); /* →奇：写入中 */
    atomic_thread_fence(memory_order_release);

    memcpy(d->slots[w], frame, len);
    d->lens[w] = len;
    const uint32_t gen = atomic_fetch_add_explicit(&d->generation, 1u,
                                                   memory_order_relaxed) + 1u;
    d->gen[w] = gen;

    atomic_store_explicit(&d->active, w, memory_order_release);      /* 提交槽 */
    atomic_fetch_add_explicit(&d->seq[w], 1u, memory_order_release); /* →偶：稳定 */
    return gen;
}

uint32_t par_dslot_take_latest(par_dslot_t* d, uint8_t* out_frame,
                               size_t* out_len, uint32_t* out_gen) {
    if (d == NULL) return 0;
    for (;;) {
        const uint32_t slot = atomic_load_explicit(&d->active, memory_order_acquire);
        const uint32_t s1 = atomic_load_explicit(&d->seq[slot], memory_order_acquire);
        if (s1 & 1u) continue;                  /* 写入中，重试 */
        const uint32_t gen = d->gen[slot];
        if (gen == 0) { if (out_gen) *out_gen = 0; return 0; } /* 尚无帧 */
        if (out_frame != NULL && out_len != NULL) {
            const size_t len = d->lens[slot];
            memcpy(out_frame, d->slots[slot], len);
            *out_len = len;
        }
        const uint32_t s2 = atomic_load_explicit(&d->seq[slot], memory_order_acquire);
        if (s1 == s2) {                         /* 未被写侧打断→快照一致 */
            if (out_gen) *out_gen = gen;
            return gen;
        }
        /* seq 变化：读到了撕裂候选，丢弃重试。 */
    }
}

/* ---- 上屏决策 ---- */
void par_screen_init(par_screen_t* s) {
    memset(s, 0, sizeof(*s));
}

int par_screen_apply(par_screen_t* s, uint32_t gen, const par_result_t* r) {
    if (s == NULL || r == NULL || gen == 0) return 0;
    if (gen <= s->last_gen) return 0;           /* 重放/乱序旧帧，丢弃 */

    const size_t dl = r->def_len;
    if (dl == 0 || dl > PAR_DEF_EFF_MAX) { s->rollbacks++; return 0; }

    /* 候选 = 旧文本 + 新释义（追加），UTF-8 边界对齐。 */
    size_t base = s->text_len;
    if (base > PAR_SCREEN_TEXT_MAX) base = PAR_SCREEN_TEXT_MAX;
    const size_t room = PAR_SCREEN_TEXT_MAX - base;
    if (room == 0) { s->rollbacks++; return 0; }
    const size_t take = par_utf8_clamp(r->def, dl, room);
    if (take == 0) { s->rollbacks++; return 0; }

    memcpy(s->pending, s->text, base);
    memcpy(s->pending + base, r->def, take);
    const size_t total = par_utf8_clamp(s->pending, base + take,
                                        PAR_SCREEN_TEXT_MAX);
    if (total < base + take) { s->rollbacks++; return 0; } /* 重排失败→回滚 */
    s->pending[total] = '\0';

    /* 提交：候选成为当前文本（原子式一次拷贝，UI 此刻只读 text）。 */
    memcpy(s->text, s->pending, total + 1);
    s->text_len = total;
    s->last_gen = gen;
    s->reorders++;                              /* 追加重排成功→跳末页由 UI 做 */
    return 1;
}

size_t par_hexdump(const uint8_t* frame, size_t len, char* out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    if (out == NULL || cap == 0) return 0;
    size_t used = 0;
    for (size_t i = 0; i < len; ++i) {
        if ((i % 32u) == 0u && i != 0u) {
            if (used + 1 >= cap) return used;
            out[used++] = '\n';
        }
        if (used + 2 >= cap) return used;
        out[used++] = hex[frame[i] >> 4];
        out[used++] = hex[frame[i] & 0x0Fu];
    }
    if (used < cap) out[used] = '\0';
    return used;
}
