/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 000-39b2 · WallpaperDecoder —— palette PNG 索引直通 2bpp 双平面的纯逻辑核心。
 *
 * 本头是 header-only、零 ESP 依赖：只含 <stdint.h>/<stddef.h>，因此 host 断言
 * (000-39b2_host_assert.cpp) 可以直接 #include 并运行 x1-x9 的全部行为用例。
 * 硬件 I/O（HalFile / InflateStream / strip 回调）在 WallpaperDecoder.cpp。
 *
 * 数据流（严格四步，绝不经 tone/dither）：
 *   1. checkHeader()  —— 57 字节前置门禁（签名8 + IHDR25 + PLTE24），验尺寸/位深/颜色类型/interlace
 *   2. sortPalette()  —— 选 C：读 PLTE 四组 RGB，按亮度排序，输出 order[原索引]=code(0..3)
 *   3. remapRow()     —— 逐索引换序 + 越界校验 + 打包进 strip 双平面
 *   4. crc32Update()  —— CRC 流式累加「换序后」索引（真正上屏的内容）
 *
 * 约定：
 *   - 像素索引 0..3；code 0..3（0=黑 1=深灰 2=浅灰 3=白）
 *   - code 的 bit1 → MSB 平面，bit0 → LSB 平面；字节内高位在前（与 PngToBmpConverter 2bpp 同位序）
 *   - WALLPAPER_SWAP_PLANE：0=索引即 MSB（默认），1=两平面互换（位序实测用）
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef WALLPAPER_SWAP_PLANE
#define WALLPAPER_SWAP_PLANE 0
#endif

