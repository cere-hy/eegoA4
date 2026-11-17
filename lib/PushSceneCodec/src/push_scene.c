/*
 * PushSceneCodec implementation — see include/push_scene.h for the whitepaper
 * §2.1 authoritative field layout. Pure C11, no ESP-IDF dependency: the very
 * same source is compiled into firmware and into the host assertion harness.
 */
#include "push_scene.h"

#include <string.h>

/* C11 compile-time layout pins (whitepaper §2.1: 1+2+2+2+234 == 241). */
typedef char psc_assert_frame[(PSC_FRAME_MAX == 1u + 2u + 2u + 2u + 234u) ? 1 : -1];
typedef char psc_assert_hdr[(PSC_HDR_LEN == 1u + 2u + 2u + 2u) ? 1 : -1];

/* UTF-8: a continuation byte has top bits 10xxxxxx. The cut point `hi` is a
   valid boundary only if s[hi] is NOT a continuation byte (or hi==lo). Walk
   back while the byte AT the cut point continues a multibyte sequence.
   Caller guarantees hi indexes a valid byte (s[hi] readable). */
static size_t utf8_boundary_back(const uint8_t* s, size_t lo, size_t hi) {
    while (hi > lo && (s[hi] & 0xC0u) == 0x80u) {
        --hi;
    }
    return hi;
}

/* Number of bytes occupied by the UTF-8 sequence starting at s[i], bounded. */
static size_t utf8_seq_len(const uint8_t* s, size_t i, size_t end) {
    uint8_t b = s[i];
    size_t want;
    if (b < 0x80u) want = 1;
    else if ((b & 0xE0u) == 0xC0u) want = 2;
    else if ((b & 0xF0u) == 0xE0u) want = 3;
    else if ((b & 0xF8u) == 0xF0u) want = 4;
    else return 0; /* invalid lead byte */
    if (i + want > end) return 0;
    return want;
}

size_t psc_word_clamp(const uint8_t* word, size_t len, uint8_t* out) {
    size_t n = len;
    if (n <= PSC_WORD_MAX) {
        /* No truncation: the whole word is valid, no boundary check needed. */
        if (out && n) memcpy(out, word, n);
        return n;
    }
    n = PSC_WORD_MAX;
    n = utf8_boundary_back(word, 0, n); /* never split a multibyte char; s[n] is readable */
    if (out && n) memcpy(out, word, n);
    return n;
}

int psc_plan(const uint8_t* ctx, size_t ctx_len,
             size_t word_len, psc_plan_t* out) {
    if (!ctx || !out) return 0;
    if (word_len == 0 || word_len > PSC_WORD_MAX) return 0; /* 空词拒 */
    if (ctx_len > PSC_CTX_MAX) return 0;

    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    uint8_t n = 0;

    while (pos < ctx_len && n < PSC_MAX_FRAMES) {
        size_t cap = (n == 0) ? (PSC_FRAME_MAX - (11u + word_len))
                              : (PSC_FRAME_MAX - PSC_HDR_LEN);
        size_t end = pos + cap;
        if (end >= ctx_len) {
            end = ctx_len; /* slice reaches input end: already a char boundary */
        } else {
            end = utf8_boundary_back(ctx, pos, end); /* mid-cut: snap to boundary */
        }
        /* If nothing fits (a single char wider than cap) -> reject. */
        if (end == pos) {
            size_t seq = utf8_seq_len(ctx, pos, ctx_len);
            if (seq == 0 || seq > cap) return 0;
            end = pos + seq;
        }
        out->off[n] = (uint16_t)pos;
        out->len[n] = (uint16_t)(end - pos);
        ++n;
        pos = end;
    }
    if (pos < ctx_len) return 0; /* more than 5 frames needed */
    /* Empty ctx still yields one (header + metadata, zero-length ctx) frame. */
    if (n == 0) n = 1;
    out->n = n;
    return (int)n;
}

int psc_frame_count(const uint8_t* ctx, size_t ctx_len, size_t word_len) {
    psc_plan_t plan;
    return psc_plan(ctx, ctx_len, word_len, &plan);
}

