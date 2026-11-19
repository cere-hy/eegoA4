/*
 * SPDX-License-Identifier: Apache-2.0
 * B6.5 S3 — PushTextFilter 实现。六步固定顺序，逐行处理，维护 ``` 代码块状态。
 * 见 include/PushTextFilter.h 的硬约束说明。
 */
#include "PushTextFilter.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- dynamic byte buffer ---------------- */

typedef struct {
    char* d;
    size_t n;
    size_t cap;
} Buf;

static bool buf_reserve(Buf* b, size_t need) {
    if (b->cap >= need) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) return false;
        cap *= 2;
    }
    char* p = (char*)realloc(b->d, cap);
    if (!p) return false;
    b->d = p;
    b->cap = cap;
    return true;
}

static bool buf_putc(Buf* b, unsigned char c) {
    if (!buf_reserve(b, b->n + 1)) return false;
    b->d[b->n++] = (char)c;
    return true;
}

static bool buf_put(Buf* b, const char* s, size_t n) {
    if (n == 0) return true;
    if (!buf_reserve(b, b->n + n)) return false;
    memcpy(b->d + b->n, s, n);
    b->n += n;
    return true;
}

static bool buf_puts(Buf* b, const char* s) { return buf_put(b, s, strlen(s)); }

/* ---------------- utf8 helpers ---------------- */

static size_t utf8_seq_len(unsigned char b) {
    if (b < 0x80) return 1;
    if (b >= 0xC2 && b <= 0xDF) return 2;
    if (b >= 0xE0 && b <= 0xEF) return 3;
    if (b >= 0xF0 && b <= 0xF4) return 4;
    return 0;
}

static size_t zwsp_at(const char* s, size_t pos, size_t len) {
    if (pos + 3 <= len && (unsigned char)s[pos] == 0xE2 && (unsigned char)s[pos + 1] == 0x80 &&
        ((unsigned char)s[pos + 2] == 0x8B || (unsigned char)s[pos + 2] == 0x8C ||
         (unsigned char)s[pos + 2] == 0x8D))
        return 3;
    return 0;
}

static size_t bom_at(const char* s, size_t pos, size_t len) {
    if (pos + 3 <= len && (unsigned char)s[pos] == 0xEF && (unsigned char)s[pos + 1] == 0xBB &&
        (unsigned char)s[pos + 2] == 0xBF)
        return 3;
    return 0;
}

static size_t objrepl_at(const char* s, size_t pos, size_t len) {
    if (pos + 3 <= len && (unsigned char)s[pos] == 0xEF && (unsigned char)s[pos + 1] == 0xBF &&
        (unsigned char)s[pos + 2] == 0xBC)
        return 3;
    return 0;
}

static size_t ideosp_at(const char* s, size_t pos, size_t len) {
    if (pos + 3 <= len && (unsigned char)s[pos] == 0xE3 && (unsigned char)s[pos + 1] == 0x80 &&
        (unsigned char)s[pos + 2] == 0x80)
        return 3;
    return 0;
}

/* ---------------- step 1: control-char filter ---------------- */

static bool step1_controls(const char* in, size_t len, Buf* out, size_t* removed) {
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        size_t z = zwsp_at(in, i, len);
        if (z) {
            i += z;
            n += z;
            continue;
        }
        z = bom_at(in, i, len);
        if (z) {
            i += z;
            n += z;
            continue;
        }
        unsigned char c = (unsigned char)in[i];
        if ((c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) || c == 0x7F) {
            ++i;
            ++n;
            continue;
        }
        if (!buf_putc(out, c)) return false;
        ++i;
    }
    *removed = n;
    return true;
}

/* ---------------- step 2: UTF-8 tail repair ---------------- */

static size_t utf8_keep_len(const char* s, size_t len, size_t* dropped) {
    *dropped = 0;
    if (len == 0) return 0;
    for (size_t back = 1; back <= 3 && back <= len; ++back) {
        size_t start = len - back;
        unsigned char lead = (unsigned char)s[start];
        size_t need = utf8_seq_len(lead);
        if (need == 0) continue;
        if (need > back) { /* lead 在末尾但序列不完整 */
            *dropped = back;
            return start;
        }
        return len; /* 完整序列 */
    }
    return len;
}

/* ---------------- step 3: newline unify + blank-run compress ---------------- */