namespace wallpaper {

// ---- 规格常量（768×552 有效区，物理 768×600 底部 48 行白 padding 由驱动自动补）----
constexpr uint16_t kWpWidth = 768;
constexpr uint16_t kWpHeight = 552;
constexpr uint8_t kWpStripRows = 80;
constexpr size_t kWpIdxLen = static_cast<size_t>(kWpWidth) * kWpHeight;       // 423936
constexpr size_t kWpRawLen = static_cast<size_t>(kWpWidth + 1) * kWpHeight;   // 424488
constexpr size_t kWpHeaderBytes = 57;                                          // 8 + 25 + 24

// 结果码：与 PUSH_RES_* 对齐（0=OK 1=UNUSABLE 2=WRITE_FAIL 3=BUSY）；解码器只产 0/1。
constexpr uint8_t kWpOk = 0;
constexpr uint8_t kWpUnusable = 1;

// 000-39b2a: 失败阶段（WALLPAPER_ERR stage=<值>）。解码器在首个失败点记录，
// 接线层统一打印；五值 + None，本批不扩。分段后真机排查一眼可分。
enum WpStage : uint8_t {
  kWpStageNone = 0,       // 成功
  kWpStageHeader = 1,     // 57B 前置门禁 / IDAT 定位 / inflate 初始化失败
  kWpStagePlte = 2,       // PLTE 守卫失败
  kWpStageFilter = 3,     // 某行 filter != 0
  kWpStageRowCount = 4,   // 截断(<552行) 或 超行(>552行，多读一字节命中)
  kWpStageIndex = 5,      // 索引越界 > 3
  kWpStageRefresh = 6,    // sink 终止 / displayGrayscaleBase 失败
};

// IDX_HEAD / IDX_TAIL 各取「换序后」索引缓冲的首末 16 字节（= 32 hex 字符），
// 与 CRC 覆盖同一份数据（x8 口径一致，反序件判据 3 才能对上）。
constexpr size_t kWpHeadTailBytes = 16;
constexpr size_t kWpFilenameMax = 63;

// ---- 前置门禁 ----
struct WpMeta {
  uint32_t width;
  uint32_t height;
  uint8_t bitDepth;
  uint8_t colorType;
  uint8_t interlace;
  uint16_t plteLen;      // PLTE 块 data 长度，应为 12
};

inline uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// 校验 57 字节前缀。返回 kWpOk / kWpUnusable。成功时 meta 被填充。
// 只验签名、IHDR 六字段（width/height/bitDepth/colorType/compression/filter/interlace 中的六项，
// compression 与 filter 一并要求为 0）、PLTE 存在且长度 12。tRNS 若在 PLTE 之后出现，由流式层跳过（忽略不报错）。
inline uint8_t checkHeader(const uint8_t* p57, WpMeta& meta) {
  static const uint8_t SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  for (int i = 0; i < 8; ++i) if (p57[i] != SIG[i]) return kWpUnusable;

  // IHDR：offset 8 起，len(4)=13 type(4)="IHDR" data(13) crc(4) = 25 字节
  if (be32(p57 + 8) != 13u) return kWpUnusable;
  if (p57[12] != 'I' || p57[13] != 'H' || p57[14] != 'D' || p57[15] != 'R') return kWpUnusable;
  meta.width = be32(p57 + 16);
  meta.height = be32(p57 + 20);
  meta.bitDepth = p57[24];
  meta.colorType = p57[25];
  const uint8_t compression = p57[26];
  const uint8_t filter = p57[27];
  meta.interlace = p57[28];
  if (meta.width != kWpWidth) return kWpUnusable;
  if (meta.height != kWpHeight) return kWpUnusable;
  if (meta.bitDepth != 8) return kWpUnusable;
  if (meta.colorType != 3) return kWpUnusable;   // PNG_COLOR_PALETTE
  if (compression != 0 || filter != 0) return kWpUnusable;
  if (meta.interlace != 0) return kWpUnusable;

  // PLTE：offset 33 起，len(4) type(4)="PLTE" data(12) crc(4) = 24 字节
  meta.plteLen = static_cast<uint16_t>(be32(p57 + 33));
  if (meta.plteLen != 12) return kWpUnusable;
  if (p57[37] != 'P' || p57[38] != 'L' || p57[39] != 'T' || p57[40] != 'E') return kWpUnusable;
  return kWpOk;
}

// PLTE data 在 57 字节前缀内的偏移（供 sortPalette 取四组 RGB）
constexpr size_t kPlteDataOffset = 41;  // 8 + 25 + 4(len) + 4(type)

// 选 C：按亮度排序四组 PLTE。plte12 为 12 字节（4×RGB）。
// outOrder[原索引 0..3] = code 0..3（0=最暗→黑，3=最亮→白）。
// 三条守卫任一不满足 → kWpUnusable：
//   1) 每项近似灰度 |R-G|<=4 且 |G-B|<=4
//   2) 四项亮度两两差 >= 8
//   3) 非全同亮度（被 2 覆盖，显式保留成断言语义）
inline uint8_t sortPalette(const uint8_t* plte12, uint8_t outOrder[4]) {
  uint16_t lum[4];
  for (int i = 0; i < 4; ++i) {
    const uint8_t r = plte12[i * 3 + 0];
    const uint8_t g = plte12[i * 3 + 1];
    const uint8_t b = plte12[i * 3 + 2];
    if (r > g ? (r - g) > 4 : (g - r) > 4) return kWpUnusable;
    if (g > b ? (g - b) > 4 : (b - g) > 4) return kWpUnusable;
    lum[i] = static_cast<uint16_t>((static_cast<uint32_t>(r) * 25u +
                                    static_cast<uint32_t>(g) * 50u +
                                    static_cast<uint32_t>(b) * 25u) / 100u);
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      const uint16_t d = lum[i] > lum[j] ? lum[i] - lum[j] : lum[j] - lum[i];
      if (d < 8) return kWpUnusable;
    }
  }
  // 排序：按亮度升序，code = 排序位次。亮度唯一（上一步保证两两差>=8），无并列。
  for (int i = 0; i < 4; ++i) outOrder[i] = 0;
  for (int i = 0; i < 4; ++i) {
    uint8_t rank = 0;
    for (int j = 0; j < 4; ++j) if (lum[j] < lum[i]) ++rank;
    outOrder[i] = rank;
  }
  return kWpOk;
}

