/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：手机 BLE 推流的会话状态机 —— B6.5 S1 完整能力版。源自 pico-plus
 * push_session，按长期定位（设备主输入通道）完整保留：七态（含 COOLDOWN）、
 * 断点续传（RESUME_QUERY/adopt/resume/has_part）、CR C/长度收全判据、动作位掩码。
 * 纯 C11：不碰文件/NVS/射频/时钟，宿主测试驱动全部转移边。
 *
 * 与本令的差异（仅两处业务策略，状态机结构照权威件）：
 *   - 五种 type 全解析；URL(3) 回 ERR(FETCH) 说明不支持；WORD(4) 按普通文本收。
 *   - 不产出 WORD_CARD 特判（单词卡按普通文本落库 type=4）。
 *   AI/TTS/CAPS 帧不属本核（独立 codec），本状态机不定义也不处理。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "push_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 会话状态（七态，含 COOLDOWN，与权威件一致）。
typedef enum {
    PUSH_STATE_IDLE = 0,
    PUSH_STATE_ARMED,
    PUSH_STATE_WINDOW,
    PUSH_STATE_CONNECTED,
    PUSH_STATE_RECEIVING,
    PUSH_STATE_PROCESSING,
    PUSH_STATE_COOLDOWN,
} push_state_t;

/// 状态机事件（与权威件一致）。
typedef enum {
    PUSH_EVT_ARM = 0,
    PUSH_EVT_ADV_UP,
    PUSH_EVT_CONNECT,
    PUSH_EVT_DISCONNECT,
    PUSH_EVT_BEGIN,
    PUSH_EVT_DATA,
    PUSH_EVT_END,
    PUSH_EVT_RESUME_QUERY,
    PUSH_EVT_VERIFY,
    PUSH_EVT_CANCEL,
    PUSH_EVT_TICK,
} push_evt_t;

/// 动作位掩码：胶水执行副作用，状态机不碰外设。编号对齐权威件。
enum {
    PUSH_ACT_ADV_ON = 1u << 0,
    PUSH_ACT_ADV_OFF = 1u << 1,
    PUSH_ACT_READY = 1u << 2,      ///< 回 ST_READY
    PUSH_ACT_RESUME = 1u << 3,     ///< 回 ST_RESUME（offset+next_seq）
    PUSH_ACT_APPEND = 1u << 4,     ///< 把刚收的包写进 .part（胶水执行）
    PUSH_ACT_VERIFY = 1u << 5,     ///< 回读 .part 算 CRC32 + 长度对账
    PUSH_ACT_COMMIT = 1u << 6,     ///< 事务四拍：.part→校验→push.txt→清理（胶水执行）
    PUSH_ACT_DROP = 1u << 7,       ///< 丢弃 .part 与会话
    PUSH_ACT_ERR = 1u << 8,        ///< 回 ST_ERR，码在 last_err
    PUSH_ACT_PROGRESS = 1u << 9,   ///< 回 ST_PROGRESS
    PUSH_ACT_OPEN_BOOK = 1u << 10, ///< 收全通过 → 打开文档事件
};

/// 会话簿记（含 has_part / cooldown 字段，与权威件一致）。
typedef struct {
    push_state_t state;
    push_begin_t begin;
    uint32_t received;   ///< 已收字节数，等于 .part 长度
    uint16_t next_seq;   ///< 期望的下一包序号
    uint32_t window_ms;  ///< 武装窗口时长；0 = 无超时
    uint32_t armed_at_ms;
    uint32_t cooldown_ms;    ///< 续推宽限
    uint32_t cooldown_at_ms; ///< 宽限起点
    uint16_t token;
    bool has_part;       ///< .part 是否已建立（有未完成会话）
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
    uint32_t cooldown_ms;
    bool verify_ok;
    uint32_t now_ms;
} push_event_t;

/// 初始化到 IDLE。
void push_session_init(push_session_t *s);

/// 喂一个事件，返回动作位掩码。非法转移不改状态。
uint32_t push_session_event(push_session_t *s, push_evt_t evt, const push_event_t *arg);

/// 断点续传：把已恢复的会话簿记装回并置 RECEIVING（胶水从 NVS/.part 重建后调用）。
void push_session_resume(push_session_t *s, const push_begin_t *begin, uint32_t received, uint16_t next_seq);

/// 预装会话簿记但不动状态：胶水在用户重新武装时（IDLE 态）从 NVS/.part 恢复半成品用。
/// received == 0 时不装（has_part 假）。
void push_session_adopt(push_session_t *s, const push_begin_t *begin, uint32_t received, uint16_t next_seq);

/// 窗口剩余毫秒；0 表示无超时或已不在窗口。
uint32_t push_session_window_left_ms(const push_session_t *s, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
