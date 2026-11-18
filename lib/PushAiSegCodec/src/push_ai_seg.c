/* PushAiSegCodec — 0x08 AI 释义分帧编解码实现（方案乙）。纯 C11。 */
#include "push_ai_seg.h"

#include <string.h>

_Static_assert(PAS_FRAME_LEN == 241u, "0x08 frame must be 241 bytes");
_Static_assert(PAS_WORD_SLOT == 64u, "word slot");
_Static_assert(PAS_SEG_SLOT == 166u, "seg slot");

static uint16_t rd_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void wr_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

bool pas_seg_decode(const uint8_t* frame, size_t len, pas_seg_t* out) {
    if (frame == NULL || out == NULL || len != PAS_FRAME_LEN) return false;
    if (frame[0] != PAS_MAGIC0 || frame[1] != PAS_MAGIC1 ||
        frame[2] != PAS_OPCODE || frame[3] != PAS_VERSION) return false;

    const size_t wn = frame[4];
    if (wn >= PAS_WORD_SLOT) return false;

    const size_t off = 4u + 1u + PAS_WORD_SLOT;  /* 69 */
    const uint16_t idx = rd_u16(frame + off);
    const uint16_t total = rd_u16(frame + off + 2u);
    const uint16_t dn = rd_u16(frame + off + 4u);
    if (total == 0u || idx >= total) return false;
    if (dn >= PAS_SEG_SLOT) return false;         /* 槽保 NUL，与参考固件同构 */

    memset(out, 0, sizeof(*out));
    memcpy(out->word, frame + 5u, wn);
    out->word[wn] = 0;
    out->seg_idx = idx;
    out->seg_total = total;
    out->dlen = dn;
    memcpy(out->seg, frame + off + 6u, dn);
    out->seg[dn] = 0;
    return true;
}

size_t pas_seg_encode(const pas_seg_t* s, uint8_t* out, size_t cap) {
    if (s == NULL || out == NULL || cap < PAS_FRAME_LEN) return 0;
    if (s->seg_total == 0u || s->seg_idx >= s->seg_total) return 0;

    memset(out, 0, PAS_FRAME_LEN);
    out[0] = PAS_MAGIC0;
    out[1] = PAS_MAGIC1;
    out[2] = PAS_OPCODE;
    out[3] = PAS_VERSION;

    size_t wn = strnlen(s->word, PAS_WORD_SLOT);
    if (wn >= PAS_WORD_SLOT) wn = PAS_WORD_SLOT - 1u;
    out[4] = (uint8_t)wn;
    memcpy(out + 5u, s->word, wn);

    const size_t off = 4u + 1u + PAS_WORD_SLOT;
    wr_u16(out + off, s->seg_idx);
    wr_u16(out + off + 2u, s->seg_total);
    size_t dn = strnlen(s->seg, PAS_SEG_SLOT);
    if (dn >= PAS_SEG_SLOT) dn = PAS_SEG_SLOT - 1u;
    wr_u16(out + off + 4u, (uint16_t)dn);
    memcpy(out + off + 6u, s->seg, dn);
    return PAS_FRAME_LEN;
}

size_t pas_caps_encode(uint32_t bits, uint8_t* out, size_t cap) {
    if (out == NULL || cap < 5u) return 0;
    out[0] = PAS_ST_CAPS;
    wr_u32(out + 1u, bits);
    return 5u;
}

size_t pas_err_encode(uint8_t code, uint8_t* out, size_t cap) {
    if (out == NULL || cap < 2u) return 0;
    out[0] = PAS_ST_ERR;
    out[1] = code;
    return 2u;
}
