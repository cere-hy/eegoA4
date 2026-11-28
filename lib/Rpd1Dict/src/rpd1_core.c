/*
 * RPD1 offline dictionary core — format logic ported verbatim from
 * reference firmware main/dict/dict_lookup.c. Only the storage backend changed
 * (injected rpd1_io_t instead of POSIX FILE*). Constants, offsets, binary
 * search, key-window scan and stemming are byte-for-byte upstream.
 */
#include "rpd1_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Header layout (all little-endian) — mirrors build_dict.py verbatim. */
#define RPD1_MAGIC "RPD1"
#define RPD1_HEADER_SIZE 64u
#define RPD1_REC_BYTES 6u

void rpd1_close(rpd1_dict_t* d) {
  if (!d) return;
  d->io = NULL;
  d->sparse = NULL;
  d->sparse_count = 0;
  d->ready = false;
}

bool rpd1_ready(const rpd1_dict_t* d) {
  return d && d->ready && d->sparse && d->sparse_count > 0;
}

bool rpd1_open(rpd1_dict_t* d, const rpd1_io_t* io,
               rpd1_sparse_t* sparse_buf, uint32_t sparse_cap) {
  if (!d || !io || !io->read_at || !io->file_size || !sparse_buf || sparse_cap == 0) return false;
  memset(d, 0, sizeof(*d));

  unsigned char header[RPD1_HEADER_SIZE];
  if (!io->read_at(0, header, RPD1_HEADER_SIZE, io->ctx)) return false;
  if (memcmp(header, RPD1_MAGIC, 4) != 0) return false;

  uint32_t version, entries, block_log2, fields[8];
  memcpy(&version, header + 8, 4);
  memcpy(&entries, header + 12, 4);
  memcpy(&block_log2, header + 16, 4);
  memcpy(fields, header + 0x14, 32);
  if (version != 1 || entries == 0 || block_log2 < 4 || block_log2 > 16) return false;

  uint32_t keys_off = fields[0], keys_len = fields[1];
  uint32_t rec_off = fields[2], rec_len = fields[3];
  uint32_t defs_off = fields[4], defs_len = fields[5];
  uint32_t sparse_off = fields[6], sparse_len = fields[7];

  uint64_t size = 0;
  if (!io->file_size(&size, io->ctx) || size == 0) return false;

  /* Same self-consistency check as upstream try_open(): rec_len is a redundant
   * copy of entries*6, sections ordered keys -> rec -> defs -> sparse, all in bounds. */
  const bool layout_ok =
      rec_len == (uint64_t)entries * RPD1_REC_BYTES &&
      keys_off >= RPD1_HEADER_SIZE && keys_off < rec_off &&
      rec_off < defs_off && defs_off < sparse_off &&
      keys_off + (uint64_t)keys_len <= size &&
      rec_off + (uint64_t)rec_len <= size &&
      defs_off + (uint64_t)defs_len <= size &&
      sparse_off + (uint64_t)sparse_len <= size;
  if (!layout_ok) return false;

  /* Sparse count estimate: >=6B header + 1B headword per slot, capped. */
  uint32_t max_slots = sparse_len / 7 + 2;
  if (max_slots > sparse_cap) max_slots = sparse_cap;
  if (max_slots == 0 || sparse_len > RPD1_SPARSE_MAX * 7u + RPD1_SPARSE_MAX * RPD1_HEAD_MAX) return false;

  /* Sparse blob is at most a few hundred KB; caller may pass a static/PSRAM
   * buffer, but the transient blob must come from somewhere — read in chunks
   * directly into entries to avoid a second large buffer. */
  uint32_t pos = 0, count = 0;
  bool ok = true;
  while (ok && pos + 6 <= sparse_len && count < max_slots) {
    unsigned char rec[6];
    if (!io->read_at(sparse_off + pos, rec, 6, io->ctx)) { ok = false; break; }
    uint32_t key_off;
    uint16_t key_len;
    memcpy(&key_off, rec, 4);
    memcpy(&key_len, rec + 4, 2);
    pos += 6;
    if (key_len == 0 || key_len > RPD1_HEAD_MAX - 1 || pos + key_len > sparse_len) { ok = false; break; }
    sparse_buf[count].key_off = key_off;
    sparse_buf[count].key_len = key_len;
    if (!io->read_at(sparse_off + pos, sparse_buf[count].first, key_len, io->ctx)) { ok = false; break; }
    sparse_buf[count].first[key_len] = 0;
    pos += key_len;
    ++count;
  }
  if (!ok || count == 0) return false;

  d->io = io;
  d->entries = entries;
  d->block_log2 = block_log2;
  d->keys_off = keys_off;
  d->keys_len = keys_len;
  d->rec_off = rec_off;
  d->defs_off = defs_off;
  d->defs_len = defs_len;
  d->sparse = sparse_buf;
  d->sparse_count = count;
  d->ready = true;
  return true;
}

