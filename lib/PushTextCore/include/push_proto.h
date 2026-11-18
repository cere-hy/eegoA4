/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：手机 BLE「文本推送」帧格式与 CRC32 —— B6.5 S1 完整能力版。
 * 源自 pico-plus components/push_sink，按长期定位（设备主输入通道）保留：
 *   - BEGIN/DATA/CTRL/STATUS 编解码、CRC32、512KB 上限、五种 type 全解析；
 *   - 断点续传协议号（0x03 RESUME_QUERY / STATUS 0x11 / session_id 对账锚）。
 * 不搬（去重，非阉割）：AI 帧常量与 codec（0x06/0x07/0x08/0x09）、TTS(0x19)、
 * 能力位(0x18)、URL 抓取——eegoA4 已有独立 PushAiResultCodec/PushAiSegCodec
 * 承载 AI，0x07/0x08 判据一律不动；type=3(URL) 协议层认，业务回 ERR 说明不支持。
 * 纯 C11，不依赖 ESP-IDF：字节序全部显式小端，宿主与目标逐字段一致。
 *
 * 冻结：BEGIN 83B 布局、opcode 编号、STATUS 编号均对齐权威件，不得单方改字段。
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
#define PUSH_TOTAL_MAX (512u * 1024u) ///< 正文硬上限（长期定位：传书按此规划）

/// CTRL 操作码。编号对齐权威件冻结协议；本核实现文本推送子集，AI 段不定义。
enum {
    PUSH_OP_BEGIN = 0x01,
    PUSH_OP_END = 0x02,
    PUSH_OP_RESUME_QUERY = 0x03, ///< 重连后问断点 / ask for the resume point after reconnect
    PUSH_OP_ABORT = 0x04,        ///< 放弃本次会话 / abandon the session
    /* 0x05 SET_TIME / 0x06-0x09 AI / 0x0A-0x0F 预留：属其他模块，本核不定义 */
};

#define PUSH_CTRL_HDR_LEN 4u ///< magic(2) + op(1) + version(1)

/// STATUS 操作码（设备→手机）。编号对齐权威件冻结协议。
enum {
    PUSH_ST_READY = 0x10,    ///< 会话已接受 / session accepted
    PUSH_ST_RESUME = 0x11,   ///< 断点：body=received(u32 LE)+next_seq(u16 LE) / resume point
    PUSH_ST_DONE = 0x12,     ///< 校验通过 / verified
    PUSH_ST_ERR = 0x13,      ///< 拒绝，body 是错误码 / rejected
    PUSH_ST_EVT_READ = 0x14, ///< 用户标记已读（编号占位）/ marked read
    PUSH_ST_BATT = 0x15,     ///< 电量（编号占位）
    PUSH_ST_PROGRESS = 0x16, ///< 接收进度 / progress
    /* 0x17 AI_QUERY / 0x18 CAPS / 0x19 TTS：属其他模块，本核不定义 */
};

/// 正文类型——认全五种，落库 type 为后续格式扩展留口。
enum {
    PUSH_TYPE_TEXT = 0,     ///< 纯文本
    PUSH_TYPE_MARKDOWN = 1, ///< Markdown（当前按纯文本收，转换在手机侧）
    PUSH_TYPE_HTML = 2,     ///< HTML（同上）
    PUSH_TYPE_URL = 3,      ///< URL：协议认，业务回 ERR（URL 抓取当前不做）
    PUSH_TYPE_WORD = 4,     ///< 单词卡：按普通文本处理，落库 type=4
};

/// 错误码（STATUS ERR 的 body）。编号对齐权威件。
enum {
    PUSH_ERR_NONE = 0,
    PUSH_ERR_NOT_ARMED = 1,
    PUSH_ERR_BAD_FRAME = 2, ///< 魔数/版本/长度/seq/未知 type 非法
    PUSH_ERR_TOO_LARGE = 3, ///< total_len 超 512KB
    PUSH_ERR_CHECKSUM = 4,  ///< CRC32 或长度不符（收全判据失败）
    PUSH_ERR_NO_CARD = 5,
    PUSH_ERR_LOW_BATTERY = 6,
    PUSH_ERR_BUSY = 7,
    PUSH_ERR_FETCH = 8,     ///< URL 类型当前不支持（协议认、业务拒）
    PUSH_ERR_NO_DICT = 9,
};

/// BEGIN 帧（83 字节，全部小端）—— 冻结布局，与 APK 参考实现逐字节一致。
typedef struct {
    uint32_t total_len;  ///< 正文字节数
    uint32_t crc32;      ///< 正文 CRC32
    uint32_t session_id; ///< 会话标识，断点续传对账锚
    uint16_t token;      ///< 广播名后缀
    uint8_t type;        ///< PUSH_TYPE_*（0..4）
    char title[PUSH_TITLE_MAX]; ///< UTF-8，截断保 0
} push_begin_t;

#define PUSH_BEGIN_LEN 83u  ///< BEGIN 帧字节数
#define PUSH_DATA_HDR_LEN 3u ///< DATA 帧头：seq(2)+len(1)
#define PUSH_STATUS_MAX 16u  ///< STATUS 帧上限
#define PUSH_ST_RESUME_BODY 6u ///< RESUME 回执 body：received(4)+next_seq(2)

/* ---- CRC32 ---- */

/// IEEE 反射式 CRC32（poly 0xEDB88320），与 zlib 同款。
uint32_t push_crc32(const void *data, size_t len);

/// 流式 CRC32：crc 传上一段返回值（首段传 0）。链式等价于对拼接后一次运算。
uint32_t push_crc32_continue(uint32_t crc, const void *data, size_t len);

/* ---- BEGIN 编解码 / BEGIN codec ---- */

size_t push_begin_encode(const push_begin_t *begin, uint8_t *out, size_t cap);
bool push_begin_decode(const uint8_t *in, size_t len, push_begin_t *begin);

/* ---- DATA / CTRL 帧头 ---- */

bool push_data_decode(const uint8_t *in, size_t len, uint16_t *seq,
                      const uint8_t **payload, size_t *payload_len);
bool push_ctrl_header_decode(const uint8_t *in, size_t len, uint8_t *op);

/* ---- STATUS 编解码 / STATUS codec ---- */

size_t push_status_encode(uint8_t opcode, const uint8_t *body, size_t body_len,
                          uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
