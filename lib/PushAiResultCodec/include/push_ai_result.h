/*
 * PushAiResultCodec — 0x07 AI 释义单帧接收核（EEGO A4 rebuild, batch B5）。
 *
 * 权威布局：ReadPicoPush 硬件接入标准白皮书 v1 §2.2「0x07 权威字段表」
 * （三端互证：固件 C push_proto.c + APK Java PushProto + PROTOCOL.md）。
 * 字段偏移以白皮书为准，与 pico-plus push_ai_result_decode 逐字节同构；
 * 不在本层反推硬编码、不复制 pico 实现，仅按权威字段表从零实现。
 *
 * 0x07 线格式（冻结，整帧恒 241 B，零填充）：
 *   offset 0  4B  头 = 50 43 07 01  ('P''C' / opcode 0x07 / version 1)
 *   offset 4  1B  word_len (u8)，< 64
 *   offset 5  64B word 槽（固定占 64，尾部 0 填充）
 *   offset 69 2B  def_len (u16 LE)，< 170（dn=170 装不下 NUL，协议违规拒收）
 *   offset 71 170B def 槽（169 有效 + 1 NUL）
 *   整帧 = 4 + 1 + 64 + 2 + 170 = 241 B（_Static_assert 钉死）。
 *
 * 认帧：带 4B 头（'P''C' 命中）→ payload 从第 4 字节起；裸帧（无头，
 * 老路/直喂）→ 从 0 起。两条都接，由 par_decode() 的 frame_off 参量区分。
 *
 * 双槽 + 原子代际号：BLE 写任务侧 publish，主循环读侧 take_latest。
 * 写只碰非活跃槽、提交时 release 交换索引；读 acquire 快照索引后只读
 * 完整槽——两个任务永不读写同一槽，故不撕裂（host pthread 断言）。
 *
 * 上屏决策：比代际号→追加重排→跳末页；重排失败回滚到上一合格文本。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAR_MAGIC0        (0x50u)
#define PAR_MAGIC1        (0x43u)
#define PAR_OPCODE        (0x07u)
#define PAR_VERSION       (0x01u)
#define PAR_FRAME_LEN     (241u) /* 4+1+64+2+170 */
#define PAR_HDR_LEN       (4u)
#define PAR_WORD_SLOT     (64u)
#define PAR_DEF_SLOT      (170u)
#define PAR_DEF_EFF_MAX   (169u) /* 169 有效 + 1 NUL */
/* 字段区相对偏移（frame_off 之后，裸帧 frame_off=0 / 带头帧 =4）：
   带头帧绝对偏移 = frame_off + 相对偏移 = 4/5/69/71，与白皮书一致。 */
#define PAR_F_WORD_LEN    (0u)
#define PAR_F_WORD        (1u)             /* 1 + 0 */
#define PAR_F_DEF_LEN     (65u)            /* 1 + 64 */
#define PAR_F_DEF         (67u)            /* 65 + 2 */
#define PAR_FIELD_LEN     (237u)           /* 1+64+2+170 */
#define PAR_WORD_EFF_MAX  (63u)

typedef struct {
    char word[PAR_WORD_SLOT];      /* NUL 结尾，UTF-8 */
    char def[PAR_DEF_SLOT];        /* NUL 结尾，UTF-8 */
    uint8_t word_len;
    uint16_t def_len;
} par_result_t;

/* 认帧：返回 payload 起始偏移（带头 4 / 裸帧 0）；非 0/4 视为非法。
   len 必须 >= 整帧剩余长度，否则返回 SIZE_MAX 表拒收。 */
size_t par_frame_offset(const uint8_t* frame, size_t len);

/* 硬解一帧（权威偏移表）。frame 指向整帧起点，frame_off 由 par_frame_offset 给出。
   校验：魔数/版本/opcode（带头时）、word_len<64、def_len<170、总长足够。
   成功写 out 并返回 1；任何违规返回 0 且 out 不变。 */
int par_decode(const uint8_t* frame, size_t len, size_t frame_off, par_result_t* out);

/* UTF-8 安全的字节钳制：截到 <= cap 且不切断多字节序列，返回字节数。 */
size_t par_utf8_clamp(const char* s, size_t n, size_t cap);

/* ---- 双槽 + 原子代际号（SPSC：BLE 写 producer / 主循环 consumer）---- */
/* 不透明句柄：原子/seqlock 成员全部藏进 .c，头文件对 C 与 C++ 均为纯声明。
 * 内部 = 两个 241B 槽 + per-slot seqlock（偶=稳定/奇=写入中）+ 原子活跃索引。
 * 写侧只碰非活跃槽：seq++(奇)→填充→提交 active(release)→seq++(偶)。
 * 读侧 acquire 取 active 后，seq 奇则重试、复制后复查 seq 不变才算成功。
 * 即便生产者连发两帧翻回读侧正在读的槽，seqlock 复查也会捕获并重读，
 * 绝不撕裂（host pthread 并发断言覆盖）。gen[slot] 受 seqlock 保护、单调。 */
typedef struct par_dslot par_dslot_t;

/* 分配并初始化一个双槽实例；失败返回 NULL。 */
par_dslot_t* par_dslot_create(void);
void par_dslot_destroy(par_dslot_t* d);

/* producer（BLE 写任务）：把一帧写进非活跃槽，提交后 generation++。
   返回新的代际号（>=1）；非法输入返回 0（不提交）。 */
uint32_t par_dslot_publish(par_dslot_t* d, const uint8_t* frame, size_t len);

/* consumer（主循环）：读已提交槽的快照。返回代际号（0 = 尚无帧），
   并把 *out_gen 置为该代际号；out_frame/out_len 可为空（只查代际）。 */
uint32_t par_dslot_take_latest(par_dslot_t* d, uint8_t* out_frame,
                               size_t* out_len, uint32_t* out_gen);

/* ---- 上屏决策（纯逻辑，host 可断言）---- */
/* 文本累加上限：单条释义 169B，留若干条重排缓冲。 */
#define PAR_SCREEN_TEXT_MAX (1024u)

typedef struct {
    char text[PAR_SCREEN_TEXT_MAX];  /* 当前已上屏文本（NUL 结尾） */
    size_t text_len;
    char pending[PAR_SCREEN_TEXT_MAX];/* 追加后的候选（供回滚对照） */
    uint32_t last_gen;               /* 已上屏的代际号，防重放 */
    int reorders;                    /* 成功重排次数 */
    int rollbacks;                   /* 回滚次数 */
} par_screen_t;

void par_screen_init(par_screen_t* s);

/* 消费结果上屏：gen <= last_gen 直接丢弃（重放）；否则把 def 追加到候选，
   UTF-8 边界对齐；候选不超上限→提交+重排计数++、返回 1（跳末页由 UI 负责）；
   超上限/非法→回滚（保留旧 text）、rollbacks++、返回 0。 */
int par_screen_apply(par_screen_t* s, uint32_t gen, const par_result_t* r);

/* 十六进制留证：把 len 字节按每行 32B 写入 out（需足够大），
   返回写入字节数。判读行三连由固件侧日志 separately 打。 */
size_t par_hexdump(const uint8_t* frame, size_t len, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
