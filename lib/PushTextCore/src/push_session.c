/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * B6.5 S1 完整能力版 push_session。状态机结构照权威件（七态 + 断点续传 + cooldown），
 * 纯 C11：时间由调用方注入 now_ms，副作用经动作位掩码申领；不读时钟/文件/射频。
 *
 * 与本令的业务策略差异：
 *   - begin_check：五种 type 全解析；URL(3) 回 ERR(FETCH)；WORD(4) 按普通文本
 *     （要求有正文，不做「title 即单词」无正文特例），落库 type 由胶水带出。
 *   - VERIFY 成功统一 COMMIT|OPEN_BOOK，不产出 WORD_CARD 特判。
 */
#include "push_session.h"

#include <string.h>

void push_session_init(push_session_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->state = PUSH_STATE_IDLE;
}

static void adopt_locked(push_session_t *s, const push_begin_t *begin, uint32_t received, uint16_t next_seq) {
    s->begin = *begin;
    s->received = received;
    s->next_seq = next_seq;
    s->has_part = received > 0;
}

void push_session_resume(push_session_t *s, const push_begin_t *begin, uint32_t received, uint16_t next_seq) {
    if (!s || !begin) return;
    adopt_locked(s, begin, received, next_seq);
    s->state = PUSH_STATE_RECEIVING; // 恢复的半成品按续收中对待
}

void push_session_adopt(push_session_t *s, const push_begin_t *begin, uint32_t received, uint16_t next_seq) {
    if (!s || !begin || received == 0) return; // 没东西可装：IDLE 保持原样
    adopt_locked(s, begin, received, next_seq);
}

uint32_t push_session_window_left_ms(const push_session_t *s, uint32_t now_ms) {
    if (!s || s->window_ms == 0) return 0;
    const uint32_t elapsed = now_ms - s->armed_at_ms;
    return elapsed >= s->window_ms ? 0u : s->window_ms - elapsed;
}

static uint32_t goto_idle(push_session_t *s) {
    s->state = PUSH_STATE_IDLE;
    return PUSH_ACT_ADV_OFF;
}

static bool window_expired(const push_session_t *s, uint32_t now_ms) {
    return s->window_ms != 0 && (now_ms - s->armed_at_ms) >= s->window_ms;
}

/// BEGIN 会话级校验：token / type / total_len。
/// 六种 type 全解析（000-39b2b 追加 WALLPAPER=5）；URL(3) 明确回 FETCH（不支持，非未知帧丢弃）；
/// 未知 type(>5) 回 BAD_FRAME；WORD/WALLPAPER 都带正文。
static int begin_check(const push_session_t *s, const push_begin_t *b) {
    if (b->token != s->token) return PUSH_ERR_BAD_FRAME;
    if (b->type > PUSH_TYPE_WALLPAPER) return PUSH_ERR_BAD_FRAME;  // 未知 type(>5)：明确 ERR
    if (b->type == PUSH_TYPE_URL) return PUSH_ERR_FETCH;           // 认 type=3，业务不支持
    // 判据边界与 push_type_route() 一致（type=5 归壁纸、不进文本 sink，见 push_session.h）。
    // TEXT/MARKDOWN/HTML/WORD/WALLPAPER 都带正文：total_len ∈ (0, PUSH_TOTAL_MAX]
    if (b->total_len == 0) return PUSH_ERR_BAD_FRAME;
    if (b->total_len > PUSH_TOTAL_MAX) return PUSH_ERR_TOO_LARGE;
    return PUSH_ERR_NONE;
}