size_t psc_frame_encode(uint16_t seq,
                        const uint8_t* word,  size_t word_len,
                        uint16_t word_off, uint8_t flags,
                        const uint8_t* ctx,   size_t ctx_len,
                        psc_plan_t* plan,
                        uint8_t* out) {
    if (!out || !ctx || !word) return 0;
    if (word_len == 0 || word_len > PSC_WORD_MAX) return 0;

    psc_plan_t local;
    if (!plan) {
        if (psc_plan(ctx, ctx_len, word_len, &local) == 0) return 0;
        plan = &local;
    }
    if (seq >= plan->n) return 0;

    memset(out, 0, PSC_FRAME_MAX);
    out[0] = PSC_OPCODE;
    /* u16 LE fields */
    out[1] = (uint8_t)(seq & 0xFFu);
    out[2] = (uint8_t)(seq >> 8);
    out[3] = (uint8_t)(plan->n & 0xFFu);
    out[4] = (uint8_t)(plan->n >> 8);

    const uint8_t* slice = ctx + plan->off[seq];
    size_t slen = plan->len[seq];

    if (seq == 0) {
        /* first frame: header + metadata (word_len, word, word_off, flags) + ctx */
        out[7] = (uint8_t)word_len;
        memcpy(out + 8, word, word_len);
        size_t wo = 8u + word_len;
        out[wo]     = (uint8_t)(word_off & 0xFFu);
        out[wo + 1] = (uint8_t)(word_off >> 8);
        out[wo + 2] = flags;
        memcpy(out + (11u + word_len), slice, slen);
        /* payload = metadata(1+word_len+2+1) + ctx slice */
        uint16_t payload = (uint16_t)(1u + word_len + 2u + 1u + slen);
        out[5] = (uint8_t)(payload & 0xFFu);
        out[6] = (uint8_t)(payload >> 8);
    } else {
        /* subsequent frame: header + ctx only (no metadata) */
        memcpy(out + PSC_HDR_LEN, slice, slen);
        out[5] = (uint8_t)(slen & 0xFFu);
        out[6] = (uint8_t)(slen >> 8);
    }
    return PSC_FRAME_MAX;
}

int psc_frame_decode(const uint8_t* frame, size_t frame_len,
                     uint16_t* seq, uint16_t* total, uint16_t* payload,
                     uint8_t*  word, size_t* word_len,
                     uint16_t* word_off, uint8_t* flags,
                     const uint8_t** ctx, size_t* ctx_len) {
    if (!frame || frame_len < PSC_FRAME_MAX || frame[0] != PSC_OPCODE) return -1;
    uint16_t s = (uint16_t)(frame[1] | (frame[2] << 8));
    uint16_t t = (uint16_t)(frame[3] | (frame[4] << 8));
    uint16_t p = (uint16_t)(frame[5] | (frame[6] << 8));
    if (seq) *seq = s;
    if (total) *total = t;
    if (payload) *payload = p;
    if (t == 0 || t > PSC_MAX_FRAMES || s >= t) return -1;
    if (p > PSC_FRAME_MAX - PSC_HDR_LEN) return -1;

    if (s == 0) {
        size_t wl = frame[7];
        if (wl > PSC_WORD_MAX) return -1;
        size_t wo = 8u + wl;
        size_t clen = p - (1u + wl + 2u + 1u);
        if (word && word_len && wl) memcpy(word, frame + 8, wl);
        if (word_len) *word_len = wl;
        if (word_off) *word_off = (uint16_t)(frame[wo] | (frame[wo + 1] << 8));
        if (flags) *flags = frame[wo + 2];
        if (ctx) *ctx = frame + (11u + wl);
        if (ctx_len) *ctx_len = clen;
        return 1;
    }
    if (word_len) *word_len = 0;
    if (ctx) *ctx = frame + PSC_HDR_LEN;
    if (ctx_len) *ctx_len = p;
    return 0;
}
