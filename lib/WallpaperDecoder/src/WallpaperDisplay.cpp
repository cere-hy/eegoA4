/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 000-39b2 · 接线层：SD 卡 PNG → WallpaperDecoder → 七步 Absolute 上屏。
 * 七步照抄 EpubReaderActivity.cpp:2326-2500。不置 bit5、不做 ACK。
 *
 * 「不上屏」契约：解码失败（含索引越界/553行/filter非0）时不调用
 * displayGrayBuffer()，驱动离屏缓冲里的半成品不会到达面板，屏幕保持之前内容。
 */
#include "WallpaperDecoderApi.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <push_proto.h>

#include <cstdio>

namespace wallpaper {

namespace {

// strip sink：把一条带的两平面写入驱动离屏缓冲（不触发上屏）。
struct SinkCtx {
  GfxRenderer* renderer;
  bool ok;
};

bool stripSink(void* vctx, uint16_t yStart, uint8_t numRows,
               const uint8_t* lsb, const uint8_t* msb) {
  auto* ctx = static_cast<SinkCtx*>(vctx);
  if (!ctx->renderer->supportsStripGrayscale()) { ctx->ok = false; return false; }
  ctx->renderer->writeGrayscalePlaneStrip(true, lsb, yStart, numRows);
  ctx->renderer->writeGrayscalePlaneStrip(false, msb, yStart, numRows);
  return true;
}

}  // namespace

uint8_t showWallpaper(GfxRenderer& renderer, const char* pngPath, bool swapPlanes) {
  // /wallpaper/ 目录（含父级）确保存在；失败不回 ERR —— 后续 openFileForRead 失败自会返回 UNUSABLE。
  if (!Storage.exists("/wallpaper")) Storage.mkdir("/wallpaper", true);

  // 第 1 步：能力判定。A4 应支持 Absolute；不支持 → UNUSABLE，不写半幅。
  if (!renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported())
    return PUSH_RES_UNUSABLE;

  // 第 2 步：先起 Absolute 灰度传递，再写平面。
  // 必须先于 writeGrayscalePlaneStrip：后者经 acceptGrayscaleRows 累加 _grayRows，
  // 而 displayGrayscaleBase 会清零 _grayRows —— 顺序反了会丢记账，displayGrayBuffer 判定行数不足而取消。
  // 注意：beginGrayscale(Absolute) 对驱动是 no-op（不动屏幕、不动已写平面），
  // 故解码失败时不调 displayGrayBuffer() 即可保证屏幕保持之前内容。
  const auto baseMode = HalDisplay::FAST_REFRESH;
  if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute, baseMode)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return PUSH_RES_UNUSABLE;
  }

  // 打开 PNG（解码器只读，不改文件）。
  HalFile file;
  if (!Storage.openFileForRead("WALL", pngPath, file)) return PUSH_RES_UNUSABLE;

  SinkCtx sinkCtx{&renderer, true};
  DecodeResult dec = decodeWallpaper(file, stripSink, &sinkCtx, swapPlanes);
  file.close();
  if (dec.code != kWpOk || !sinkCtx.ok) {
    // 解码失败：取消本次灰度传递（_grayPassFailed=true），不 displayGrayBuffer → 屏幕保持。
    renderer.setRenderMode(GfxRenderer::BW);
    return PUSH_RES_UNUSABLE;
  }

  // 第 5 步：提交上屏（两平面已写满 552 行）。
  renderer.waitRefreshComplete();
  renderer.displayGrayBuffer();

  // 第 6/7 步：复位渲染模式 + cleanup。
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.cleanupGrayscaleWithFrameBuffer();

  // 判据日志：CRC 覆盖换序后索引，双长度由尺寸算出。
  LOG_INF("WALL", "WALLPAPER_PATH=direct");
  LOG_INF("WALL", "WALLPAPER_IDX_CRC=%08x idx_len=%u raw_len=%u",
       static_cast<unsigned>(dec.idxCrc),
       static_cast<unsigned>(kWpIdxLen),
       static_cast<unsigned>(kWpRawLen));
  return PUSH_RES_OK;
}

}  // namespace wallpaper
