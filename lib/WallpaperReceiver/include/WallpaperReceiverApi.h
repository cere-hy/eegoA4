/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 000-39b2c · 壁纸接收链硬件层（依赖 HalStorage）。
 * 纯逻辑见 WallpaperReceiver.h；本层只做「构造帧→校验→原子落盘→置 pending」。
 * 不解码、不上屏——延后触发，下次进壁纸界面由 CMD:wallpaper 消费 pending。
 */
#pragma once

#include "WallpaperReceiver.h"

namespace wallpaper_rx {

/// 固定落盘路径（硬编码前缀，不接受上游指定，w6）。
constexpr const char* kWallpaperDir = "/wallpaper";
constexpr const char* kWallpaperPart = "/wallpaper/wallpaper.png.part";
constexpr const char* kWallpaperFinal = "/wallpaper/wallpaper.png";

/// 全局会话状态（单实例）。
extern RxState g_rx;

/// 初始化：确保 /wallpaper/ 目录存在。
bool initReceiver();

/// 开始一张壁纸接收，返回 PUSH_RES_*。totalLen/crc32 取自 BEGIN 帧。
uint8_t rxBegin(uint32_t totalLen, uint32_t crc32);

/// 追加一段 DATA 载荷，返回 PUSH_RES_*。内部写 .part + 累积 CRC。
uint8_t rxAppend(const uint8_t* payload, size_t len);

/// END：关闭 .part → 回读 CRC/长度校验 → rename 落盘 → 置 pending。
/// 返回 PUSH_RES_*。失败不覆盖已有 wallpaper.png，并复位会话。
uint8_t rxEnd();

/// 放弃本次接收，删除 .part，复位（失败复位要求）。
void rxAbort();

/// 000-39b3a: 空闲超时阈值 kWpRxIdleTimeoutMs 定义在 WallpaperReceiver.h（纯逻辑头），
/// 本层与 host 断言共用同一常量（15s）。

/// 000-39b3a: 空闲超时兜底。由主循环节拍周期调用（不新开定时器）。
/// 会话在 kWpRxIdleTimeoutMs 内无任何 BEGIN/DATA 活动 → rxAbort() + 打 timeout 日志，
/// 清 busy 使下次 BEGIN 可成功。无会话或未超时时是廉价 no-op。
void rxIdleCheck(uint32_t nowMs);

/// 000-39b2c 验证接口：从 /wallpaper/<name> 读取 PNG，按标准 DATA 分片（≤241B）
/// 喂给接收链 rxBegin/rxAppend/rxEnd。路径白名单由调用方先校验。
/// 返回最终 PUSH_RES_*；预期 CRC 取自源文件的整段 CRC32（与 BEGIN 帧口径一致）。
uint8_t rxFeedFromFile(const char* name);

}  // namespace wallpaper_rx
