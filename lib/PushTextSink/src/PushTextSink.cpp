// B6.5 S2/S3 — text-push glue. See include/PushTextSink.h.
#include "PushTextSink.h"

#if defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4

#include <Arduino.h>
#include <BlePushGatt.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PushTextFilter.h>
#include <PushThumb.h>
#include <push_sink_core.h>
#include <WallpaperReceiverApi.h>  // 000-39b3: type=5 帧分流到壁纸接收链（落盘/校验/pending）

// 000-39b3: PUSH_TYPE_WALLPAPER 编译期常量（push_proto.h 由 push_sink_core.h 引入）。
#ifndef PUSH_TYPE_WALLPAPER
#define PUSH_TYPE_WALLPAPER 5
#endif

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>

namespace pushtextsink {
#ifdef PUSH_TEST_INJECT_WRITE_FAIL
// ---- 000-39b1a-t: test-only write-failure injection (never in delivery) ----
// Compiled in ONLY when -DPUSH_TEST_INJECT_WRITE_FAIL is passed. Without the
// macro this whole block is preprocessed away, so the file stays byte-identical
// to the 000-39b1a baseline (asserted by n1: strip-then-hash == 874c425d…).
// Point semantics: 0 = no injection (default), 1 = cleanPart read fails,
// 2 = openFileForWrite fails, 3 = short write / rename fails.
static int g_test_fail_point = 0;  // default 0: a forgotten setting can NOT fail a push
extern "C" void push_test_set_fail_point(int p) { g_test_fail_point = p; }
#endif
namespace {

// ---- paths (fixed; book name is a hardcoded constant per S3 decision) ----
constexpr const char* kPartDir = "/.readpico";
constexpr const char* kPartPath = "/.readpico/push.part";
constexpr const char* kBakPath = "/PUSH.txt.bak";
constexpr const char* kTmpPath = "/PUSH.txt.tmp";
constexpr const char* kBookPath = "/PUSH.txt";  // cache hash key must match reader's path exactly
// S3b P0: don't clear the live reader cache at commit time (a reader may be
// paging through old PUSH content; deleting its section/book cache mid-session
// can drive the rebuild path into a corrupt-metadata abort). Instead drop a
// sticky "dirty" marker OUTSIDE the cache dir; the reader consumes it on next
// open. Keep this literal in sync with EpubReaderActivity.cpp.
constexpr const char* kDirtyPath = "/.crosspoint/push_dirty";

// The one session core. Static storage — no heap. Touched ONLY on the NimBLE
// host task from the CTRL/DATA callbacks below (serialized on that task).
push_sink_core_t g_core;
bool g_ready = false;
uint16_t g_token = 0;
// S3b P3: one-shot completion flag, written on the host task after a successful
// commit, consumed by the main loop (takePushCompleted) to decide auto-open.
std::atomic<bool> g_pushCompleted{false};

// .part streaming handle (HalFile, host task only). Opened with O_WRONLY|O_CREAT
// (no truncate) and seeked to end, so fresh BEGIN (file removed first) starts at
// 0 and a resume appends.
HalFile g_part;
bool g_partOpen = false;
uint32_t g_partStartMs = 0;
uint32_t g_partBytes = 0;  // appends this session (for logging)

// BEGIN fields captured for END verification (core also keeps them, but a
// previous RESUME_QUERY may have re-adopted; these stay authoritative).
uint32_t g_expectCrc = 0;
uint32_t g_expectLen = 0;
bool g_haveBegin = false;
// 000-39b3: 当前会话 type。type=5 走壁纸接收链，不进文本 .part/PUSH.txt。
uint8_t g_sessionType = 0;
// 000-39b3: 壁纸落盘成功 one-shot，main loop 消费后按界面状态决定即时上屏或留 pending。
std::atomic<bool> g_wallpaperDone{false};

/// 000-39b3: 把 wallpaper_rx 的 PUSH_RES_* 结果码翻成 STATUS 帧写进 g_core.status。
/// OK → DONE(0x12)+1B 0；UNUSABLE/WRITE_FAIL/BUSY → ERR(0x13)+1B res code。
static void emitWallpaperResult(uint8_t res) {
  const uint8_t op = (res == PUSH_RES_OK) ? PUSH_ST_DONE : PUSH_ST_ERR;
  const size_t n = push_status_encode(op, &res, 1, g_core.status, sizeof(g_core.status));
  g_core.status_len = n;
  // 用 logPrintf 而非 logf：本函数在匿名 namespace 中早于 logf 定义，
  // 直接调 logf 会被解析成 math.h 的 float logf(float)。
  logPrintf("INF", "WALL", "WALLPAPER RX result=%u STATUS op=0x%02X len=%u",
            (unsigned)res, (unsigned)op, (unsigned)n);
}

// PROGRESS throttle: at most one status notification per 400 ms.
uint32_t g_lastProgressMs = 0;
constexpr uint32_t kProgressGapMs = 400u;

void logf(const char* fmt, ...) {
  char line[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  logPrintf("INF", "BLE", "PUSH %s", line);
}

// ---- .part lifecycle -----------------------------------------------------

void closePart() {
  if (g_partOpen) {
    g_part.flush();
    g_part.close();
    g_partOpen = false;
  }
}

/// Fresh BEGIN: ensure dir, remove stale part, open for append-at-0.
bool beginPart() {
  closePart();
  if (!Storage.exists(kPartDir) && !Storage.mkdir(kPartDir)) {
    logf("part mkdir FAIL %s", kPartDir);
    return false;
  }
  if (Storage.exists(kPartPath) && !Storage.remove(kPartPath)) {
    logf("part remove FAIL %s", kPartPath);
    return false;
  }
  g_part = Storage.open(kPartPath, O_WRONLY | O_CREAT);
  if (!g_part) {
    logf("part open FAIL %s", kPartPath);
    return false;
  }
  g_partOpen = true;
  g_partStartMs = millis();
  g_partBytes = 0;
  return true;
}

/// Resume: open existing .part and seek to end (append).
bool resumePart() {
  closePart();
  if (!Storage.exists(kPartPath)) return false;
  g_part = Storage.open(kPartPath, O_WRONLY | O_CREAT);
  if (!g_part) return false;
  g_partOpen = true;
  g_part.seekSet(g_part.size());
  g_partBytes = (uint32_t)g_part.size();
  g_partStartMs = millis();
  logf("part resume offset=%u", (unsigned)g_partBytes);
  return true;
}

/// Append a DATA payload to the open .part handle. false = short write.
bool appendPart(const uint8_t* payload, size_t len) {
  if (!g_partOpen || !payload) return false;
  const size_t w = g_part.write(payload, len);
  if (w != len) {
    logf("part append SHORT %u/%u", (unsigned)w, (unsigned)len);
    closePart();
    return false;
  }
  g_partBytes += (uint32_t)len;
  return true;
}

// ---- verify + clean + commit ---------------------------------------------

/// Read .part back in 512B chunks: full CRC32 + byte count.
static bool verifyPart(uint32_t* outCrc, uint32_t* outLen) {
  if (!Storage.exists(kPartPath)) return false;
  HalFile f;
  if (!Storage.openFileForRead("PUSH", kPartPath, f)) return false;
  uint8_t buf[512];
  uint32_t crc = 0, total = 0;
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    crc = push_crc32_continue(crc, buf, (size_t)n);
    total += (uint32_t)n;
  }
  if (n < 0) return false;
  *outCrc = crc;
  *outLen = total;
  return true;
}

/// Six-step clean raw .part -> std::string.
static bool cleanPart(std::string& outClean) {
#ifdef PUSH_TEST_INJECT_WRITE_FAIL
  if (g_test_fail_point == 1) {
    logf("TEST INJECT: cleanPart read fail (point 1)");
    return false;
  }
#endif
  if (!Storage.readFileToString("PUSH", kPartPath, PUSH_TOTAL_MAX + 1, outClean)) {
    logf("clean read FAIL");
    return false;
  }
  char* cleaned = NULL;
  push_filter_stats_t st;
  const size_t inLen = outClean.size();
  size_t n = push_text_filter(outClean.data(), inLen, &cleaned, &st);
  if (cleaned == NULL || n == (size_t)-1) {
    if (cleaned) push_text_filter_free(cleaned);
    logf("clean filter FAIL");
    return false;
  }
  outClean.assign(cleaned, n);
  push_text_filter_free(cleaned);
  logf("clean in=%u out=%u ctrl=%u tail=%u blank=%u img=%u fence=%u unclosed=%d",
       (unsigned)inLen, (unsigned)n, (unsigned)st.removed_controls,
       (unsigned)st.dropped_utf8_tail, (unsigned)st.collapsed_runs,
       (unsigned)st.images_removed, (unsigned)st.code_fences, st.unclosed_fence ? 1 : 0);
  return true;
}

// S3cPF6: a single push instant, captured once and shared by the document
// mtime and the cover so both time displays are byte-for-byte同源.
struct PushInstant {
  uint16_t year;
  uint8_t month, day, hour, minute, second;
};
static PushInstant capturePushInstant() {
  const std::time_t now = std::time(nullptr);
  struct tm lt {};
  if (localtime_r(&now, &lt) != nullptr) {
    return {static_cast<uint16_t>(lt.tm_year + 1900), static_cast<uint8_t>(lt.tm_mon + 1),
            static_cast<uint8_t>(lt.tm_mday), static_cast<uint8_t>(lt.tm_hour),
            static_cast<uint8_t>(lt.tm_min), static_cast<uint8_t>(lt.tm_sec)};
  }
  // Fallback: no RTC yet; let SdFat keep its default (we skip stamping).
  return {0, 0, 0, 0, 0, 0};
}
static void formatInstant(const PushInstant& p, char out[32]) {
  if (p.year == 0) {
    std::snprintf(out, 32, "0000-00-00 00:00");
  } else {
    std::snprintf(out, 32, "%04u-%02u-%02u %02u:%02u", p.year, p.month, p.day, p.hour, p.minute);
  }
}

/// Transactional four-beat: PUSH.txt -> .bak -> tmp(cleaned) -> PUSH.txt -> clear .bak.
/// `inst` (when year!=0) is stamped onto the final file so its FAT mtime equals
/// the push instant instead of SdFat's compile-year placeholder.
// 000-39b1: outErr reports a 1B failure code so the caller can turn a
// post-VERIFY disk failure into STATUS ERR(0x13) instead of letting the core's
// pre-emitted DONE(OK) masquerade as success.
// 000-39b1a: every storage failure reports PUSH_ERR_WRITE_FAIL. Storage exposes
// no card-presence signal (SDCardManager::ready() is a one-shot "mounted" flag,
// all file APIs are bare bool), so a distinct "no card" code cannot be told
// apart from read-only mount / full card / IO error. Reporting WRITE_FAIL for
// all of them beats a wrong "no card" that points diagnosis the wrong way.
static bool commitDocument(const std::string& cleaned, const PushInstant& inst, uint8_t& outErr) {
  outErr = PUSH_ERR_WRITE_FAIL;  // default: opened OK, write/rename failed
  {  // 1) cleaned -> tmp
    HalFile t;
#ifdef PUSH_TEST_INJECT_WRITE_FAIL
    if (g_test_fail_point == 2) {
      logf("TEST INJECT: openFileForWrite fail (point 2)");
      return false;  // outErr stays WRITE_FAIL, same as the real open failure
    }
#endif
    if (!Storage.openFileForWrite("PUSH", kTmpPath, t)) {
      logf("commit tmp open FAIL");
      return false;  // outErr already WRITE_FAIL: cannot distinguish cause
    }
    const size_t w = t.write(reinterpret_cast<const uint8_t*>(cleaned.data()), cleaned.size());
    t.flush();
    // S3cPF6: stamp the tmp BEFORE rename so PUSH.txt inherits the push time.
    if (inst.year != 0 &&
        !t.setModificationTime(inst.year, inst.month, inst.day, inst.hour, inst.minute, inst.second)) {
      logf("commit mtime stamp FAIL (mtime may stay placeholder)");
    }
#ifdef PUSH_TEST_INJECT_WRITE_FAIL
    if (g_test_fail_point == 3) {
      logf("TEST INJECT: force short write (point 3)");
      outErr = PUSH_ERR_WRITE_FAIL;
      return false;
    }
#endif
    if (w != cleaned.size()) {
      logf("commit tmp short %u/%u", (unsigned)w, (unsigned)cleaned.size());
      outErr = PUSH_ERR_WRITE_FAIL;  // opened but short write => card full / IO error
      return false;
    }
  }  // closed before rename
  bool hadBak = false;
  if (Storage.exists(kBookPath)) {  // 2) old -> .bak
    Storage.remove(kBakPath);
    if (!Storage.rename(kBookPath, kBakPath)) {
      logf("commit backup FAIL");
      Storage.remove(kTmpPath);
      outErr = PUSH_ERR_WRITE_FAIL;  // opened but rename failed
      return false;
    }
    hadBak = true;
  }
#ifdef PUSH_TEST_INJECT_WRITE_FAIL
  if (g_test_fail_point == 3) {
    logf("TEST INJECT: force final rename fail (point 3)");
    Storage.remove(kTmpPath);
    if (hadBak) Storage.rename(kBakPath, kBookPath);
    outErr = PUSH_ERR_WRITE_FAIL;
    return false;
  }
#endif
  if (!Storage.rename(kTmpPath, kBookPath)) {  // 3) tmp -> PUSH.txt
    logf("commit rename FAIL; rollback");
    if (hadBak) {
      Storage.remove(kBookPath);
      Storage.rename(kBakPath, kBookPath);
    }
    outErr = PUSH_ERR_WRITE_FAIL;  // opened but final rename failed
    return false;
  }
  if (hadBak) Storage.remove(kBakPath);  // 4) clear .bak
  // S3b P0: do NOT clearCache() here. A reader may still be paging through the
  // old PUSH content (its sections/book.bin are the cache we'd delete); deleting
  // it mid-session drove the rebuild path into a corrupt-metadata abort. Instead
  // drop a sticky marker outside the cache dir; the reader consumes it on next
  // open (clearCache + remove marker) and rebuilds from page 1 there.
  // Marker write is best-effort: if it fails, content is already committed.
  if (!Storage.exists(kDirtyPath)) {
    HalFile d;
    if (Storage.openFileForWrite("PUSH", kDirtyPath, d)) {
      d.write(reinterpret_cast<const uint8_t*>("dirty"), 5);
      d.flush();
      logf("dirty marker written");
    } else {
      logf("dirty marker FAIL (cache will not be invalidated)");
    }
  }
  return true;
}

// ---- S3b P4: cover thumbnail ------------------------------------------------
// BMP sink: writes into the HalFile pointed to by ctx.
static size_t thumbSink(void* ctx, const uint8_t* data, size_t len) {
  auto* f = static_cast<HalFile*>(ctx);
  return f ? f->write(data, len) : 0;
}

/// Generate <cachePath>/thumb_400.bmp (same path/format the shelf reads).
/// Best effort: any failure degrades to "no cover"; never affects the push.
static void generatePushThumb(size_t byteCount, const PushInstant& inst) {
  const std::string cacheDir = std::string("/.crosspoint/epub_") +
                               std::to_string(std::hash<std::string>{}(kBookPath));
  const std::string thumbPath = cacheDir + "/thumb_400.bmp";
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(cacheDir.c_str())) {
    logf("thumb FAIL: mkdir");
    return;
  }
  // S3cPF6: use the SAME captured instant as the document mtime — the bmp and
  // the FAT timestamp now share one source (the push moment), so the classic
  // (bmp) and vector (mtime) cover paths agree exactly.
  char timeText[32] = {0};
  formatInstant(inst, timeText);
  pushthumb::CoverInfo info{timeText, byteCount};

