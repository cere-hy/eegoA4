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

// stage 名（WALLPAPER_ERR stage=<...>）。与 WpStage 一一对应，本批五值不扩。
const char* stageName(uint8_t stage) {
  switch (stage) {
    case kWpStageHeader:   return "header";
    case kWpStagePlte:     return "plte";
    case kWpStageFilter:   return "filter";
    case kWpStageRowCount: return "rowcount";
    case kWpStageIndex:    return "index";
    case kWpStageRefresh:  return "refresh";
    default:               return "header";
  }
}

// 16 字节 → 32 个小写 hex 字符（IDX_HEAD/IDX_TAIL）。
void bytesToHex(const uint8_t* bytes, size_t n, char* out) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[i * 2]     = kHex[bytes[i] >> 4];
    out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
  }
  out[n * 2] = '\0';
}

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

// 拒收后的唯一灰度会话复位点。
// GfxRenderer::setRenderMode(BW) 内部：if (mode==BW && absoluteGrayPlanes)
//   display.cleanupGrayscaleBuffers(nullptr)  // cancel an unfinished absolute pass
//   absoluteGrayPlanes=false —— 即此处显式复位 _grayscaleMode，避免停留在 Absolute。
// 本仓库没有对外暴露 cancelGrayscalePass()，故 setRenderMode(BW) 是本路径唯一复位点，
// 不依赖「下次 displayBuffer() 的 cancelGrayscalePass()」那个隐含行为。
static void resetGraySession(GfxRenderer& renderer) {
  renderer.setRenderMode(GfxRenderer::BW);
}

uint8_t showWallpaper(GfxRenderer& renderer, const char* pngPath, bool swapPlanes,
                      const char* expectedHex) {
  // /wallpaper/ 目录（含父级）确保存在；失败不回 ERR —— 后续 openFileForRead 失败自会返回 UNUSABLE。
  if (!Storage.exists("/wallpaper")) Storage.mkdir("/wallpaper", true);

  // 第 1 步：能力判定。A4 应支持 Absolute；不支持 → UNUSABLE，不写半幅。
  if (!renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported()) {
    LOG_ERR("WALL", "WALLPAPER_ERR code=1 stage=header");
    return PUSH_RES_UNUSABLE;
  }

  // 第 2 步：先起 Absolute 灰度传递，再写平面。
  // 必须先于 writeGrayscalePlaneStrip：后者经 acceptGrayscaleRows 累加 _grayRows，
  // 而 displayGrayscaleBase 会清零 _grayRows —— 顺序反了会丢记账，displayGrayBuffer 判定行数不足而取消。
  // 注意：beginGrayscale(Absolute) 对驱动是 no-op（不动屏幕、不动已写平面），
  // 故解码失败时不调 displayGrayBuffer() 即可保证屏幕保持之前内容。
  const auto baseMode = HalDisplay::FAST_REFRESH;
  if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute, baseMode)) {
    resetGraySession(renderer);
    LOG_ERR("WALL", "WALLPAPER_ERR code=1 stage=refresh");
    return PUSH_RES_UNUSABLE;
  }

  // 打开 PNG（解码器只读，不改文件）。
  HalFile file;
  if (!Storage.openFileForRead("WALL", pngPath, file)) {
    resetGraySession(renderer);
    LOG_ERR("WALL", "WALLPAPER_ERR code=1 stage=header");
    return PUSH_RES_UNUSABLE;
  }

  SinkCtx sinkCtx{&renderer, true};
  DecodeResult dec = decodeWallpaper(file, stripSink, &sinkCtx, swapPlanes);
  file.close();
  if (dec.code != kWpOk || !sinkCtx.ok) {
    // 解码失败：显式复位灰度会话（唯一复位点），不 displayGrayBuffer → 屏幕保持之前内容。
    resetGraySession(renderer);
    // stage 归一化：sink 放弃时 decodeWallpaper 已置 refresh；这里兜底防御。
    uint8_t stage = dec.stage;
    if (stage == kWpStageNone) stage = (!sinkCtx.ok) ? kWpStageRefresh : kWpStageHeader;
    LOG_ERR("WALL", "WALLPAPER_ERR code=%d stage=%s", static_cast<int>(dec.code), stageName(stage));
    return PUSH_RES_UNUSABLE;
  }

  // 第 5 步：提交上屏（两平面已写满 552 行）。
  renderer.waitRefreshComplete();
  renderer.displayGrayBuffer();

  // 第 6/7 步：复位渲染模式 + cleanup。
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.cleanupGrayscaleWithFrameBuffer();

  // 判据日志：三行都在解码成功后打印。CRC 单行含 expected/swap；
  // IDX_HEAD/IDX_TAIL 取换序后索引首末 16B（与 CRC 同一份数据，反序件判据 3 才对得上）。
  const char* exp = (expectedHex && *expectedHex) ? expectedHex : "-";
  LOG_INF("WALL", "WALLPAPER_PATH=direct swap=%d", swapPlanes ? 1 : 0);
  LOG_INF("WALL", "WALLPAPER_IDX_CRC=%08x idx_len=%u raw_len=%u expected=%s swap=%d",
       static_cast<unsigned>(dec.idxCrc),
       static_cast<unsigned>(kWpIdxLen),
       static_cast<unsigned>(kWpRawLen), exp, swapPlanes ? 1 : 0);
  char headHex[kWpHeadTailBytes * 2 + 1];
  char tailHex[kWpHeadTailBytes * 2 + 1];
  bytesToHex(dec.head, kWpHeadTailBytes, headHex);
  bytesToHex(dec.tail, kWpHeadTailBytes, tailHex);
  LOG_INF("WALL", "IDX_HEAD %s", headHex);
  LOG_INF("WALL", "IDX_TAIL %s", tailHex);
  return PUSH_RES_OK;
}

}  // namespace wallpaper