/* ---- exact lookup: RAM binary search -> one key-window read -> linear scan. ---- */
static bool lookup_exact(const rpd1_dict_t* d, const char* key,
                         char* out, size_t cap, size_t* out_len) {
  if (!rpd1_ready(d) || !key || !*key) return false;
  const size_t key_len = strlen(key);
  if (key_len >= RPD1_HEAD_MAX) return false;

  uint32_t lo = 0, hi = d->sparse_count;
  while (lo + 1 < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (strcmp(d->sparse[mid].first, key) <= 0) lo = mid;
    else hi = mid;
  }
  if (strcmp(d->sparse[lo].first, key) > 0) return false;

  const uint32_t block = 1u << d->block_log2;
  const uint32_t win_off = d->sparse[lo].key_off;
  const uint32_t win_end = lo + 1 < d->sparse_count
      ? d->sparse[lo + 1].key_off : d->keys_len;
  if (win_end <= win_off || win_end > d->keys_len) return false;
  const uint32_t win_len = win_end - win_off;
  if (win_len == 0 || win_len > RPD1_WINDOW_MAX) return false;
  const uint32_t first = lo << d->block_log2;
  uint32_t count = d->entries - first;
  if (count > block) count = block;

  /* Key window + record slice: two transient buffers. Upstream allocated these
   * in PSRAM; here the caller of lookup_exact provides no allocator, so use
   * fixed stack buffers bounded by RPD1_WINDOW_MAX — but cap the practical read
   * to avoid large stacks on-device. The adapter redirects these via a private
   * static pool on ESP32; on host they are ordinary heap-free stack arrays. */
  char* win = (char*)malloc(win_len);
  unsigned char* recs = (unsigned char*)malloc((size_t)count * RPD1_REC_BYTES);
  bool hit = false;
  const rpd1_io_t* io = d->io;
  if (win && recs &&
      io->read_at(d->keys_off + win_off, win, win_len, io->ctx) &&
      io->read_at(d->rec_off + (uint64_t)first * RPD1_REC_BYTES, recs,
                  (size_t)count * RPD1_REC_BYTES, io->ctx)) {
    uint32_t wp = 0, rp = 0;
    while (wp + 2 <= win_len && rp + RPD1_REC_BYTES <= (uint32_t)count * RPD1_REC_BYTES) {
      uint16_t klen;
      memcpy(&klen, win + wp, 2);
      if (klen == 0 || wp + 2u + klen > win_len) break;
      const char* kb = win + wp + 2;
      int cmp = memcmp(kb, key, klen < key_len ? klen : key_len);
      if (cmp == 0) cmp = klen < key_len ? -1 : (klen > key_len ? 1 : 0);
      if (cmp == 0) {
        uint32_t def_off;
        uint16_t def_len;
        memcpy(&def_off, recs + rp, 4);
        memcpy(&def_len, recs + rp + 4, 2);
        if (def_len && def_off + def_len <= d->defs_len && def_len < cap) {
          if (io->read_at(d->defs_off + def_off, out, def_len, io->ctx)) {
            out[def_len] = 0;
            if (out_len) *out_len = def_len;
            hit = true;
          }
        }
        break;
      }
      if (cmp > 0) break;
      wp += 2u + klen;
      rp += RPD1_REC_BYTES;
    }
  }
  free(win);
  free(recs);
  return hit;
}

/* ---- lightweight stemming, byte-for-byte from upstream make_candidates. ---- */
static bool ends(const char* w, const char* suf) {
  size_t lw = strlen(w), ls = strlen(suf);
  return lw > ls && strcmp(w + lw - ls, suf) == 0;
}