  // Remove any stale thumb first (generateThumbBmp's "exists -> skip" would
  // otherwise keep the old cover); write via a fresh handle, then close before
  // anything else touches the file.
  Storage.remove(thumbPath.c_str());
  HalFile f;
  if (!Storage.openFileForWrite("PUSH", thumbPath, f)) {
    logf("thumb FAIL: open %s", thumbPath.c_str());
    return;
  }
  const bool ok = pushthumb::writeCoverBmp(&thumbSink, &f, info);
  f.flush();
  f.close();
  if (!ok) {
    Storage.remove(thumbPath.c_str());
    logf("thumb FAIL: render");
  } else {
    logf("thumb ok bytes=%u", (unsigned)byteCount);
  }
}

/// Drive the core to CONNECTED from whatever state it is in.
void driveConnected(uint32_t now_ms) {
  if (g_core.sess.state == PUSH_STATE_IDLE) {
    push_sink_core_arm(&g_core, g_token, 0u, 0u, now_ms);
  }
  push_sink_core_disconnect(&g_core);
  push_sink_core_adv_up(&g_core);
  push_sink_core_connect(&g_core);
}

/// Take the core's latest STATUS frame (consume) and emit via independent mbuf.
void emitStatus(uint32_t now_ms) {
  uint8_t frame[PUSH_CORE_STATUS_CAP];
  size_t n = push_sink_core_status(&g_core, frame, sizeof(frame));
  g_core.status_len = 0;  // take semantics
  if (n == 0) return;
  const uint8_t op = frame[0];
  if (op == PUSH_ST_PROGRESS) {
    if (g_lastProgressMs != 0 && (uint32_t)(now_ms - g_lastProgressMs) < kProgressGapMs) return;
    g_lastProgressMs = now_ms;
  } else {
    g_lastProgressMs = 0;
  }
  const bool ok = blepushgatt::sendStatusFrame(frame, n);
  logf("STATUS op=0x%02X len=%u notify=%s", (unsigned)op, (unsigned)n, ok ? "ok" : "FAIL");
}

// CTRL hook. 0x07/0x08 are routed to their dedicated hooks by B3, never here.
void onCtrl(uint8_t op, const uint8_t* frame, size_t len) {
  if (!g_ready || frame == nullptr) return;
  const uint32_t now = millis();

  if (op == PUSH_OP_BEGIN) {
    driveConnected(now);
    push_begin_t b;
    const bool haveBegin = push_begin_decode(frame, len, &b);
    if (haveBegin) {
      g_expectCrc = b.crc32;
      g_expectLen = b.total_len;
      g_haveBegin = true;
      g_sessionType = b.type;
    }
    logf("CTRL BEGIN len=%u crc=0x%08X type=%u state=%d",
         (unsigned)g_expectLen, (unsigned)g_expectCrc,
         (unsigned)(haveBegin ? b.type : 0), (int)g_core.sess.state);

    // 000-39b3: type=5 走壁纸接收链，不进文本 .part / PUSH.txt / push_sink_core 状态机。
    // wallpaper_rx 有自己的 BUSY/CRC/长度/pending 语义（PUSH_RES_* 码表）。
    if (haveBegin && b.type == PUSH_TYPE_WALLPAPER) {
      // 000-39b3a 缺陷 A：rxBegin 的返回值是「开始是否成功」，不是「会话结果」。
      // 成功时【绝不】回 DONE——APK 收到 DONE 会立即停发 DATA，图永远收不全。
      // 只有失败(UNUSABLE/WRITE_FAIL/BUSY)才立即回 ERR；会话 OK 只在 END 校验
      // 通过后由下方 END 分支的 rxEnd 结果上报一次。
      const uint8_t res = wallpaper_rx::rxBegin(b.total_len, b.crc32);
      if (res != PUSH_RES_OK) {
        emitWallpaperResult(res);
        emitStatus(now);
      }
      return;
    }

    beginPart();  // fresh document: truncate .part
    push_sink_core_on_ctrl(&g_core, frame, len);
    emitStatus(now);
    return;
  }

  if (op == PUSH_OP_RESUME_QUERY) {
    // 000-39b3: 壁纸会话不支持文本 .part 断点续传；壁纸 RxState 无 resume offset。
    // 断连重连后 APK 重发 BEGIN，rxBegin 若上张未完成会回 BUSY(3)，语义正确。
    if (g_sessionType == PUSH_TYPE_WALLPAPER) return;
    driveConnected(now);
    if (Storage.exists(kPartPath)) {
      uint32_t sz = 0;
      HalFile f;
      if (Storage.openFileForRead("PUSH", kPartPath, f)) sz = (uint32_t)f.size();
      // Re-adopt true offset so ST_RESUME carries received/next_seq.
      push_sink_core_adopt(&g_core, &g_core.sess.begin, sz,
                           (uint16_t)((sz / PUSH_DATA_PAYLOAD_MAX) + 1u));
      resumePart();
      logf("CTRL RESUME_QUERY adopt offset=%u", (unsigned)sz);
    }
    push_sink_core_on_ctrl(&g_core, frame, len);
    emitStatus(now);
    return;
  }

  if (op == PUSH_OP_END) {
    // 000-39b3: type=5 —— 壁纸链 END：rxEnd 内部做磁盘回读 CRC/长度 + 丢弃 sink 解码预校验
    // + 原子 rename + pending。结果码 PUSH_RES_* 直接翻 STATUS（0→DONE，1/2/3→ERR）。
    if (g_sessionType == PUSH_TYPE_WALLPAPER) {
      const uint8_t res = wallpaper_rx::rxEnd();
      emitWallpaperResult(res);
      if (res == PUSH_RES_OK) g_wallpaperDone.store(true, std::memory_order_release);
      emitStatus(now);
      g_sessionType = 0;
      g_haveBegin = false;
      return;
    }
    closePart();
    // Verify from disk BEFORE feeding END: inject the external CRC verdict so
    // the core's VERIFY uses our full-file CRC (covers resume segments too).
    uint32_t crc = 0, len_on_disk = 0;
    bool readOk = verifyPart(&crc, &len_on_disk);
    bool crcOk = readOk && g_haveBegin && (crc == g_expectCrc) && (len_on_disk == g_expectLen);
    logf("END verify read=%s crc=%s disk=%u/%u want=%u/%u ms=%u",
         readOk ? "ok" : "FAIL", crcOk ? "MATCH" : "MISMATCH",
         (unsigned)len_on_disk, (unsigned)crc, (unsigned)g_expectLen, (unsigned)g_expectCrc,
         (unsigned)(millis() - g_partStartMs));
    push_sink_core_set_verified_crc(&g_core, crcOk);
    push_sink_core_on_ctrl(&g_core, frame, len);

    if (push_sink_core_take_commit(&g_core)) {
      uint32_t t0 = millis();
      std::string cleaned;
      const PushInstant inst = capturePushInstant();  // S3cPF6: one instant for mtime + cover
      // 000-39b1a: storage failures are not split into "no card" vs "write";
      // see commitDocument's header note. Both enter here as WRITE_FAIL.
      uint8_t errCode = PUSH_ERR_WRITE_FAIL;
      bool committed = false;
      if (!cleanPart(cleaned)) {
        // Cannot read back .part: missing/corrupt/read-only/IO — all WRITE_FAIL.
      } else {
        committed = commitDocument(cleaned, inst, errCode);
      }
      if (committed) {
        logf("COMMIT ok bytes=%u ms=%u", (unsigned)cleaned.size(), (unsigned)(millis() - t0));
        Storage.remove(kPartPath);  // fully consumed — safe to drop
        generatePushThumb(cleaned.size(), inst);  // S3b P4: best-effort cover; failure is silent
        g_pushCompleted.store(true, std::memory_order_release);  // S3b P3: let main loop auto-open
      } else {
        // 000-39b1 (裁定1): the core already emitted DONE(OK) into g_core.status
        // inside push_sink_core_on_ctrl(END) because VERIFY passed. A disk-write
        // failure AFTER that must override it with ERR(0x13)+1B code — otherwise
        // the APK reports success on a document that was never written (false ACK).
        // Both paths carry a 1B body; text path uses PUSH_ERR_* (fine-grained).
        const uint8_t code = errCode;
        const size_t n = push_status_encode(PUSH_ST_ERR, &code, 1,
                                            g_core.status, sizeof(g_core.status));
        g_core.status_len = n;
        logf("PUSH ACK: STATUS ERR code=%u stage=write (DONE overridden)", (unsigned)code);
        // Keep .part so a retry can rebuild the document without a re-push.
        logf("COMMIT FAIL — original PUSH.txt preserved, .part kept for retry");
      }
    }
    // DONE (now with 1B PUSH_RES_OK body) or the ERR override above; send after commit.
    emitStatus(now);
    return;
  }

  if (op == PUSH_OP_ABORT) {
    if (g_sessionType == PUSH_TYPE_WALLPAPER) {
      wallpaper_rx::rxAbort();  // 删 .part、复位，不动已有 wallpaper.png
      g_sessionType = 0;
      g_haveBegin = false;
    } else {
      closePart();
    }
    logf("CTRL ABORT");
  } else {
    logf("CTRL op=0x%02X recv (state=%d)", (unsigned)op, (int)g_core.sess.state);
  }
  push_sink_core_on_ctrl(&g_core, frame, len);
  emitStatus(now);
}

// DATA hook. Frame is raw seq(u16 LE)+len(u8)+payload, no opcode.
void onData(const uint8_t* frame, size_t len) {
  if (!g_ready || frame == nullptr) return;
  const uint32_t now = millis();
  uint16_t seq = 0;
  const uint8_t* payload = NULL;
  size_t payload_len = 0;
  const bool decodable = push_data_decode(frame, len, &seq, &payload, &payload_len);

  // 000-39b3: type=5 —— DATA 直接喂壁纸接收链（≤241B/片由 rxAppend 内部累积 CRC + 写 .part），
  // 不进文本 .part，也不走 push_sink_core 状态机（壁纸有独立 RxState）。
  if (g_sessionType == PUSH_TYPE_WALLPAPER) {
    if (decodable) {
      const uint8_t res = wallpaper_rx::rxAppend(payload, payload_len);
      if (res != PUSH_RES_OK) {
        wallpaper_rx::rxAbort();
        emitWallpaperResult(res);
        emitStatus(now);
        g_sessionType = 0;
        g_haveBegin = false;
      }
    }
    return;
  }

  const uint32_t acts = push_sink_core_on_data(&g_core, frame, len);
  if ((acts & PUSH_ACT_APPEND) && decodable) {
    if (!appendPart(payload, payload_len)) {
      push_sink_core_cancel(&g_core);
    }
  }
  emitStatus(now);
}

}  // namespace