// 把一行 768 个索引换序 + 越界校验 + 打包进两条 strip 平面行（每行 widthBytes=96 字节）。
// idx: 反滤波后的原始索引行（每字节一索引，bitDepth=8/colorType=3）。
// order: sortPalette 输出。lsb/msb: 96 字节输出。swap: 位序双版开关。
// remappedOut: 可选（可为 nullptr），写出换序后 code 序列 768 字节，供 CRC 累加（x8）。
// 任一索引 > 3 → kWpUnusable（x5：不上屏）。
inline uint8_t remapRow(const uint8_t* idx, const uint8_t order[4],
                        uint8_t* lsb, uint8_t* msb, bool swap,
                        uint8_t* remappedOut = nullptr) {
  constexpr int W = kWpWidth;
  constexpr int WB = W / 8;  // 96
  for (int b = 0; b < WB; ++b) { lsb[b] = 0; msb[b] = 0; }
  for (int x = 0; x < W; ++x) {
    const uint8_t raw = idx[x];
    if (raw > 3) return kWpUnusable;
    const uint8_t code = order[raw];
    if (remappedOut) remappedOut[x] = code;
    const uint8_t bit = static_cast<uint8_t>(0x80u >> (x & 7));
    const uint8_t planeMsb = (code & 0x02u) ? bit : 0;
    const uint8_t planeLsb = (code & 0x01u) ? bit : 0;
    const uint8_t toMsb = swap ? planeLsb : planeMsb;
    const uint8_t toLsb = swap ? planeMsb : planeLsb;
    msb[x >> 3] |= toMsb;
    lsb[x >> 3] |= toLsb;
  }
  return kWpOk;
}

// ---- CRC32（IEEE 反射 poly 0xEDB88320），与 push_crc32_continue 同语义 ----
// 首段传 0；后续段传上一段返回值。链式等价于对拼接后一次运算。
inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  uint32_t c = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    c ^= data[i];
    for (int k = 0; k < 8; ++k)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return c ^ 0xFFFFFFFFu;
}

// ---- 000-39b2a 测试接口（纯逻辑，host 行为向可测）----

// y1/y6 行数门禁：写完 kWpHeight 行后多读一字节，若还能读出数据 → 源图行数 > 552
// → 返回 kWpUnusable（stage=rowcount，由调用方记录）；read 返回 false(zlib 流正常结束) → kWpOk。
// 抽成纯函数供 host 行为向喂 true/false；流式层用它判定，不能只做字符串匹配。
inline uint8_t classifyExtraRow(bool extraByteReadable) {
  return extraByteReadable ? kWpUnusable : kWpOk;
}

// y4 CMD 文件名白名单：只接受 /wallpaper/ 下的文件名，拒绝空串、含 '/'、含 ".."、
// 长度 > kWpFilenameMax、以及绝对路径。合法返回 true。喂三个恶意串验证。
inline bool isSafeWallpaperName(const char* name) {
  if (name == nullptr) return false;
  size_t len = 0;
  for (const char* p = name; *p != '\0'; ++p) {
    ++len;
    if (len > kWpFilenameMax) return false;
    if (*p == '/') return false;                 // 不接受任何路径分隔符
    if (*p == '\\') return false;                // 反斜杠同样拒绝
    if (*p == '.' && *(p + 1) == '.') return false;  // 拒绝 ".."
  }
  if (len == 0) return false;
  return true;
}

// 从「换序后」索引缓冲取首末 16 字节，填进 hex 输出（IDX_HEAD/IDX_TAIL）。
// remapped: 长度 kWpIdxLen 的换序 code 序列；head/tail: 各 16 字节。
inline void extractHeadTail(const uint8_t* remapped, uint8_t head[kWpHeadTailBytes],
                            uint8_t tail[kWpHeadTailBytes]) {
  for (size_t i = 0; i < kWpHeadTailBytes; ++i) {
    head[i] = remapped[i];
    tail[i] = remapped[kWpIdxLen - kWpHeadTailBytes + i];
  }
}

}  // namespace wallpaper
