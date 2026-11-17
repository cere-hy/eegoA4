/*
 * PushSceneCodec — 0x1A 场景/查词上行帧编码器（EEGO A4 rebuild, batch B4）。
 *
 * 权威布局：ReadPicoPush 硬件接入标准白皮书 v1 §2.1「设备发 0x1A」字段表
 * （三端互证、带 file:line）。字段布局以白皮书为准，不反推硬编码。
 *
 * 整帧恒 241 B、零填充：
 *   offset 0   width 1   opcode = 0x1A (PUSH_ST_AI_SCENE)
 *   offset 1   width 2   seq   (u16 LE)，从 0 起
 *   offset 3   width 2   total (u16 LE)，总帧数 1..5
 *   offset 5   width 2   payload (u16 LE)，本帧有效载荷长度（含元数据区）
 *   —— 首帧 (seq==0) 才带下面 4 个元数据字段 ——
 *   offset 7   width 1   word_len  首帧词字节数（后续帧 = 0）
 *   offset 8   width wl  word      首帧词，UTF-8，按 word_len 取（无 NUL 语义）
 *   offset 8+wl width 2 word_off   词在完整 ctx 内的字节偏移（UTF-8，汉字 3 B）
 *   offset 10+wl width 1 flags     见 PSC_FLAG_*
 *   —— ctx 切片区 ——
 *   首帧 ctx 从 11+wl 起，容量 241-(11+wl)=230-wl
 *   后续帧 ctx 从 7 起，容量 241-7=234（整帧 1+2+2+2+234=241）
 *   ctx 按 UTF-8 整字符边界切，任一帧丢失即整条失败（对端 seq==0 重置）。
 *
 * 判据五向量（B4 施工卡 / 点核 §三）：
 *   28B→1帧/payload36、300B→2帧、1024B→5帧、空词拒、199B 词截 63B。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSC_OPCODE      (0x1Au)
#define PSC_FRAME_MAX   (241u)  /* 1+2+2+2+234 */
#define PSC_HDR_LEN     (7u)    /* opcode+seq+total+payload */
#define PSC_WORD_MAX    (63u)   /* 词有效字节上限（白皮书 ≤63） */
#define PSC_CTX_MAX     (1024u) /* 上下文硬上限 */
#define PSC_MAX_FRAMES  (5u)

#define PSC_FLAG_CTX_TRUNCATED   (1u << 0) /* ctx 被截断（末尾落整字符边界） */
#define PSC_FLAG_SENTENCE_FALLBACK (1u << 1) /* 句界未找到、退回页界 */
#define PSC_FLAG_EXCERPT         (1u << 2) /* 整段摘抄：word=锚点, word_off 恒 0 */

/* UTF-8 安全的词截断：取前 ≤PSC_WORD_MAX 字节且不切断多字节序列。
   返回写入 out 的字节数；out 可为 NULL（只查询应截长度）。 */
size_t psc_word_clamp(const uint8_t* word, size_t len, uint8_t* out);

/* 分帧计划：按 UTF-8 整字符边界把 ctx 切成 ≤PSC_MAX_FRAMES 帧。
   返回帧数 1..5；非法输入（空词/超词长/ctx 超上限/单字符放不下/超帧数）返回 0。 */
typedef struct {
    uint16_t off[PSC_MAX_FRAMES]; /* 各帧 ctx 切片在输入 ctx 中的起点 */
    uint16_t len[PSC_MAX_FRAMES]; /* 各帧 ctx 切片字节数 */
    uint8_t  n;                   /* 实际帧数 */
} psc_plan_t;

int psc_plan(const uint8_t* ctx, size_t ctx_len,
             size_t word_len, psc_plan_t* out);

/* 便捷帧数查询（内部即 psc_plan().n；非法返回 0）。 */
int psc_frame_count(const uint8_t* ctx, size_t ctx_len, size_t word_len);

/* 编码第 seq 帧到 out（必须 ≥PSC_FRAME_MAX 字节，函数先 memset 241 零）。
   返回写入字节数恒 PSC_FRAME_MAX；seq 越界或参数非法返回 0。
   word 必须已按 psc_word_clamp 处理（word_len ≤ PSC_WORD_MAX）。 */
size_t psc_frame_encode(uint16_t seq,
                        const uint8_t* word,  size_t word_len,
                        uint16_t word_off, uint8_t flags,
                        const uint8_t* ctx,   size_t ctx_len,
                        psc_plan_t* plan,
                        uint8_t* out);

/* 解码辅助（测试 / 对端校验用）：从 241B 帧读回 header 与字段。
   返回 1=首帧含元数据, 0=后续帧, -1=非法帧。 */
int psc_frame_decode(const uint8_t* frame, size_t frame_len,
                     uint16_t* seq, uint16_t* total, uint16_t* payload,
                     uint8_t*  word, size_t* word_len,
                     uint16_t* word_off, uint8_t* flags,
                     const uint8_t** ctx, size_t* ctx_len);

#ifdef __cplusplus
}
#endif
