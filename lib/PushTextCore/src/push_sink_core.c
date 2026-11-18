/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * B6.5 S1 完整能力版 push_sink_core。副作用出口有三：动作位掩码、status 帧、
 * open/commit_ready 事件。上层（S2 GATT / S3 落盘+事务四拍 / S4 UI）消费。
 */
#include "push_sink_core.h"

#include <string.h>

void push_sink_core_init(push_sink_core_t *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    push_session_init(&c->sess);
}

/// 把动作位翻译成 STATUS 帧写进 c->status（覆盖式；上层及时取走）。
static void emit_status(push_sink_core_t *c, uint8_t opcode, const void *body, size_t body_len) {
    c->status_len = push_status_encode(opcode, (const uint8_t *)body, body_len,
                                       c->status, sizeof(c->status));
}

/// 若动作含 ERR，用 last_err 填 ST_ERR body。
static void maybe_emit_err(push_sink_core_t *c, uint32_t acts) {
    if (acts & PUSH_ACT_ERR) {
        uint8_t code = (uint8_t)c->sess.last_err;
        emit_status(c, PUSH_ST_ERR, &code, 1);
    }
}

uint32_t push_sink_core_arm(push_sink_core_t *c, uint16_t token, uint32_t window_ms,
                            uint32_t cooldown_ms, uint32_t now_ms) {
    if (!c) return 0;
    c->status_len = 0;
    push_event_t e;
    memset(&e, 0, sizeof(e));
    e.token = token;
    e.window_ms = window_ms;
    e.cooldown_ms = cooldown_ms;
    e.now_ms = now_ms;
    return push_session_event(&c->sess, PUSH_EVT_ARM, &e);
}

uint32_t push_sink_core_adv_up(push_sink_core_t *c) {
    return c ? push_session_event(&c->sess, PUSH_EVT_ADV_UP, NULL) : 0;
}
uint32_t push_sink_core_connect(push_sink_core_t *c) {
    return c ? push_session_event(&c->sess, PUSH_EVT_CONNECT, NULL) : 0;
}
uint32_t push_sink_core_disconnect(push_sink_core_t *c) {
    return c ? push_session_event(&c->sess, PUSH_EVT_DISCONNECT, NULL) : 0;
}
uint32_t push_sink_core_cancel(push_sink_core_t *c) {
    if (!c) return 0;
    uint32_t acts = push_session_event(&c->sess, PUSH_EVT_CANCEL, NULL);
    if (acts & PUSH_ACT_DROP) {
        c->open_ready = false;
        c->commit_ready = false;
    }
    return acts;
}
uint32_t push_sink_core_tick(push_sink_core_t *c, uint32_t now_ms) {
    if (!c) return 0;
    push_event_t e;
    memset(&e, 0, sizeof(e));
    e.now_ms = now_ms;
    return push_session_event(&c->sess, PUSH_EVT_TICK, &e);
}

void push_sink_core_adopt(push_sink_core_t *c, const push_begin_t *begin,
                          uint32_t received, uint16_t next_seq) {
    if (!c) return;
    push_session_adopt(&c->sess, begin, received, next_seq);
    c->crc_acc = 0u;           // 续收段的 CRC 由上层回读 .part 后经 set_verified_crc 覆盖
    c->crc_external_set = false;
}

void push_sink_core_resume(push_sink_core_t *c, const push_begin_t *begin,
                           uint32_t received, uint16_t next_seq) {
    if (!c) return;
    push_session_resume(&c->sess, begin, received, next_seq);
    c->crc_acc = 0u;
    c->crc_external_set = false;
}

void push_sink_core_set_verified_crc(push_sink_core_t *c, bool crc_match) {
    if (!c) return;
    c->crc_external_set = true;
    c->crc_external_ok = crc_match;
}

/// 产出 ST_RESUME：body = received(u32 LE) + next_seq(u16 LE)。
static void emit_resume(push_sink_core_t *c) {
    c->resume_offset = c->sess.received;
    c->resume_next_seq = c->sess.next_seq;
    uint8_t body[PUSH_ST_RESUME_BODY];
    const uint32_t rx = c->sess.received;
    body[0] = (uint8_t)(rx & 0xFFu);
    body[1] = (uint8_t)((rx >> 8) & 0xFFu);
    body[2] = (uint8_t)((rx >> 16) & 0xFFu);
    body[3] = (uint8_t)((rx >> 24) & 0xFFu);
    body[4] = (uint8_t)(c->sess.next_seq & 0xFFu);
    body[5] = (uint8_t)((c->sess.next_seq >> 8) & 0xFFu);
    emit_status(c, PUSH_ST_RESUME, body, sizeof(body));
}