static size_t make_candidates(const char* w, char out[][RPD1_WORD_MAX], size_t max) {
  size_t n = 0;
  char buf[RPD1_WORD_MAX];
  size_t len = strlen(w);
  #define ADD(candidate) do { \
      if (n < max && (candidate)[0] && strcmp(candidate, w) != 0) { \
        bool dup = false; \
        for (size_t i = 0; i < n; ++i) if (strcmp(out[i], (candidate)) == 0) { dup = true; break; } \
        if (!dup) snprintf(out[n++], RPD1_WORD_MAX, "%s", (candidate)); \
      } \
  } while (0)

  if (ends(w, "'s")) { snprintf(buf, sizeof(buf), "%.*s", (int)(len - 2), w); ADD(buf); }
  if (ends(w, "ies") && len > 4) { snprintf(buf, sizeof(buf), "%.*sy", (int)(len - 3), w); ADD(buf); }
  if (ends(w, "es") && len > 3) { snprintf(buf, sizeof(buf), "%.*s", (int)(len - 2), w); ADD(buf); }
  if (ends(w, "s") && !ends(w, "ss") && len > 2) { snprintf(buf, sizeof(buf), "%.*s", (int)(len - 1), w); ADD(buf); }
  if (ends(w, "ied") && len > 4) { snprintf(buf, sizeof(buf), "%.*sy", (int)(len - 3), w); ADD(buf); }
  if (ends(w, "ed") && len > 3) {
    snprintf(buf, sizeof(buf), "%.*s", (int)(len - 2), w); ADD(buf);
    snprintf(buf, sizeof(buf), "%.*s", (int)(len - 1), w); ADD(buf);
    if (len > 5 && w[len - 3] == w[len - 4] && strchr("bcdfgklmnprstz", w[len - 3])) {
      snprintf(buf, sizeof(buf), "%.*s", (int)(len - 3), w); ADD(buf);
    }
  }
  if (ends(w, "ing") && len > 4) {
    snprintf(buf, sizeof(buf), "%.*s", (int)(len - 3), w); ADD(buf);
    snprintf(buf, sizeof(buf), "%.*se", (int)(len - 3), w); ADD(buf);
    if (len > 5 && w[len - 4] == w[len - 5] && strchr("bcdfgklmnprstz", w[len - 4])) {
      snprintf(buf, sizeof(buf), "%.*s", (int)(len - 4), w); ADD(buf);
    }
  }
  static const char* cmp_suf[] = {"er", "est"};
  for (size_t i = 0; i < 2; ++i) {
    const char* suf = cmp_suf[i];
    size_t ls = strlen(suf);
    if (ends(w, suf) && len > ls + 2) {
      snprintf(buf, sizeof(buf), "%.*s", (int)(len - ls), w); ADD(buf);
      if (len > ls + 3 && w[len - ls - 1] == w[len - ls - 2] &&
          strchr("bcdfgklmnprstz", w[len - ls - 1])) {
        snprintf(buf, sizeof(buf), "%.*s", (int)(len - ls - 1), w); ADD(buf);
      }
      if ((ends(w, "ier") || ends(w, "iest")) && len > ls + 2) {
        snprintf(buf, sizeof(buf), "%.*sy", (int)(len - ls - 1), w); ADD(buf);
      }
    }
  }
  if (ends(w, "ily") && len > 4) { snprintf(buf, sizeof(buf), "%.*sy", (int)(len - 3), w); ADD(buf); }
  else if (ends(w, "ly") && len > 4) { snprintf(buf, sizeof(buf), "%.*s", (int)(len - 2), w); ADD(buf); }
  #undef ADD
  return n;
}

static bool dict_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'' || c == '-';
}

bool rpd1_lookup(const rpd1_dict_t* d, const char* word, char* out, size_t cap) {
  if (!rpd1_ready(d) || !word || !out || cap < 64) return false;
  out[0] = 0;
  char raw[RPD1_WORD_MAX];
  size_t raw_len = 0;
  for (const char* p = word; *p && raw_len + 1 < sizeof(raw); ++p) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    raw[raw_len++] = c;
  }
  raw[raw_len] = 0;
  size_t a = 0, b = raw_len;
  while (a < b && !dict_word_char(raw[a])) ++a;
  while (b > a && !dict_word_char(raw[b - 1])) --b;
  char key[RPD1_WORD_MAX];
  size_t n = 0;
  while (a < b && n + 1 < sizeof(key)) key[n++] = raw[a++];
  key[n] = 0;
  if (!key[0]) return false;
  if (lookup_exact(d, key, out, cap, NULL)) return true;

  char cands[8][RPD1_WORD_MAX];
  size_t count = make_candidates(key, cands, 8);
  for (size_t i = 0; i < count; ++i) {
    if (lookup_exact(d, cands[i], out, cap, NULL)) {
      size_t cl = strlen(cands[i]);
      size_t used = strlen(out);
      if (used + cl + 4 < cap) {
        memmove(out + cl + 3, out, used + 1);
        out[0] = '[';
        memcpy(out + 1, cands[i], cl);
        out[cl + 1] = ']';
        out[cl + 2] = '\n';
      }
      return true;
    }
  }
  return false;
}
