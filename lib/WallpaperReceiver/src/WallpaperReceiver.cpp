// 000-39b2c · 壁纸接收链硬件层实现。见 include/WallpaperReceiverApi.h。
#include "WallpaperReceiverApi.h"

#if defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WallpaperDecoderApi.h>  // 000-39b2c: 落盘前纯解码校验（不上屏）

#include <cstdarg>
#include <cstdio>

namespace wallpaper_rx {

RxState g_rx;
namespace {
HalFile g_part;
bool g_partOpen = false;

void logRx(const char* fmt, ...) {
  char line[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  logPrintf("INF", "WALL", "WALLPAPER_RX %s", line);
}

void closePart() {
  if (g_partOpen) {
    g_part.flush();
    g_part.close();
    g_partOpen = false;
  }
}

// stage 词汇复用 b2a WpStage 六值集合（w5），不新增词。
const char* stageOf(uint8_t stage) {
  switch (stage) {
    case kWpRxStagePlte:     return "plte";
    case kWpRxStageFilter:   return "filter";
    case kWpRxStageRowCount: return "rowcount";
    case kWpRxStageIndex:    return "index";
    case kWpRxStageRefresh:  return "refresh";
    case kWpRxStageHeader:
    default:                 return "header";
  }
}
}  // namespace

bool initReceiver() {
  reset(g_rx);
  if (!Storage.exists(kWallpaperDir) && !Storage.mkdir(kWallpaperDir, true)) {
    logRx("rejected stage=write code=%u (mkdir)", static_cast<unsigned>(PUSH_RES_WRITE_FAIL));
    return false;
  }
  return true;
}

uint8_t rxBegin(uint32_t totalLen, uint32_t crc32) {
  const uint8_t r = begin(g_rx, totalLen, crc32);
  if (r != PUSH_RES_OK) {
    logRx("rejected stage=header code=%u (busy=%d)", static_cast<unsigned>(r), g_rx.busy ? 1 : 0);
    return r;
  }
  // Fresh session: ensure dir, drop stale part, open for write.
  if (!Storage.exists(kWallpaperDir) && !Storage.mkdir(kWallpaperDir, true)) {
    abortSession(g_rx);
    logRx("rejected stage=write code=%u (mkdir)", static_cast<unsigned>(PUSH_RES_WRITE_FAIL));
    return PUSH_RES_WRITE_FAIL;
  }
  if (Storage.exists(kWallpaperPart) && !Storage.remove(kWallpaperPart)) {
    abortSession(g_rx);
    logRx("rejected stage=write code=%u (rm stale part)", static_cast<unsigned>(PUSH_RES_WRITE_FAIL));
    return PUSH_RES_WRITE_FAIL;
  }
  closePart();
  g_part = Storage.open(kWallpaperPart, O_WRONLY | O_CREAT);
  if (!g_part) {
    abortSession(g_rx);
    logRx("rejected stage=write code=%u (open part)", static_cast<unsigned>(PUSH_RES_WRITE_FAIL));
    return PUSH_RES_WRITE_FAIL;
  }
  g_partOpen = true;
  markBusy(g_rx);
  logRx("begin len=%u", static_cast<unsigned>(totalLen));
  return PUSH_RES_OK;
}

uint8_t rxAppend(const uint8_t* payload, size_t len) {
  const uint8_t r = append(g_rx, payload, len);
  if (r != PUSH_RES_OK) {
    logRx("rejected stage=%s code=%u",
          g_rx.lastStage == kWpRxStageRowCount ? "rowcount" : "header", static_cast<unsigned>(r));
    return r;
  }
  if (!g_partOpen) {
    abortSession(g_rx);
    logRx("rejected stage=write code=%u (part closed)", static_cast<unsigned>(PUSH_RES_WRITE_FAIL));
    return PUSH_RES_WRITE_FAIL;
  }
  const size_t w = g_part.write(payload, len);
  if (w != len) {
    closePart();
    abortSession(g_rx);
    logRx("rejected stage=write code=%u (short write)", static_cast<unsigned>(PUSH_RES_WRITE_FAIL));
    return PUSH_RES_WRITE_FAIL;
  }
  return PUSH_RES_OK;
}

uint8_t rxEnd() {
  closePart();
  if (!g_rx.inSession) {
    logRx("rejected stage=header code=%u (no session)", static_cast<unsigned>(PUSH_RES_UNUSABLE));
    return PUSH_RES_UNUSABLE;
  }

  // 回读 .part 做整段 CRC + 长度校验（与 PushTextSink.verifyPart 同范式）。
  uint32_t diskCrc = 0, diskLen = 0;
  bool readOk = false;
  if (Storage.exists(kWallpaperPart)) {
    HalFile f;
    if (Storage.openFileForRead("WALL", kWallpaperPart, f)) {
      uint8_t buf[512];
      int n;
      while ((n = f.read(buf, sizeof(buf))) > 0) {
        diskCrc = crc32Update(diskCrc, buf, static_cast<size_t>(n));
        diskLen += static_cast<uint32_t>(n);
      }
      f.close();
      readOk = (n >= 0);
    }
  }

  const bool lenOk = readOk && (diskLen == g_rx.totalLen) && (g_rx.received == g_rx.totalLen);
  const bool crcOk = lenOk && (diskCrc == g_rx.expectCrc);

  if (!lenOk || !crcOk) {
    // 失败：删 part，保留已有 wallpaper.png 不动，复位会话（失败复位要求）。
    Storage.remove(kWallpaperPart);
    const uint8_t r = finish(g_rx, crcOk, lenOk, true);
    logRx("rejected stage=%s code=%u (disk %u/%u want %u crc=%08x/%08x)",
          g_rx.lastStage == kWpRxStageRowCount ? "rowcount" : "header",
          static_cast<unsigned>(r), static_cast<unsigned>(diskLen),
          static_cast<unsigned>(diskCrc), static_cast<unsigned>(g_rx.totalLen),
          static_cast<unsigned>(diskCrc), static_cast<unsigned>(g_rx.expectCrc));
    return r;
  }

  // 落盘前纯解码校验：57B 前缀门禁抓不到 IDAT 内的 0x40/553行/filter!=0，
  // 必须完整 inflate。校验在 .part 上做，丢弃 sink 不上屏 → 不打断阅读（0-4）。
  // 判据0 已保证 40KB 可拿到；拿不到则此处 UNUSABLE，接收链不落盘（与判据0 停手一致）。
  {
    uint8_t vstage = kWpRxStageNone;
    const uint8_t vcode = wallpaper::validateWallpaperFile(kWallpaperPart, &vstage);
    if (vcode != wallpaper::kWpOk) {
      Storage.remove(kWallpaperPart);  // 坏图不落盘，旧 wallpaper.png 保持不动（判据2）
      g_rx.inSession = false;
      g_rx.busy = false;
      g_rx.lastStage = vstage;
      logRx("rejected stage=%s code=%u (decode preflight)",
            stageOf(vstage), static_cast<unsigned>(PUSH_RES_UNUSABLE));
      return PUSH_RES_UNUSABLE;
    }
  }

  // 原子落盘：.part → wallpaper.png。rename 覆盖旧图；失败则保留旧图 + 删 part。
  // Storage.rename 在目标存在时先替换（SdFat 语义）；为稳妥先移除旧目标再 rename，
  // 但仅在校验通过后才动旧图——失败路径（上分支）从不触碰旧图。
  if (Storage.exists(kWallpaperFinal) && !Storage.remove(kWallpaperFinal)) {
    Storage.remove(kWallpaperPart);
    const uint8_t r = finish(g_rx, true, true, false);
    logRx("rejected stage=write code=%u (rm old)", static_cast<unsigned>(r));
    return r;
  }
  if (!Storage.rename(kWallpaperPart, kWallpaperFinal)) {
    Storage.remove(kWallpaperPart);
    const uint8_t r = finish(g_rx, true, true, false);
    logRx("rejected stage=write code=%u (rename)", static_cast<unsigned>(r));
    return r;
  }

  const uint8_t r = finish(g_rx, true, true, true);
  logRx("saved path=/wallpaper/wallpaper.png bytes=%u crc=%08x",
        static_cast<unsigned>(diskLen), static_cast<unsigned>(diskCrc));
  return r;
}

void rxAbort() {
  closePart();
  Storage.remove(kWallpaperPart);
  reset(g_rx);
  logRx("aborted");
}

uint8_t rxFeedFromFile(const char* name) {
  if (!name || !*name) return PUSH_RES_UNUSABLE;
  const String src = String(kWallpaperDir) + "/" + String(name);
  HalFile f;
  if (!Storage.openFileForRead("WALL", src, f)) {
    logRx("rejected stage=header code=%u (open src)", static_cast<unsigned>(PUSH_RES_UNUSABLE));
    return PUSH_RES_UNUSABLE;
  }
  const uint32_t total = static_cast<uint32_t>(f.size());
  if (total == 0) {
    f.close();
    return PUSH_RES_UNUSABLE;
  }
  // 整段 CRC32（等价于 BEGIN.crc32），先算源字节。
  uint32_t srcCrc = 0;
  uint8_t buf[PUSH_DATA_PAYLOAD_MAX];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    srcCrc = crc32Update(srcCrc, buf, static_cast<size_t>(n));
  }
  f.close();

  uint8_t r = rxBegin(total, srcCrc);
  if (r != PUSH_RES_OK) return r;

  // 重新打开，按 ≤241B 分片喂入（模拟 DATA 帧 seq+len+payload）。
  if (!Storage.openFileForRead("WALL", src, f)) {
    rxAbort();
    return PUSH_RES_UNUSABLE;
  }
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    r = rxAppend(buf, static_cast<size_t>(n));
    if (r != PUSH_RES_OK) { f.close(); rxAbort(); return r; }
  }
  f.close();
  return rxEnd();
}

}  // namespace wallpaper_rx

#else  // non-EEGO-A4: inert stubs

namespace wallpaper_rx {
RxState g_rx;
bool initReceiver() { return false; }
uint8_t rxBegin(uint32_t, uint32_t) { return PUSH_RES_UNUSABLE; }
uint8_t rxAppend(const uint8_t*, size_t) { return PUSH_RES_UNUSABLE; }
uint8_t rxEnd() { return PUSH_RES_UNUSABLE; }
void rxAbort() {}
uint8_t rxFeedFromFile(const char*) { return PUSH_RES_UNUSABLE; }
}  // namespace wallpaper_rx

#endif