void init() {
  if (g_ready) return;
  push_sink_core_init(&g_core);
  wallpaper_rx::initReceiver();  // 000-39b3: 确保壁纸目录存在（幂等）
  g_sessionType = 0;
  g_wallpaperDone.store(false, std::memory_order_release);
  g_token = blepushgatt::tokenFromStaMac();
  push_sink_core_arm(&g_core, g_token, 0u, 0u, millis());
  push_sink_core_adv_up(&g_core);
  g_lastProgressMs = 0;
  g_partOpen = false;
  g_partBytes = 0;
  g_haveBegin = false;
  g_ready = true;

  blepushgatt::setCtrlHandler(&onCtrl);
  blepushgatt::setDataHandler(&onData);
  logf("sink init: token=0x%04X state=%d (persist+clean enabled)",
       (unsigned)g_token, (int)g_core.sess.state);
}

bool takePushCompleted() {
  return g_pushCompleted.exchange(false, std::memory_order_acq_rel);
}

// 000-39b3: 壁纸落盘成功 one-shot。main loop 消费后按界面状态决定即时上屏或留 pending。
bool takeWallpaperDone() {
  return g_wallpaperDone.exchange(false, std::memory_order_acq_rel);
}

}  // namespace pushtextsink

#else  // non-EEGO-A4: inert stubs

namespace pushtextsink {
void init() {}
bool takePushCompleted() { return false; }
bool takeWallpaperDone() { return false; }
}  // namespace pushtextsink

#endif
