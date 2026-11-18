/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：文本推送「纯核胶水」——B6.5 S1 完整能力版的无外设外壳。职责：
 *   1) 把 CTRL/DATA 字节喂给完整状态机（含断点续传 / cooldown / 五 type）；
 *   2) 边收边流式算 CRC32，END 时以「CRC 相等 && 长度相等」做收全判定；
 *   3) 把动作位翻译成 STATUS 帧（READY/RESUME/DONE/ERR/PROGRESS）与开书/提交事件。
 * 不碰 GATT/文件系统/NVS/UI——.part 落盘、NVS 对账、事务四拍 .bak 回滚、
 * 自动打开属 S2–S4。纯 C11，host 可直接驱动。
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

/// 产出的 STATUS 帧容量（RESUME 带 6B、ERR 带 1B、PROGRESS 带 4B；留 16）。
#define PUSH_CORE_STATUS_CAP 16u

typedef struct {
    push_session_t sess;      ///< 会话状态机
    uint32_t crc_acc;         ///< 流式 CRC32 累加值（边收边算）
    uint8_t status[PUSH_CORE_STATUS_CAP]; ///< 最近一次产出的 STATUS 帧
    size_t status_len;        ///< STATUS 帧长度（0 = 无）
    bool open_ready;          ///< 收全通过，待上层打开文档（一次性）
    bool commit_ready;        ///< 收全通过，待上层执行事务四拍提交（一次性）
    char title[PUSH_TITLE_MAX]; ///< 收全时的文档标题（来自 BEGIN）
    uint8_t doc_type;         ///< 收全时的文档类型（来自 BEGIN.type，落库 sidecar 用）
    uint32_t resume_offset;   ///< 最近一次 RESUME 回执的 offset（= received）
    uint16_t resume_next_seq; ///< 最近一次 RESUME 回执的 next_seq
    bool crc_external_set;    ///< true = VERIFY 用上层回读 .part 的外部 CRC 结论
    bool crc_external_ok;     ///< 外部 CRC 结论（与 begin.crc32 比对结果）
} push_sink_core_t;

/// 复位到初始态（清会话、CRC、产出）。
void push_sink_core_init(push_sink_core_t *c);

/// 用户点接收：token/window_ms/cooldown_ms 注入，now_ms 为时刻。
uint32_t push_sink_core_arm(push_sink_core_t *c, uint16_t token, uint32_t window_ms,
                            uint32_t cooldown_ms, uint32_t now_ms);

/// 广播起来 / 手机连上 / 断开 / 取消 / 周期事件：转对应 session 事件。
uint32_t push_sink_core_adv_up(push_sink_core_t *c);
uint32_t push_sink_core_connect(push_sink_core_t *c);
uint32_t push_sink_core_disconnect(push_sink_core_t *c);
uint32_t push_sink_core_cancel(push_sink_core_t *c);
uint32_t push_sink_core_tick(push_sink_core_t *c, uint32_t now_ms);

/// 断点续传桥接（胶水从 NVS/.part 重建后调用）：
/// - adopt：预装簿记不动状态（重新武装时用），received==0 不装；
/// - resume：装簿记并进 RECEIVING（已连上直接续收时用）。
/// 两者都把 crc_acc 置 0——续收的 CRC 由上层在 VERIFY 时用回读 .part 的全量 CRC
/// 覆盖（见 push_sink_core_verify_with_crc），流式累加仅用于「一次性收全」场景。
void push_sink_core_adopt(push_sink_core_t *c, const push_begin_t *begin,
                          uint32_t received, uint16_t next_seq);
void push_sink_core_resume(push_sink_core_t *c, const push_begin_t *begin,
                           uint32_t received, uint16_t next_seq);

/// 用回读 .part 得到的全量 CRC 做 VERIFY 结论（续传场景必经）。
/// 调用后紧接着喂 END 即可；crc_match 应已在胶水层与 begin.crc32 比对好。
void push_sink_core_set_verified_crc(push_sink_core_t *c, bool crc_match);

/// 收一帧 CTRL（含 4B 头）。BEGIN→READY 或 RESUME；RESUME_QUERY→ST_RESUME；
/// END→收全判定；ABORT→取消；未知 op→ERR(BAD_FRAME)。返回动作位掩码。
uint32_t push_sink_core_on_ctrl(push_sink_core_t *c, const uint8_t *frame, size_t len);

/// 收一帧 DATA（seq(2)+len(1)+payload）。seq 连续才流转式 CRC；返回动作位掩码。
uint32_t push_sink_core_on_data(push_sink_core_t *c, const uint8_t *frame, size_t len);

/// 取走一次性 open_ready（读走即清）。title/doc_type 一并带出。
bool push_sink_core_take_open(push_sink_core_t *c, char *title, size_t cap, uint8_t *type);

/// 取走一次性 commit_ready（读走即清）。上层据此执行事务四拍 .bak 提交。
bool push_sink_core_take_commit(push_sink_core_t *c);

/// 最近一次 STATUS 帧：返回长度并拷入 out（cap 足够时）。无 STATUS 返回 0。
size_t push_sink_core_status(const push_sink_core_t *c, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