uint32_t push_sink_core_on_ctrl(push_sink_core_t *c, const uint8_t *frame, size_t len) {
    if (!c || !frame) return 0;
    uint8_t op = 0;
    if (!push_ctrl_header_decode(frame, len, &op)) {
        c->sess.last_err = PUSH_ERR_BAD_FRAME;
        emit_status(c, PUSH_ST_ERR, &c->sess.last_err, 1);
        return PUSH_ACT_ERR;
    }

    push_event_t e;
    memset(&e, 0, sizeof(e));

    if (op == PUSH_OP_BEGIN) {
        push_begin_t b;
        if (!push_begin_decode(frame, len, &b)) {
            c->sess.last_err = PUSH_ERR_BAD_FRAME;
            emit_status(c, PUSH_ST_ERR, &c->sess.last_err, 1);
            return PUSH_ACT_ERR;
        }
        e.begin = &b;
        uint32_t acts = push_session_event(&c->sess, PUSH_EVT_BEGIN, &e);
        if (acts & PUSH_ACT_READY) {
            c->crc_acc = 0u;                     // 新会话：CRC 从头算
            c->crc_external_set = false;
            c->open_ready = false;
            c->commit_ready = false;
            emit_status(c, PUSH_ST_READY, NULL, 0);
        } else if (acts & PUSH_ACT_RESUME) {
            // 同会话重发 BEGIN → 续传：产出 RESUME 回执带出 offset。
            c->crc_external_set = false;
            emit_resume(c);
        } else {
            maybe_emit_err(c, acts); // 含 URL(3)→ERR(FETCH) / 未知 type→ERR(BAD_FRAME)
        }
        return acts;
    }

    if (op == PUSH_OP_RESUME_QUERY) {
        uint32_t acts = push_session_event(&c->sess, PUSH_EVT_RESUME_QUERY, NULL);
        if (acts & PUSH_ACT_RESUME) {
            emit_resume(c);
        } else {
            maybe_emit_err(c, acts);
        }
        return acts;
    }

    if (op == PUSH_OP_END) {
        uint32_t end_acts = push_session_event(&c->sess, PUSH_EVT_END, NULL);
        if (!(end_acts & PUSH_ACT_VERIFY)) {
            maybe_emit_err(c, end_acts); // 非 RECEIVING 态收到 END：非法转移，回 ERR
            return end_acts;
        }
        // 收全判据：CRC 相等 && session 内长度相等（长度判据在 push_session 的 VERIFY 里）。
        // 一次性收全用流式 crc_acc；续传场景上层已回读 .part 并 set_verified_crc 覆盖。
        const bool crc_ok = c->crc_external_set ? c->crc_external_ok
                                                : (c->crc_acc == c->sess.begin.crc32);
        push_event_t v;
        memset(&v, 0, sizeof(v));
        v.verify_ok = crc_ok;
        uint32_t acts = end_acts | push_session_event(&c->sess, PUSH_EVT_VERIFY, &v);
        c->crc_external_set = false; // 一次性消费
        if (acts & PUSH_ACT_COMMIT) {
            // 记录标题/type 供 S3 落库 sidecar / S4 打开；一次性事件由上层消费。
            memcpy(c->title, c->sess.begin.title, PUSH_TITLE_MAX);
            c->title[PUSH_TITLE_MAX - 1] = 0;
            c->doc_type = c->sess.begin.type;
            c->open_ready = true;
            c->commit_ready = true;
            emit_status(c, PUSH_ST_DONE, NULL, 0);
        } else {
            maybe_emit_err(c, acts); // CHECKSUM：CRC/长度不符 → ST_ERR(4)，.part 保留供续传
            c->open_ready = false;
            c->commit_ready = false;
        }
        return acts;
    }

    if (op == PUSH_OP_ABORT) {
        return push_sink_core_cancel(c);
    }

    // 未知 op：ERR(BAD_FRAME)，状态机不动（与权威件 default 分支一致）。
    c->sess.last_err = PUSH_ERR_BAD_FRAME;
    emit_status(c, PUSH_ST_ERR, &c->sess.last_err, 1);
    return PUSH_ACT_ERR;
}

uint32_t push_sink_core_on_data(push_sink_core_t *c, const uint8_t *frame, size_t len) {
    if (!c || !frame) return 0;
    uint16_t seq = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (!push_data_decode(frame, len, &seq, &payload, &payload_len)) {
        c->sess.last_err = PUSH_ERR_BAD_FRAME;
        emit_status(c, PUSH_ST_ERR, &c->sess.last_err, 1);
        return PUSH_ACT_ERR;
    }
    push_event_t e;
    memset(&e, 0, sizeof(e));
    e.seq = seq;
    e.data = payload;
    e.data_len = payload_len;
    uint32_t acts = push_session_event(&c->sess, PUSH_EVT_DATA, &e);
    if (acts & PUSH_ACT_APPEND) {
        // 只在 seq 连续被接受后才累积 CRC（跳号包不进 CRC，与「不落盘」一致）。
        c->crc_acc = push_crc32_continue(c->crc_acc, payload, payload_len);
        uint8_t body[4]; // ST_PROGRESS body：received(u32 LE)
        const uint32_t rx = c->sess.received;
        body[0] = (uint8_t)(rx & 0xFFu);
        body[1] = (uint8_t)((rx >> 8) & 0xFFu);
        body[2] = (uint8_t)((rx >> 16) & 0xFFu);
        body[3] = (uint8_t)((rx >> 24) & 0xFFu);
        emit_status(c, PUSH_ST_PROGRESS, body, 4);
    } else {
        maybe_emit_err(c, acts); // seq 跳号/超长 → ERR
    }
    return acts;
}

bool push_sink_core_take_open(push_sink_core_t *c, char *title, size_t cap, uint8_t *type) {
    if (!c || !c->open_ready) return false;
    if (title && cap) {
        size_t n = strnlen(c->title, PUSH_TITLE_MAX);
        if (n >= cap) n = cap - 1;
        memcpy(title, c->title, n);
        title[n] = 0;
    }
    if (type) *type = c->doc_type;
    c->open_ready = false; // 一次性
    return true;
}

bool push_sink_core_take_commit(push_sink_core_t *c) {
    if (!c || !c->commit_ready) return false;
    c->commit_ready = false; // 一次性
    return true;
}

size_t push_sink_core_status(const push_sink_core_t *c, uint8_t *out, size_t cap) {
    if (!c || !out || c->status_len == 0 || cap < c->status_len) return 0;
    memcpy(out, c->status, c->status_len);
    return c->status_len;
}