static bool step3_newlines(const char* in, size_t len, Buf* out, size_t* collapsed) {
    size_t run = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r') {
            if (i + 1 < len && (unsigned char)in[i + 1] == '\n') ++i;
            c = '\n';
        }
        if (c == '\n') {
            ++run;
            if (run <= 2) {
                if (!buf_putc(out, '\n')) return false;
            } else {
                ++(*collapsed);
            }
            ++i;
            continue;
        }
        run = 0;
        if (!buf_putc(out, c)) return false;
        ++i;
    }
    return true;
}

/* ---------------- step 4/5/6 helpers (single line) ---------------- */

static bool is_word(unsigned char c) { return isalnum(c) || c >= 0x80; }

static size_t leading_ws(const char* s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t') {
            ++i;
        } else {
            size_t sp = ideosp_at(s, i, len);
            if (sp)
                i += sp;
            else
                break;
        }
    }
    return i;
}

static size_t trailing_ws_trim(const char* s, size_t len) {
    while (len > 0) {
        size_t last = len - 1;
        unsigned char c = (unsigned char)s[last];
        if (c == ' ' || c == '\t') {
            --len;
        } else if (last >= 2 && ideosp_at(s, last - 2, len) == 3) {
            len -= 3;
        } else {
            break;
        }
    }
    return len;
}

static bool is_list_at(const char* s, size_t pos, size_t len) {
    if (pos >= len) return false;
    char c = s[pos];
    if (c == '-' || c == '*' || c == '+') return pos + 1 < len && s[pos + 1] == ' ';
    if (isdigit((unsigned char)c)) {
        size_t j = pos;
        while (j < len && isdigit((unsigned char)s[j])) ++j;
        if (j < len && (s[j] == '.' || s[j] == ')') && j + 1 < len && s[j + 1] == ' ') return true;
    }
    return false;
}

static bool step4_whitespace(const char* s, size_t len, Buf* out) {
    size_t lead = leading_ws(s, len);
    size_t start = lead;
    size_t end = trailing_ws_trim(s, len);
    if (end < start) end = start;

    bool keep_indent = (lead > 0) && is_list_at(s, lead, len);
    if (keep_indent) {
        size_t units = 0;
        for (size_t i = 0; i < lead;) {
            size_t sp = ideosp_at(s, i, lead);
            if (sp) {
                ++units;
                i += sp;
            } else {
                ++units;
                ++i;
            }
        }
        size_t level = units / 2;
        if (level == 0) level = 1;
        for (size_t k = 0; k < level; ++k) {
            if (!buf_put(out, "  ", 2)) return false;
        }
    }

    bool prev_space = false;
    for (size_t i = start; i < end; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ') {
            if (!prev_space && !buf_putc(out, ' ')) return false;
            prev_space = true;
        } else {
            if (!buf_putc(out, c)) return false;
            prev_space = false;
        }
    }
    return true;
}

static bool strip_heading(const char* s, size_t len, Buf* out) {
    size_t i = 0;
    while (i < len && s[i] == '#' && i < 6) ++i;
    if (i > 0 && i < len && (s[i] == ' ' || s[i] == '\t')) {
        while (i < len && (s[i] == ' ' || s[i] == '\t')) ++i;
        return buf_put(out, s + i, len - i);
    }
    return buf_put(out, s, len);
}

static bool strip_quote(const char* s, size_t len, Buf* out) {
    size_t i = 0;
    while (i < len && s[i] == '>') {
        ++i;
        if (i < len && s[i] == ' ') ++i;
    }
    return buf_put(out, s + i, len - i);
}

static bool strip_html(const char* s, size_t len, Buf* out) {
    for (size_t i = 0; i < len;) {
        if (s[i] == '<') {
            size_t j = i + 1;
            bool closing_or_decl = (j < len && (s[j] == '/' || s[j] == '!'));
            bool open_tag = (j < len && isalpha((unsigned char)s[j]));
            /* 闭合标签/声明无条件剥；开标签要求 '<' 左边界非词（避免误伤 a<b 比较） */
            if ((closing_or_decl || (open_tag && (i == 0 || !is_word((unsigned char)s[i - 1]))))) {
                while (j < len && s[j] != '>') ++j;
                if (j < len) {
                    i = j + 1;
                    continue;
                }
            }
        }
        if (!buf_putc(out, (unsigned char)s[i])) return false;
        ++i;
    }
    return true;
}