uint32_t push_session_event(push_session_t *s, push_evt_t evt, const push_event_t *arg) {
    if (!s) return 0;
    const uint32_t now_ms = arg ? arg->now_ms : 0;

    switch (s->state) {
    case PUSH_STATE_IDLE:
        if (evt == PUSH_EVT_ARM && arg) {
            s->token = arg->token;
            s->window_ms = arg->window_ms;
            s->armed_at_ms = now_ms;
            s->cooldown_ms = arg->cooldown_ms;
            s->cooldown_at_ms = 0;
            s->state = PUSH_STATE_ARMED;
            return PUSH_ACT_ADV_ON;
        }
        break;

    case PUSH_STATE_ARMED:
        if (evt == PUSH_EVT_ADV_UP) {
            s->state = PUSH_STATE_WINDOW;
            return 0;
        }
        if (evt == PUSH_EVT_CANCEL) return goto_idle(s);
        break;

    case PUSH_STATE_WINDOW:
        if (evt == PUSH_EVT_CONNECT) {
            s->state = PUSH_STATE_CONNECTED;
            return 0;
        }
        if (evt == PUSH_EVT_CANCEL) return goto_idle(s);
        if (evt == PUSH_EVT_TICK && window_expired(s, now_ms)) return goto_idle(s);
        break;

    case PUSH_STATE_CONNECTED: {
        if (evt == PUSH_EVT_DISCONNECT) {
            s->state = PUSH_STATE_WINDOW; // 有 .part 时簿记留在，供续传
            return 0;
        }
        if (evt == PUSH_EVT_CANCEL) {
            return goto_idle(s) | (s->has_part ? PUSH_ACT_DROP : 0u);
        }
        if (evt == PUSH_EVT_TICK && window_expired(s, now_ms)) return goto_idle(s);
        if (evt == PUSH_EVT_RESUME_QUERY) {
            // 无论有没有 .part 都回 RESUME：offset 0 即让手机重头发。
            // 有半成品必须同时进 RECEIVING，否则后续 DATA 被当非法转移吞掉。
            if (s->has_part) s->state = PUSH_STATE_RECEIVING;
            return PUSH_ACT_RESUME;
        }
        if (evt == PUSH_EVT_END && s->has_part) {
            // 坏 CRC 后手机补包再重发 END：放行，再走一次对账。
            s->state = PUSH_STATE_PROCESSING;
            return PUSH_ACT_VERIFY;
        }
        if (evt == PUSH_EVT_BEGIN && arg && arg->begin) {
            const push_begin_t *b = arg->begin;
            const int err = begin_check(s, b);
            if (err != PUSH_ERR_NONE) {
                s->last_err = err;
                return PUSH_ACT_ERR;
            }
            if (s->has_part && b->session_id == s->begin.session_id) {
                // 同会话重发 BEGIN：不重置计数，直接续传。
                s->state = PUSH_STATE_RECEIVING;
                return PUSH_ACT_RESUME;
            }
            // 新会话：旧 .part 作废。
            const uint32_t drop = s->has_part ? PUSH_ACT_DROP : 0u;
            s->begin = *b;
            s->received = 0;
            s->next_seq = 0;
            s->has_part = true;
            s->state = PUSH_STATE_RECEIVING;
            return drop | PUSH_ACT_READY;
        }
        break;
    }

    case PUSH_STATE_RECEIVING:
        if (evt == PUSH_EVT_DISCONNECT) {
            s->state = PUSH_STATE_WINDOW; // 断连回窗口，.part 留着等续传
            return 0;
        }
        if (evt == PUSH_EVT_CANCEL) {
            return goto_idle(s) | PUSH_ACT_DROP;
        }
        if (evt == PUSH_EVT_DATA && arg) {
            // seq 连续性：跳号即拒，本包不落盘，手机重发。
            if (arg->seq != s->next_seq) {
                s->last_err = PUSH_ERR_BAD_FRAME;
                return PUSH_ACT_ERR;
            }
            if ((uint64_t)s->received + arg->data_len > s->begin.total_len) {
                s->last_err = PUSH_ERR_BAD_FRAME;
                return PUSH_ACT_ERR;
            }
            if ((uint64_t)s->received + arg->data_len > PUSH_TOTAL_MAX) {
                s->last_err = PUSH_ERR_TOO_LARGE;
                return PUSH_ACT_ERR;
            }
            // 记账先于落盘：胶水按 ACT_APPEND 写 .part，计数在此推进，
            // 续传 offset/next_seq 才与 .part 长度一致。
            s->received += (uint32_t)arg->data_len;
            ++s->next_seq;
            return PUSH_ACT_APPEND | PUSH_ACT_PROGRESS;
        }
        if (evt == PUSH_EVT_END) {
            // 长度对账交给 VERIFY：不等当校验失败，保留 .part 续传。
            s->state = PUSH_STATE_PROCESSING;
            return PUSH_ACT_VERIFY;
        }
        break;

    case PUSH_STATE_PROCESSING:
        if (evt == PUSH_EVT_DISCONNECT) {
            s->state = PUSH_STATE_WINDOW;
            return 0;
        }
        if (evt == PUSH_EVT_VERIFY && arg) {
            if (!arg->verify_ok || s->received != s->begin.total_len) {
                // 收全判据失败：保留 .part（不 DROP）供续传，回 CONNECTED 可补包重发 END。
                s->last_err = PUSH_ERR_CHECKSUM;
                s->state = PUSH_STATE_CONNECTED;
                return PUSH_ACT_ERR;
            }
            // 成功：进宽限；事务四拍 commit 与开书由胶水/UI 执行。
            s->cooldown_at_ms = now_ms;
            s->state = PUSH_STATE_COOLDOWN;
            return PUSH_ACT_COMMIT | PUSH_ACT_OPEN_BOOK;
        }
        break;

    case PUSH_STATE_COOLDOWN:
        if (evt == PUSH_EVT_DISCONNECT) {
            s->state = PUSH_STATE_WINDOW; // 推完手机就走，回窗口继续广播
            return 0;
        }
        if (evt == PUSH_EVT_TICK) {
            if (s->cooldown_ms != 0 && (now_ms - s->cooldown_at_ms) >= s->cooldown_ms) {
                if (window_expired(s, now_ms)) return goto_idle(s);
                if (s->window_ms == 0) return goto_idle(s); // 常开会话：宽限到就收摊
                s->state = PUSH_STATE_WINDOW;
                return 0;
            }
            if (window_expired(s, now_ms)) return goto_idle(s);
            return 0;
        }
        if (evt == PUSH_EVT_CONNECT) {
            s->state = PUSH_STATE_CONNECTED;
            return 0;
        }
        if (evt == PUSH_EVT_BEGIN && arg && arg->begin) {
            const int err = begin_check(s, arg->begin);
            if (err != PUSH_ERR_NONE) {
                s->last_err = err;
                return PUSH_ACT_ERR;
            }
            s->begin = *arg->begin;
            s->received = 0;
            s->next_seq = 0;
            s->has_part = true;
            s->state = PUSH_STATE_RECEIVING;
            return PUSH_ACT_READY;
        }
        if (evt == PUSH_EVT_CANCEL) return goto_idle(s);
        break;
    }

    return 0; // 非法转移：保持原状
}
