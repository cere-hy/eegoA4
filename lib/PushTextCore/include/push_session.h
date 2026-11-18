/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：文本推送会话状态机的裁剪版。源自 pico-plus push_session，按 S1 裁剪：
 *   保留 IDLE→ARMED→WINDOW→CONNECTED→RECEIVING→PROCESSING 主路径 + seq 连续性 +
 *   长度判据 + CRC 验证动作位；去掉断点续传(RESUME/adopt/has_part)、事务 commit、
 *   cooldown 续推宽限、单词卡特判。纯 C11，不碰文件/时钟/射频，宿主全边可测。
 * English: trimmed text-push session machine. Keeps the main path and the
 * DATA-seq / length / CRC verdicts; drops resume, transactional commit, cooldown
 * and word-card special-casing. Pure C11, no file/clock/radio, fully host-testable.
 *
 * 冻结：状态集与转移边以权威件设计草案 §4 的主路径为准；本裁剪不新增状态。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "push_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 会话状态（裁剪后 6 态；无 COOLDOWN）。
typedef enum {
    PUSH_STATE_IDLE = 0,   ///< 未武装
    PUSH_STATE_ARMED,      ///< 已点接收，等广播
    PUSH_STATE_WINDOW,     ///< 窗口期，等手机连接
    PUSH_STATE_CONNECTED,  ///< 已连接，未开始传输
    PUSH_STATE_RECEIVING,  ///< BEGIN 已接受，正文流式中
    PUSH_STATE_PROCESSING, ///< END 已到，等 CRC/长度对账
} push_state_t;

/// 事件（裁剪掉 RESUME_QUERY / UNPLUG）。
typedef enum {
    PUSH_EVT_ARM = 0,
    PUSH_EVT_ADV_UP,
    PUSH_EVT_CONNECT,
    PUSH_EVT_DISCONNECT,
    PUSH_EVT_BEGIN,
    PUSH_EVT_DATA,
    PUSH_EVT_END,
    PUSH_EVT_VERIFY, ///< 胶水的 CRC/长度结论
    PUSH_EVT_CANCEL,
    PUSH_EVT_TICK,   ///< 周期事件，arg.now_ms
} push_evt_t;

/// 动作位掩码：胶水执行副作用，状态机不碰外设。
enum {
    PUSH_ACT_ADV_ON = 1u << 0,
    PUSH_ACT_ADV_OFF = 1u << 1,
    PUSH_ACT_READY = 1u << 2,      ///< 回 ST_READY
    PUSH_ACT_APPEND = 1u << 3,     ///< 把刚收的包写进正文缓冲（胶水执行）
    PUSH_ACT_VERIFY = 1u << 4,     ///< 对正文算 CRC32 + 长度对账
    PUSH_ACT_DROP = 1u << 5,       ///< 丢弃本次会话缓冲
    PUSH_ACT_ERR = 1u << 6,        ///< 回 ST_ERR，码在 last_err
    PUSH_ACT_PROGRESS = 1u << 7,   ///< 回 ST_PROGRESS
    PUSH_ACT_OPEN_BOOK = 1u << 8,  ///< 收全通过 → 打开文档事件
};

/// 会话簿记（无 has_part / cooldown 字段）。
typedef struct {
    push_state_t state;
    push_begin_t begin;
    uint32_t received;   ///< 已收字节数
    uint16_t next_seq;   ///< 期望的下一包序号
    uint32_t window_ms;  ///< 武装窗口时长；0 = 无超时
    uint32_t armed_at_ms;
    uint16_t token;
    int last_err;
} push_session_t;

/// 事件参数：BEGIN 与 DATA 带载荷，VERIFY 带结论，其余忽略。
typedef struct {
    const push_begin_t *begin;
    const uint8_t *data;
    size_t data_len;
    uint16_t seq;
    uint16_t token;
    uint32_t window_ms;
    bool verify_ok;
    uint32_t now_ms;
} push_event_t;

/// 初始化到 IDLE。
void push_session_init(push_session_t *s);

/// 喂一个事件，返回动作位掩码。非法转移不改状态。
uint32_t push_session_event(push_session_t *s, push_evt_t evt, const push_event_t *arg);

/// 窗口剩余毫秒；0 表示无超时或已不在窗口。
uint32_t push_session_window_left_ms(const push_session_t *s, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
