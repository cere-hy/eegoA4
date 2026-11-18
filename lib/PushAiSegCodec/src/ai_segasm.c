/* ai_segasm — 0x08 释义分帧重组器实现（方案乙 S2）。纯 C11。 */
#include "ai_segasm.h"

#include <stdio.h>
#include <string.h>

void ai_segasm_reset(ai_segasm_t* a) {
    if (!a) return;
    memset(a, 0, sizeof(*a));
}

ai_segasm_status_t ai_segasm_feed(ai_segasm_t* a, const pas_seg_t* s,
                                  const char** out_text, size_t* out_len) {
    if (!a || !s) return AI_SEGASM_REJECT;

    /* ① seg_idx==0 永远开新条：重发不与上一版拼接。 */
    if (s->seg_idx == 0) {
        ai_segasm_reset(a);
        size_t n = strnlen(s->seg, PAS_SEG_SLOT);
        if (n >= AI_SEGASM_MAX) {
            ai_segasm_reset(a);
            return AI_SEGASM_OVERFLOW;
        }
        memcpy(a->buf, s->seg, n);
        a->buf[n] = 0;
        a->len = n;
        snprintf(a->word, sizeof(a->word), "%s", s->word);
        a->total = s->seg_total;
        a->next = 1;
        a->active = true;
    } else {
        /* ② 续帧必须连得上、同一个词、total 一致，否则整条作废。 */
        if (!a->active || s->seg_idx != a->next || s->seg_total != a->total ||
            strncmp(a->word, s->word, sizeof(a->word)) != 0) {
            ai_segasm_reset(a);
            return AI_SEGASM_REJECT;
        }
        size_t n = strnlen(s->seg, PAS_SEG_SLOT);
        if (a->len + n + 1u > AI_SEGASM_MAX) {
            ai_segasm_reset(a);
            return AI_SEGASM_OVERFLOW;  /* 不截断 */
        }
        memcpy(a->buf + a->len, s->seg, n);
        a->len += n;
        a->buf[a->len] = 0;
        a->next = (uint16_t)(s->seg_idx + 1u);
    }

    /* ③ 收齐即完成（total==1 立即完成，语义等价 0x07）。 */
    if (a->next >= a->total) {
        if (out_text) *out_text = a->buf;
        if (out_len) *out_len = a->len;
        return AI_SEGASM_DONE;
    }
    return AI_SEGASM_PENDING;
}
