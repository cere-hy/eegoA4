/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 000-39b2 · WallpaperDecoder 对外 API（硬件侧）。
 * 与 WallpaperDecoder.h（纯逻辑、host 可测）分离：本头依赖 HalFile。
 *
 * 关键契约：「不上屏」= 解码失败时不调用 displayGrayBuffer()。
 * writeGrayscalePlaneStrip 只写驱动的离屏缓冲（_grayLsb/_grayMsb），
 * 错误发生前已写入的 strip 不会到达面板，屏幕保持之前内容。
 */
#pragma once

#include "WallpaperDecoder.h"
#include <GfxRenderer.h>
#include <HalStorage.h>

namespace wallpaper {

// 每条 strip 回调：写一条带的两平面到驱动离屏缓冲。返回 false 表示放弃。
using StripSink = bool (*)(void* ctx, uint16_t yStart, uint8_t numRows,
                           const uint8_t* lsb, const uint8_t* msb);

struct DecodeResult {
  uint8_t code;      // kWpOk / kWpUnusable
  uint8_t stage;     // WpStage：首个失败阶段（成功为 kWpStageNone）
  uint32_t idxCrc;   // 换序后索引的 CRC32
  uint8_t head[kWpHeadTailBytes];  // 换序后索引首 16B（IDX_HEAD）
  uint8_t tail[kWpHeadTailBytes];  // 换序后索引末 16B（IDX_TAIL）
};

// file 已定位在 PNG offset 0。解码并逐 strip 回调；调用方据此写平面。
// 返回 code=kWpOk 且 idxCrc 有效时，调用方才可执行 displayGrayBuffer() 上屏。
// 任何失败都填好 stage，且绝不调 sink 之后的上屏动作。
DecodeResult decodeWallpaper(HalFile& file, StripSink sink, void* sinkCtx, bool swapPlanes);

// ---- 接线层：从 SD 卡 /wallpaper/ 读图 → 解码 → 七步上屏 ----
// 结果码语义与 PUSH_RES_* 一致（0=OK 1=UNUSABLE 2=WRITE_FAIL）。
// swapPlanes：位序双版开关（0=索引即MSB，1=反序）。
// expectedHex：判据用期望 CRC 的 8 hex 字符串；nullptr 或缺省打印 '-'。
// 返回 PUSH_RES_OK 仅当解码成功且已 displayGrayBuffer；任何失败屏幕保持之前内容，
// 并打 WALLPAPER_ERR code=<n> stage=<...>（五个 stage 值），成功打三行 CRC/HEAD/TAIL。
uint8_t showWallpaper(GfxRenderer& renderer, const char* pngPath, bool swapPlanes,
                      const char* expectedHex = nullptr);

}  // namespace wallpaper