static bool strip_link(const char* s, size_t len, Buf* out) {
    for (size_t i = 0; i < len;) {
        if (s[i] == '[' && !(i > 0 && s[i - 1] == '!')) {
            size_t close = i + 1;
            while (close < len && s[close] != ']') ++close;
            if (close < len && close + 1 < len && s[close + 1] == '(') {
                size_t rp = close + 2;
                while (rp < len && s[rp] != ')') ++rp;
                if (rp < len) {
                    if (!buf_put(out, s + i + 1, close - (i + 1))) return false;
                    i = rp + 1;
                    continue;
                }
            }
        }
        if (!buf_putc(out, (unsigned char)s[i])) return false;
        ++i;
    }
    return true;
}

static bool strip_codespan(const char* s, size_t len, Buf* out) {
    for (size_t i = 0; i < len;) {
        if (s[i] == '`') {
            size_t j = i + 1;
            while (j < len && s[j] != '`') ++j;
            if (j < len) {
                if (!buf_put(out, s + i + 1, j - (i + 1))) return false;
                i = j + 1;
                continue;
            }
        }
        if (!buf_putc(out, (unsigned char)s[i])) return false;
        ++i;
    }
    return true;
}

static bool strip_emphasis(const char* s, size_t len, Buf* out) {
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '*' || c == '_') {
            size_t mlen = 1;
            if (i + 1 < len && (unsigned char)s[i + 1] == c) mlen = 2;
            bool left_ok = (i == 0) || !is_word((unsigned char)s[i - 1]);
            if (left_ok) {
                size_t j = i + mlen;
                bool found = false;
                while (j + mlen <= len) {
                    if ((unsigned char)s[j] == c) {
                        bool same = (mlen == 1) || (j + 1 < len && (unsigned char)s[j + 1] == c);
                        if (same && (j + mlen == len || !is_word((unsigned char)s[j + mlen]))) {
                            found = true;
                            break;
                        }
                    }
                    ++j;
                }
                if (found) {
                    size_t content_start = i + mlen;
                    if (!buf_put(out, s + content_start, j - content_start)) return false;
                    i = j + mlen;
                    continue;
                }
            }
        }
        if (!buf_putc(out, c)) return false;
        ++i;
    }
    return true;
}

static bool strip_list(const char* s, size_t len, Buf* out) {
    size_t i = leading_ws(s, len);
    if (i + 1 < len && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
        if (!buf_put(out, s, i)) return false;
        if (!buf_puts(out, "\xC2\xB7 ")) return false; /* · + 空格 */
        return buf_put(out, s + i + 2, len - (i + 2));
    }
    return buf_put(out, s, len);
}

static bool is_hr(const char* s, size_t len) {
    char mark = 0;
    size_t count = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == ' ' || c == '\t') continue;
        if (c == '-' || c == '*' || c == '_') {
            if (mark == 0) mark = c;
            if (c != mark) return false;
            ++count;
        } else {
            return false;
        }
    }
    return count >= 3;
}

static bool step6_images(const char* s, size_t len, Buf* out, size_t* images, bool* removed_any) {
    *removed_any = false;
    for (size_t i = 0; i < len;) {
        size_t o = objrepl_at(s, i, len);
        if (o) {
            i += o;
            ++(*images);
            *removed_any = true;
            continue;
        }
        if (s[i] == '!' && i + 1 < len && s[i + 1] == '[') {
            size_t close = i + 2;
            while (close < len && s[close] != ']') ++close;
            if (close < len && close + 1 < len && s[close + 1] == '(') {
                size_t rp = close + 2;
                while (rp < len && s[rp] != ')') ++rp;
                if (rp < len) {
                    i = rp + 1;
                    ++(*images);
                    *removed_any = true;
                    continue;
                }
            }
        }
        if (s[i] == '[' && !(i > 0 && s[i - 1] == '!')) {
            size_t close = i + 1;
            while (close < len && s[close] != ']') ++close;
            if (close < len) {
                size_t inner = close - (i + 1);
                const char* p = s + i + 1;
                bool pic = false;
                if (inner == 5 && memcmp(p, "Image", 5) == 0) pic = true;
                if (inner == 5 && memcmp(p, "image", 5) == 0) pic = true;
                if (inner == 6 && memcmp(p, "\xE5\x9B\xBE\xE7\x89\x87", 6) == 0) pic = true; /* 图片 */
                if (inner > 6 && memcmp(p, "\xE5\x9B\xBE\xE7\x89\x87", 6) == 0 && p[6] == ':') pic = true;
                if (pic) {
                    i = close + 1;
                    ++(*images);
                    *removed_any = true;
                    continue;
                }
            }
        }
        if (!buf_putc(out, (unsigned char)s[i])) return false;
        ++i;
    }
    return true;
}

