/*
 * ai_segasm — 0x08 释义分帧重组器（方案乙 S2，纯逻辑、无 ESP-IDF → 宿主可测）。
 * 移植自参考固件 components/push_sink，语义钉死不变，唯 AI_SEGASM_MAX
 * 由 4096 提到 8192（白皮书名义上限；用户裁定 8192）。缓冲为结构体内联数组，
 * 实例化处用静态全局（不占任务栈）。
 *
 * 语义（冻结）：
 *   1. seg_idx==0 = 新条开始：无条件重置并以本帧为头（重发不拼接旧版）。
 *   2. 续帧须「序号连得上 + 同一个词 + total 一致」，否则 REJECT 并重置。
 *   3. 超 AI_SEGASM_MAX 即 OVERFLOW 并重置（不截断：截断 = 假成功）。
 *   4. 按字节流顺序拼接、整体当 UTF-8；total==1 立即完成（等价 0x07）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "push_ai_seg.h"  /* PAS_SEG_EFF_MAX */

#ifdef __cplusplus
extern "C" {
#endif

#define AI_SEGASM_MAX 8192u  /* 方案乙裁定：白皮书 8192（原参考 4096） */

typedef struct {
    char word[64];               /* 正在重组的词（PAS_WORD_SLOT） */
    char buf[AI_SEGASM_MAX];     /* 拼接缓冲（内联；静态实例化） */
    size_t len;                  /* 已拼字节数 */
    uint16_t total;              /* 期望总帧数；0 = 空闲 */
    uint16_t next;               /* 期望的下一个 seg_idx */
    bool active;                 /* 是否有在建条目 */
} ai_segasm_t;

typedef enum {
    AI_SEGASM_PENDING = 0,  /* 还缺帧 */
    AI_SEGASM_DONE,         /* 收齐，可取全文 */
    AI_SEGASM_REJECT,       /* 乱序/词变/total 变（已重置） */
    AI_SEGASM_OVERFLOW,     /* 超上限（已重置） */
} ai_segasm_status_t;

/* 清空重组器（断开、开新条、出错后）。 */
void ai_segasm_reset(ai_segasm_t* a);

/* 喂一帧。DONE 时 out_text/out_len 描述全文；PENDING 时勿读。
   REJECT/OVERFLOW 已 reset，调用方据此走失败提示。 */
ai_segasm_status_t ai_segasm_feed(ai_segasm_t* a, const pas_seg_t* s,
                                  const char** out_text, size_t* out_len);

#ifdef __cplusplus
}
#endif
