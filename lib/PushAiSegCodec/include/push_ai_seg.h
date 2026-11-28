/*
 * PushAiSegCodec — 0x08 AI 释义分帧编解码 + 0x18 能力位 + 0x13 错误回执
 * （EEGO A4 rebuild, 方案乙）。
 *
 * 权威布局：ReadPicoPush 硬件接入标准白皮书 gen26.5 §2.2 / §3.4
 * （三端互证：参考固件 push_proto.c + APK PushProto.java + PROTOCOL 变更登记）。
 * 字段偏移与参考固件 push_proto.c 逐字节同构；本层从零实现、不复制其代码。
 *
 * 0x08 线格式（CTRL 写方向 手机→设备；整帧恒 241 B，零填充）：
 *   offset 0   4B  头 = 50 43 08 01  ('P''C' / opcode 0x08 / version 1)
 *   offset 4   1B  word_len (u8)，< 64
 *   offset 5   64B word 槽（固定占 64，尾部 0 填充；每帧都带，重组关连键）
 *   offset 69  2B  seg_idx (u16 LE)，从 0 起，< seg_total
 *   offset 71  2B  seg_total (u16 LE)，>= 1；==1 语义等价 0x07
 *   offset 73  2B  dlen (u16 LE)，< 166（槽保 NUL）
 *   offset 75  166B seg 槽（UTF-8 片段，边界由 APK 保证）
 *   整帧 = 4 + 1 + 64 + 2 + 2 + 2 + 166 = 241 B（_Static_assert 钉死）。
 *
 * 注意：0x08 帧**无截断标志位**（布局只有 idx/total/dlen）。超限判定只能在
 * 重组器本地做（见 AiSegReassembler），超 8192B → OVERFLOW → 设备本地提示。
 *
 * 0x18 能力位 STATUS 帧（设备→手机，5B）：op=0x18 + u32 LE 位图。
 *   CAP_AI_MULTI = (1<<2)；置位才让 APK 对长释义走 0x08。
 * 0x13 错误回执 STATUS 帧：op=0x13 + 1B 错误码（本层仅提供编码器，
 *   本批 AI 错误走设备本地 UI，不依赖 APK 监听——见排期文档 §九 REQ2）。
 *
 * 纯 C11，无 ESP-IDF 依赖 → host 可测。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAS_MAGIC0        (0x50u)
#define PAS_MAGIC1        (0x43u)
#define PAS_OPCODE        (0x08u)
#define PAS_VERSION       (0x01u)

#define PAS_WORD_SLOT     (64u)
#define PAS_SEG_SLOT      (166u)  /* 有效 165 + 1 NUL */
#define PAS_SEG_EFF_MAX   (165u)
#define PAS_FRAME_LEN     (241u)  /* 4+1+64+2+2+2+166 */

/* STATUS 帧常量 */
#define PAS_ST_CAPS       (0x18u)
#define PAS_ST_ERR        (0x13u)
#define PAS_ST_MAX        (16u)
#define PAS_CAP_AI_MULTI  (1u << 2)
/* 000-39b3: bit5 壁纸能力。仅 EEGO 构建在 CAPS(0x18) 应答里与 AI_MULTI 一起置位；
   0x04(AI_MULTI) | 0x20(WALLPAPER) = 0x24 —— APK 侧 D1 cap= 的直接验收值。 */
#define PAS_CAP_WALLPAPER (1u << 5)

/* 0x08 分段载荷（解码结果）*/
typedef struct {
    char word[PAS_WORD_SLOT];  /* NUL 结尾，UTF-8 */
    uint16_t seg_idx;          /* 本帧序号，从 0 起 */
    uint16_t seg_total;        /* 总帧数；>=1；==1 等价 0x07 */
    uint16_t dlen;             /* 本帧有效字节数，< 166 */
    char seg[PAS_SEG_SLOT];    /* NUL 结尾的 UTF-8 片段 */
} pas_seg_t;

/* 认帧并解码一帧 0x08。成功返回 true；头部/长度/序号非法返回 false。
   frame 为整帧起点（含 4B CTRL 头），len 必须恰为 241。 */
bool pas_seg_decode(const uint8_t* frame, size_t len, pas_seg_t* out);

/* 编码一帧 0x08（241B 零填充）。参数非法返回 0。仅 host test/未来回环使用。 */
size_t pas_seg_encode(const pas_seg_t* s, uint8_t* out, size_t cap);

/* 编 0x18 CAPS 帧（5B）：op + u32 LE bits。cap 不足返回 0。 */
size_t pas_caps_encode(uint32_t bits, uint8_t* out, size_t cap);

/* 编 0x13 ERR 帧（2B）：op + code。cap 不足返回 0。 */
size_t pas_err_encode(uint8_t code, uint8_t* out, size_t cap);

#ifdef __cplusplus
}
#endif
