/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 000-39b2c · 壁纸接收链纯逻辑核心（host 可测，无 Arduino/HalStorage 依赖）。
 *
 * 职责边界：本模块只管「接收会话 + 结果码 + BUSY 互斥 + 延后触发标记」，
 * 不做 PNG 解码、不做上屏（那是 WallpaperDecoder/showWallpaper 的事，硬约束 4）。
 *
 * 帧格式（000-39b2c 阶段零 0-1 结论，复用权威件 BEGIN/DATA/END）：
 *   - 壁纸会话复用标准 BEGIN/DATA/END；DATA 帧裸 seq(u16 LE)+len(u8)+payload，单包 ≤241B；
 *   - BEGIN 带 total_len + crc32(IEEE 0xEDB88320) 双校验，END 触发校验；
 *   - 落盘路径由固件约定 /wallpaper/ 固定文件名，APK 不指定。
 *   kWpRxOp 是固件侧定义的「壁纸会话标记」，落在白皮书预留段 0x0C–0x0F，
 *   供构造帧验证用；APK 真实帧的判别在 000-39b3 置 bit5 时与此对齐。
 *   000-39b2b: 从 0x0A 迁到 0x0C —— 定性是「预留区内提前挪，为将来同步 gen30
 *   做准备」；gen22 基线上 0x0A 本就是预留、无占用冲突（APK 已确认）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "push_proto.h"
}

namespace wallpaper_rx {

/// 壁纸接收会话标记（固件侧定义；白皮书 CTRL opcode 预留段 0x0C–0x0F）。
/// 非 APK 真实帧 opcode —— APK 复用标准 BEGIN(0x01)/DATA/END(0x02)。
/// 0x0C：预留区内提前挪（为将来同步 gen30 做准备）；gen22 上 0x0A 本就无占用冲突。
constexpr uint8_t kWpRxOp = 0x0C;

/// 日志 stage —— 复用 b2a WpStage 六值集合，不新增词（w5）。
/// 落盘失败映射到 kWpRxStageRefresh（sink/上屏失败，语义最接近写盘失败）。
constexpr uint8_t kWpRxStageNone = 0;
constexpr uint8_t kWpRxStageHeader = 1;
constexpr uint8_t kWpRxStagePlte = 2;
constexpr uint8_t kWpRxStageFilter = 3;
constexpr uint8_t kWpRxStageRowCount = 4;
constexpr uint8_t kWpRxStageIndex = 5;
constexpr uint8_t kWpRxStageRefresh = 6;

/// 接收会话状态。全静态存储，无堆；单线程事件循环访问，BUSY 用 bool 标志（不用锁）。
struct RxState {
  bool busy;           // 上一张还在处理 → PUSH_RES_BUSY
  bool inSession;      // BEGIN 已接受、尚未 END
  uint32_t totalLen;   // BEGIN.total_len（期望字节数）
  uint32_t expectCrc;  // BEGIN.crc32（期望 CRC）
  uint32_t received;   // 已累积字节
  uint32_t crcAcc;     // 流式 CRC32
  bool pending;        // 延后触发：落盘成功只置标志，下次进壁纸界面才解码
  uint32_t pendingCrc; // 落盘图的 CRC，供下次显示对账
  uint8_t lastStage;   // 最近一次失败 stage（WpStage 词汇）
};

inline void reset(RxState& s) {
  s.busy = false;
  s.inSession = false;
  s.totalLen = 0;
  s.expectCrc = 0;
  s.received = 0;
  s.crcAcc = 0;
  s.pending = false;
  s.pendingCrc = 0;
  s.lastStage = kWpRxStageNone;
}

/// 会话标记分发：kWpRxOp 已注册（返回 PUSH_RES_OK）；
/// 其他 opcode 未注册 → 原 PUSH_ERR 路径的 PUSH_ERR_BAD_FRAME（w1）。
inline uint8_t dispatch(RxState& s, uint8_t op) {
  if (op == kWpRxOp) {
    s.lastStage = kWpRxStageNone;
    return PUSH_RES_OK;
  }
  s.lastStage = kWpRxStageHeader;
  return PUSH_ERR_BAD_FRAME;
}

/// IEEE 反射式 CRC32（poly 0xEDB88320），与 zlib / push_crc32 同款。
/// 独立实现以便 host 断言无需链接 push_proto.c。
inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) {
      const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

/// BEGIN：忙 → PUSH_RES_BUSY(3)；非法长度 → PUSH_RES_UNUSABLE(1)；否则起会话返回 OK。
inline uint8_t begin(RxState& s, uint32_t totalLen, uint32_t crc32) {
  if (s.busy || s.inSession) {
    s.lastStage = kWpRxStageHeader;
    return PUSH_RES_BUSY;
  }
  if (totalLen == 0) {
    s.lastStage = kWpRxStageHeader;
    return PUSH_RES_UNUSABLE;
  }
  s.inSession = true;
  s.totalLen = totalLen;
  s.expectCrc = crc32;
  s.received = 0;
  s.crcAcc = 0;
  s.lastStage = kWpRxStageNone;
  return PUSH_RES_OK;
}

/// APPEND：非会话 → UNUSABLE；超长 → UNUSABLE(rowcount)；否则累积字节与 CRC。
inline uint8_t append(RxState& s, const uint8_t* data, size_t len) {
  if (!s.inSession || data == nullptr) {
    s.lastStage = kWpRxStageHeader;
    return PUSH_RES_UNUSABLE;
  }
  if (static_cast<uint64_t>(s.received) + len > s.totalLen) {
    s.lastStage = kWpRxStageRowCount;
    return PUSH_RES_UNUSABLE;
  }
  s.crcAcc = crc32Update(s.crcAcc, data, len);
  s.received += static_cast<uint32_t>(len);
  s.lastStage = kWpRxStageNone;
  return PUSH_RES_OK;
}

/// FINISH：CRC/长度不符 → UNUSABLE(header)；写盘失败 → WRITE_FAIL(refresh)；
/// 成功 → OK 并置 pending（延后触发，不解码不上屏）。会话结束（清 busy/inSession）。
inline uint8_t finish(RxState& s, bool crcOk, bool lenOk, bool writeOk) {
  if (!s.inSession) {
    s.lastStage = kWpRxStageHeader;
    return PUSH_RES_UNUSABLE;
  }
  s.inSession = false;
  s.busy = false;
  if (!lenOk) {
    s.lastStage = kWpRxStageRowCount;
    return PUSH_RES_UNUSABLE;
  }
  if (!crcOk) {
    s.lastStage = kWpRxStageHeader;
    return PUSH_RES_UNUSABLE;
  }
  if (!writeOk) {
    s.lastStage = kWpRxStageRefresh;
    return PUSH_RES_WRITE_FAIL;
  }
  s.pending = true;
  s.pendingCrc = s.crcAcc;
  s.lastStage = kWpRxStageNone;
  return PUSH_RES_OK;
}

/// ABORT：丢弃本次会话，复位（失败复位要求）。
inline void abortSession(RxState& s) { reset(s); }

/// 置 BUSY（硬件层在首次 append 落盘前调用；标志位即可，不用锁）。
inline void markBusy(RxState& s) { s.busy = true; }

/// 消费 pending 标志（下次进壁纸界面调用）；返回是否有待显示的新图。
inline bool takePending(RxState& s, uint32_t* crcOut) {
  if (!s.pending) return false;
  s.pending = false;
  if (crcOut) *crcOut = s.pendingCrc;
  return true;
}

}  // namespace wallpaper_rx
