/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 裁剪版 push_session 实现。时间由调用方注入 now_ms，副作用经动作位掩码申领；
 * 本文件不读时钟、不写文件、不碰射频。主路径转移边与权威件 §4 一致。
 */
#include "push_session.h"

#include <string.h>

void push_session_init(push_session_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->state = PUSH_STATE_IDLE;
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

/// BEGIN 会话级校验：token、type、total_len。返回 0 表示通过。
static int begin_check(const push_session_t *s, const push_begin_t *b) {
    if (b->token != s->token) return PUSH_ERR_BAD_FRAME;
    if (b->type > PUSH_TYPE_WORD) return PUSH_ERR_BAD_FRAME;
    // 文本推送必须有正文；裁剪版不处理 URL/单词卡无正文特例，WORD 也按普通文本，
    // 故 total_len 一律要求在 (0, PUSH_TOTAL_MAX] 内。
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
            s->state = PUSH_STATE_WINDOW;
            return 0;
        }
        if (evt == PUSH_EVT_CANCEL) return goto_idle(s);
        if (evt == PUSH_EVT_TICK && window_expired(s, now_ms)) return goto_idle(s);
        if (evt == PUSH_EVT_BEGIN && arg && arg->begin) {
            const int err = begin_check(s, arg->begin);
            if (err != PUSH_ERR_NONE) {
                s->last_err = err;
                return PUSH_ACT_ERR;
            }
            // 新会话：重置计数（裁剪版无 .part 续传，同名覆盖语义由 S3 落盘层负责）。
            s->begin = *arg->begin;
            s->received = 0;
            s->next_seq = 0;
            s->state = PUSH_STATE_RECEIVING;
            return PUSH_ACT_READY;
        }
        break;
    }

    case PUSH_STATE_RECEIVING:
        if (evt == PUSH_EVT_DISCONNECT) {
            s->state = PUSH_STATE_WINDOW;
            return 0;
        }
        if (evt == PUSH_EVT_CANCEL) {
            return goto_idle(s) | PUSH_ACT_DROP;
        }
        if (evt == PUSH_EVT_DATA && arg) {
            // seq 连续性：跳号即拒，本包不落盘，手机重发该序号。
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
            s->received += (uint32_t)arg->data_len;
            ++s->next_seq;
            return PUSH_ACT_APPEND | PUSH_ACT_PROGRESS;
        }
        if (evt == PUSH_EVT_END) {
            // 长度对账交给 VERIFY：不等就当校验失败（CRC 与长度同为收全判据）。
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
            // CRC 不符 或 长度不符 → 收全判据失败，拒收、丢弃、回 CONNECTED 可重推。
            if (!arg->verify_ok || s->received != s->begin.total_len) {
                s->last_err = PUSH_ERR_CHECKSUM;
                s->state = PUSH_STATE_CONNECTED;
                return PUSH_ACT_ERR | PUSH_ACT_DROP;
            }
            // 收全通过：提交 + 开文档。裁剪版无 cooldown，直接回 IDLE 收摊。
            s->state = PUSH_STATE_IDLE;
            return PUSH_ACT_OPEN_BOOK;
        }
        break;
    }

    return 0; // 非法转移：保持原状
}
