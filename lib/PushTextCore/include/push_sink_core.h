/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：文本推送「纯核胶水」——S1 移植的无外设外壳。职责只有三件：
 *   1) 把 CTRL/DATA 字节喂给裁剪版 session；
 *   2) 边收边流式算 CRC32（正文不必整块驻留）；
 *   3) END 时用「CRC 相等 && 长度相等」做收全判定，产出 STATUS 帧与开书事件。
 * 不碰 GATT/文件系统/UI/NVS——这些是 S2–S4 的职责。纯 C11，host 可直接驱动。
 *
 * English: hardware-free glue for S1. It feeds CTRL/DATA bytes into the trimmed
 * session, accumulates CRC32 incrementally, and on END decides "fully received"
 * by (CRC match && length match), emitting STATUS frames and an open event.
 * No GATT / FS / UI / NVS: those belong to S2-S4. Pure C11, host-drivable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "push_proto.h"
#include "push_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 产出的 STATUS 帧容量（ERR 带 1B 码、PROGRESS 带 4B，最坏 5B；留 16）。
#define PUSH_CORE_STATUS_CAP 16u

typedef struct {
    push_session_t sess;      ///< 会话状态机
    uint32_t crc_acc;         ///< 流式 CRC32 累加值
    uint8_t status[PUSH_CORE_STATUS_CAP]; ///< 最近一次产出的 STATUS 帧
    size_t status_len;        ///< STATUS 帧长度（0 = 无）
    bool open_ready;          ///< 收全通过，待上层打开文档（一次性）
    char title[PUSH_TITLE_MAX]; ///< 收全时的文档标题（来自 BEGIN）
    uint8_t doc_type;         ///< 收全时的文档类型
} push_sink_core_t;

/// 复位到初始态（清会话、CRC、产出）。
void push_sink_core_init(push_sink_core_t *c);

/// 用户点接收：now_ms 注入，token/window_ms 同 session。
/// 返回是否成功进入武装（产出 ST 由 status() 取）。
uint32_t push_sink_core_arm(push_sink_core_t *c, uint16_t token, uint32_t window_ms, uint32_t now_ms);

/// 广播起来 / 手机连上 / 断开 / 取消：直接转对应 session 事件。
uint32_t push_sink_core_adv_up(push_sink_core_t *c);
uint32_t push_sink_core_connect(push_sink_core_t *c);
uint32_t push_sink_core_disconnect(push_sink_core_t *c);
uint32_t push_sink_core_cancel(push_sink_core_t *c);
uint32_t push_sink_core_tick(push_sink_core_t *c, uint32_t now_ms);

/// 收一帧 CTRL（含 4B 头）。BEGIN 触发 READY 并重置 CRC；END 触发收全判定。
/// 未知 op 走 ERR(BAD_FRAME)。返回动作位掩码。
uint32_t push_sink_core_on_ctrl(push_sink_core_t *c, const uint8_t *frame, size_t len);

/// 收一帧 DATA（seq(2)+len(1)+payload）。seq 连续才落 CRC；返回动作位掩码。
uint32_t push_sink_core_on_data(push_sink_core_t *c, const uint8_t *frame, size_t len);

/// 取走一次性 open_ready（读走即清）。true 表示有一篇收全的文档待打开。
/// title/doc_type 一并带出。
bool push_sink_core_take_open(push_sink_core_t *c, char *title, size_t cap, uint8_t *type);

/// 最近一次 STATUS 帧：返回长度并拷入 out（cap 足够时）。无 STATUS 返回 0。
size_t push_sink_core_status(const push_sink_core_t *c, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
