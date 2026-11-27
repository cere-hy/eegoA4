/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 000-39b2 · WallpaperDecoder 流式层：HalFile + InflateStream + strip 回调。
 * 纯逻辑（门禁/排序/换序打包/CRC）来自 WallpaperDecoder.h，host 可独立测。
 *
 * 职责边界：只解码。不碰文件读写（HalFile 由调用方打开传入）、不做 ACK、不做上屏。
 * 逐 80 行 strip 交付：调用方写完平面即可释放，再要下一条 —— 峰值仅
 *   两平面 strip(96×80×2≈15KB) + inflate 流式(32KB+8KB) + 行缓冲，无整屏缓冲。
 */
#include "WallpaperDecoderApi.h"

#include <InflateStream.h>
#include <Logging.h>

#include <cstring>

namespace wallpaper {

namespace {

bool readBE32(HalFile& f, uint32_t& v) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) return false;
  v = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
      (static_cast<uint32_t>(b[2]) << 8) | b[3];
  return true;
}

// 注：壁纸规格（APK 保证）每行 filter 恒为 0x00；x4 断言要求非 0 filter 直接拒收。
// 故本解码器不做 SUB/UP/AVG/PAETH 反滤波——保留 F_NONE 语义即可。

// 找下一个 IDAT chunk，跳过 ancillary（含 tRNS：忽略不报错）与其他 chunk。
// 前置 57 字节已消费，调用前 file 停在第 57 字节处（PLTE 的 CRC 之后）。
bool findNextIdat(HalFile& f, uint32_t& chunkRemaining) {
  while (true) {
    uint32_t len;
    if (!readBE32(f, len)) return false;
    uint8_t type[4];
    if (f.read(type, 4) != 4) return false;
    if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
      chunkRemaining = len;
      return true;
    }
    if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') return false;
    if (!f.seekCur(static_cast<int64_t>(len) + 4)) return false;  // data + CRC
  }
}

struct FillCtx {
  HalFile* file;
  uint32_t chunkRemaining;
  bool finished;
  uint8_t buf[2048];
};

size_t fillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<FillCtx*>(vctx);
  if (ctx->finished) return 0;
  while (ctx->chunkRemaining == 0) {
    if (!ctx->file->seekCur(4)) { ctx->finished = true; return 0; }  // 跳过上一个 IDAT 的 CRC
    if (!findNextIdat(*ctx->file, ctx->chunkRemaining)) { ctx->finished = true; return 0; }
  }
  size_t want = sizeof(ctx->buf);
  if (want > ctx->chunkRemaining) want = ctx->chunkRemaining;
  const int n = ctx->file->read(ctx->buf, want);
  if (n <= 0) { ctx->finished = true; return 0; }
  ctx->chunkRemaining -= static_cast<uint32_t>(n);
  *data = ctx->buf;
  return static_cast<size_t>(n);
}

}  // namespace

// 主解码入口。file 必须已定位在 PNG 起始（offset 0）。
// 内部读 57 字节前门禁 → 排序 → 逐 strip 解压/反滤波/换序打包 → 回调。
DecodeResult decodeWallpaper(HalFile& file, StripSink sink, void* sinkCtx, bool swapPlanes) {
  DecodeResult result{kWpUnusable, 0};

  uint8_t header[kWpHeaderBytes];
  if (file.read(header, kWpHeaderBytes) != kWpHeaderBytes) return result;

  WpMeta meta;
  if (checkHeader(header, meta) != kWpOk) return result;

  uint8_t order[4];
  if (sortPalette(header + kPlteDataOffset, order) != kWpOk) return result;

  // 定位 IDAT：前置 57 字节后紧跟的可能是 tRNS 等 ancillary，findNextIdat 会跳过。
  FillCtx fillCtx{&file, 0, false, {}};
  if (!findNextIdat(file, fillCtx.chunkRemaining)) return result;

  InflateStream reader;
  if (!reader.init(true)) return result;  // streaming：32KB window（loan 拿不到走 heap）
  reader.setFill(fillCallback, &fillCtx);
  reader.setZlibWrapped();

  const uint32_t rowBytes = kWpWidth;                 // bitDepth=8/colorType=3：每像素 1 字节索引
  const uint32_t planeRowBytes = kWpWidth / 8;        // 96
  uint8_t rawRow[768];
  uint8_t remappedRow[768];                            // 换序后 code，CRC 覆盖它（x8）
  uint8_t lsbStrip[planeRowBytes * kWpStripRows];     // 96×80 = 7680
  uint8_t msbStrip[planeRowBytes * kWpStripRows];

  uint32_t crc = 0;
  uint16_t y = 0;
  bool aborted = false;

  while (y < kWpHeight) {
    const int rows = (kWpHeight - y < kWpStripRows) ? (kWpHeight - y) : kWpStripRows;
    std::memset(lsbStrip, 0, sizeof(lsbStrip));
    std::memset(msbStrip, 0, sizeof(msbStrip));

    for (int r = 0; r < rows; ++r) {
      uint8_t filter;
      if (!reader.read(&filter, 1)) { reader.deinit(); return result; }
      if (filter != 0) { reader.deinit(); return result; }  // x4：壁纸规格 filter 恒 0，非 0 拒收
      if (!reader.read(rawRow, rowBytes)) { reader.deinit(); return result; }

      // rawRow 即反滤波后的索引原值（filter=0 无需反滤波）；
      // 绝不经过灰度转换（那是 8bit 灰度，不是索引）——陷阱条目。

      if (remapRow(rawRow, order, lsbStrip + static_cast<size_t>(r) * planeRowBytes,
                   msbStrip + static_cast<size_t>(r) * planeRowBytes, swapPlanes,
                   remappedRow) != kWpOk) {
        reader.deinit();
        return result;  // x5：索引越界，不上屏
      }
      crc = crc32Update(crc, remappedRow, rowBytes);  // x8：CRC 覆盖换序后索引（真正上屏内容）
    }

    if (!sink(sinkCtx, y, static_cast<uint8_t>(rows), lsbStrip, msbStrip)) {
      aborted = true;
      break;
    }
    y += static_cast<uint16_t>(rows);
  }
  // x3：553 行拒收。写满 552 行后再尝试读一行的 filter；若还能读出数据，
  // 说明源图行数超过 552（zlib 流未结束）→ 判 unusable，调用方不会 displayGrayBuffer，屏幕保持。
  if (!aborted && y == kWpHeight) {
    uint8_t extra;
    if (reader.read(&extra, 1)) aborted = true;
  }
  reader.deinit();

  if (aborted || y != kWpHeight) return result;
  result.code = kWpOk;
  result.idxCrc = crc;
  return result;
}

}  // namespace wallpaper
