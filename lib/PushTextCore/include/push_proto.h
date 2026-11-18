/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：手机 BLE「文本推送」帧格式与 CRC32 的裁剪版纯核。源自 pico-plus
 * components/push_sink，按 B6.5 S1 裁剪原则只保留文本推送必需路径：
 *   - 保留：BEGIN/DATA/CTRL/STATUS 编解码、CRC32、文本类型与错误码。
 *   - 去掉：大文件/断点续传/事务 commit 相关；AI 查词(0x06/0x07/0x08/0x09)、
 *     TTS(0x19)、能力位(0x18) 全部帧——这些在 eegoA4 由独立 codec 承载，
 *     本核不定义也不覆盖，0x07/0x08 判据一律不动。
 * 纯 C11，不依赖 ESP-IDF：字节序全部显式小端，宿主与目标逐字段一致。
 *
 * English: trimmed pure core of the phone BLE TEXT-push frame format and CRC32.
 * Derived from pico-plus components/push_sink, trimmed for B6.5 S1 to the
 * text-push-only path: BEGIN/DATA/CTRL/STATUS codecs + CRC32 + text types/errors
 * are kept; large-file / resume / transactional commit paths are dropped, and
 * every AI/TTS/caps frame is intentionally absent (owned by separate codecs).
 * Pure C11, no ESP-IDF: little-endian everywhere, host and target byte-identical.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 常量 / Constants ---- */

#define PUSH_MAGIC0 0x50u ///< 'P'
#define PUSH_MAGIC1 0x43u ///< 'C'
#define PUSH_VERSION 1u   ///< 帧格式版本 / frame format version

#define PUSH_TITLE_MAX 64u            ///< 标题 UTF-8 上限（含结尾 0）
#define PUSH_DATA_PAYLOAD_MAX 241u    ///< MTU 247 时每包载荷
#define PUSH_TOTAL_MAX (512u * 1024u) ///< 正文硬上限（裁剪版仍保留，作为恶意 total_len 守卫）

/// CTRL 操作码。裁剪版只实现 BEGIN/END；其余枚举值保留以对齐 APK 冻结协议编号，
/// 本核对非 BEGIN/END 的 CTRL 帧只做头校验、交上层按未知 op 处理。
enum {
    PUSH_OP_BEGIN = 0x01,
    PUSH_OP_END = 0x02,
    PUSH_OP_RESUME_QUERY = 0x03, ///< 编号占位：本核不实现断点续传
    PUSH_OP_ABORT = 0x04,        ///< 编号占位
    /* 0x05..0x09 / 0x0A..0x0F 属 SET_TIME/AI/预留区，本核不定义 */
};

#define PUSH_CTRL_HDR_LEN 4u ///< magic(2) + op(1) + version(1)

/// STATUS 操作码（设备→手机）。裁剪版只用到 READY/DONE/ERR/PROGRESS；其余占位说明。
enum {
    PUSH_ST_READY = 0x10,    ///< 会话已接受 / session accepted
    PUSH_ST_RESUME = 0x11,   ///< 占位：本核不实现续传
    PUSH_ST_DONE = 0x12,     ///< 校验通过 / verified
    PUSH_ST_ERR = 0x13,      ///< 拒绝，body 是错误码 / rejected
    PUSH_ST_PROGRESS = 0x16, ///< 接收进度 / progress
};

/// 正文类型。URL/WORD 裁剪版不作为特殊路径（题述「单词卡按普通文本处理」）。
enum {
    PUSH_TYPE_TEXT = 0,
    PUSH_TYPE_MARKDOWN = 1,
    PUSH_TYPE_HTML = 2,
    PUSH_TYPE_WORD = 4, ///< 按普通文本处理，不做词典卡特判
};

/// 错误码（STATUS ERR 的 body）。
enum {
    PUSH_ERR_NONE = 0,
    PUSH_ERR_NOT_ARMED = 1,
    PUSH_ERR_BAD_FRAME = 2, ///< 魔数/版本/长度/seq 非法
    PUSH_ERR_TOO_LARGE = 3, ///< total_len 超上限
    PUSH_ERR_CHECKSUM = 4,  ///< CRC32 不符 / 长度不符（收全判据失败）
    PUSH_ERR_NO_CARD = 5,
    PUSH_ERR_BUSY = 7,
};

/// BEGIN 帧（83 字节，全部小端）—— 冻结布局，与 APK 参考实现逐字节一致。
typedef struct {
    uint32_t total_len;  ///< 正文字节数
    uint32_t crc32;      ///< 正文 CRC32
    uint32_t session_id; ///< 会话标识（字段保留，裁剪版不做续传对账）
    uint16_t token;      ///< 广播名后缀
    uint8_t type;        ///< PUSH_TYPE_*
    char title[PUSH_TITLE_MAX]; ///< UTF-8，截断保 0
} push_begin_t;

#define PUSH_BEGIN_LEN 83u  ///< BEGIN 帧字节数
#define PUSH_DATA_HDR_LEN 3u ///< DATA 帧头：seq(2)+len(1)
#define PUSH_STATUS_MAX 16u  ///< STATUS 帧上限

/* ---- CRC32 ---- */

/// IEEE 反射式 CRC32（poly 0xEDB88320），与 zlib 同款。
uint32_t push_crc32(const void *data, size_t len);

/// 流式 CRC32：crc 传上一段返回值（首段传 0）。链式等价于对拼接后一次运算。
/// 这是裁剪版「边收边算、END 时一次性对账」的核心，正文不必整块驻留。
uint32_t push_crc32_continue(uint32_t crc, const void *data, size_t len);

/* ---- BEGIN 编解码 / BEGIN codec ---- */

/// 序列化 BEGIN 到 out（至少 PUSH_BEGIN_LEN 字节），返回写入字节数。
size_t push_begin_encode(const push_begin_t *begin, uint8_t *out, size_t cap);

/// 反序列化 len 字节到 begin。坏魔数/坏版本/长度不足返回 false。
bool push_begin_decode(const uint8_t *in, size_t len, push_begin_t *begin);

/* ---- DATA / CTRL 帧头 ---- */

/// 取出 DATA 帧的 seq 与载荷长度；帧非法返回 false。
bool push_data_decode(const uint8_t *in, size_t len, uint16_t *seq,
                      const uint8_t **payload, size_t *payload_len);

/// 校验 CTRL 公共头（魔数 + 版本 + 长度 ≥4）并取 opcode。
bool push_ctrl_header_decode(const uint8_t *in, size_t len, uint8_t *op);

/* ---- STATUS 编解码 / STATUS codec ---- */

/// 编码一条 STATUS：opcode + 至多 12 字节 body，返回帧长（0 表示 cap 不够）。
size_t push_status_encode(uint8_t opcode, const uint8_t *body, size_t body_len,
                          uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