/* ---------------- pipeline ---------------- */

static bool is_fence(const char* s, size_t len) {
    size_t i = leading_ws(s, len);
    return (i + 3 <= len && s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`');
}

/// 代码块外一行：④⑤⑥ → linebuf。*drop=true 表示整行删除（纯图片行）。
static bool process_outside_line(const char* s, size_t len, Buf* out, push_filter_stats_t* st, bool* drop) {
    *drop = false;
    Buf b4 = {0}, a = {0}, b = {0}, c = {0}, d = {0}, e = {0}, g = {0}, fin = {0};
    bool ok = false;
    do {
        if (!step4_whitespace(s, len, &b4)) break;
        if (is_hr(b4.d, b4.n)) { /* 分隔线 → 空行（fin 为空） */
            ok = true;
            break;
        }
        if (!strip_heading(b4.d, b4.n, &a)) break;
        if (!strip_quote(a.d, a.n, &b)) break;
        if (!strip_html(b.d, b.n, &c)) break;
        if (!strip_link(c.d, c.n, &d)) break;
        if (!strip_codespan(d.d, d.n, &e)) break;
        if (!strip_emphasis(e.d, e.n, &g)) break;
        if (!strip_list(g.d, g.n, &fin)) break;

        bool removed_any = false;
        Buf img = {0};
        if (!step6_images(fin.d, fin.n, &img, &st->images_removed, &removed_any)) {
            free(img.d);
            break;
        }
        if (img.n == 0 && removed_any) {
            *drop = true;
        } else if (!buf_put(out, img.d, img.n)) {
            free(img.d);
            break;
        }
        free(img.d);
        ok = true;
    } while (0);

    free(b4.d);
    free(a.d);
    free(b.d);
    free(c.d);
    free(d.d);
    free(e.d);
    free(g.d);
    free(fin.d);
    return ok;
}

size_t push_text_filter(const char* input, size_t len, char** out, push_filter_stats_t* stats) {
    if (out) *out = NULL;
    if (!input || !out) return (size_t)-1;

    push_filter_stats_t local = {0};
    push_filter_stats_t* st = stats ? stats : &local;
    memset(st, 0, sizeof(*st));

    Buf s1 = {0}, s2 = {0}, s3 = {0}, res = {0}, linebuf = {0};
    size_t rc = (size_t)-1;
    bool in_code = false;

    do {
        if (!step1_controls(input, len, &s1, &st->removed_controls)) break;
        size_t dropped = 0;
        size_t keep = utf8_keep_len(s1.d, s1.n, &dropped);
        st->dropped_utf8_tail = dropped;
        if (!buf_put(&s2, s1.d, keep)) break;
        if (!step3_newlines(s2.d, s2.n, &s3, &st->collapsed_runs)) break;

        size_t line_start = 0;
        for (size_t i = 0; i <= s3.n; ++i) {
            if (i < s3.n && s3.d[i] != '\n') continue;
            const char* line = s3.d + line_start;
            size_t llen = i - line_start;
            bool has_newline = (i < s3.n);
            bool drop_line = false;

            linebuf.n = 0;
            if (is_fence(line, llen)) {
                ++st->code_fences;
                in_code = !in_code;
                drop_line = true; /* 围栏行自身不输出 */
            } else if (in_code) {
                if (!buf_put(&linebuf, line, llen)) goto done; /* 代码块内原样 */
            } else {
                if (!process_outside_line(line, llen, &linebuf, st, &drop_line)) goto done;
            }

            if (!drop_line) {
                if (!buf_put(&res, linebuf.d, linebuf.n)) goto done;
                if (has_newline && !buf_putc(&res, '\n')) goto done;
            }
            line_start = i + 1;
        }
        if (in_code) st->unclosed_fence = true;

        if (!buf_putc(&res, '\0')) break;
        rc = res.n - 1;
        *out = res.d;
        res.d = NULL;
    } while (0);

done:
    free(s1.d);
    free(s2.d);
    free(s3.d);
    free(res.d);
    free(linebuf.d);
    return rc;
}

void push_text_filter_free(char* p) { free(p); }
