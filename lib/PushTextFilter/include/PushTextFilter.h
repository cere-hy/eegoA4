/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：文本推送「轻量 Markdown 兜底清洗」——B6.5 S3。
 * 当前 APK 只认纯文本，用户粘贴 md 时原样发来带 # ** []() 的源码。
 * 本库按内容探测（不看 type），对收到的正文做六步清洗，作为当前阶段兜底：
 *   ① 控制字符过滤 ② UTF-8 边界修复 ③ 换行符统一
 *   ④ 空白折叠（跳过代码块） ⑤ Markdown 剥除（仅代码块外） ⑥ 图片占位符剔除
 *
 * 硬约束：
 *   - ``` 代码块内的缩进/行尾空格/连续空格原样保留（不碰任何空白与标记）；
 *   - 有序列表数字保留；诗歌/歌词短行逐行保留、不合并；
 *   - 幂等：无标记无多余空白无控制符的纯文本，走完六步逐字不变。
 *
 * 纯 C11，不依赖 ESP-IDF / Arduino / 文件系统；输入输出均为 UTF-8 字节串。
 * 输入整篇一次性处理（实际量级几百~五千字），输出为 malloc 的新缓冲区。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 清洗统计（供日志/验收）。
typedef struct {
    size_t removed_controls;   ///< ① 移除的控制符 + 零宽字符个数
    size_t dropped_utf8_tail;  ///< ② 末尾丢弃的截断残字节数
    size_t collapsed_runs;     ///< ③ 压缩的连续空行处数
    size_t images_removed;     ///< ⑥ 剔除的图片占位符条数
    size_t lines_removed;      ///< ⑥ 整行删除的纯图片行数
    size_t code_fences;        ///< 识别到的 ``` 围栏行数
    bool unclosed_fence;       ///< 存在未闭合 ```
} push_filter_stats_t;

/**
 * 六步清洗。成功返回输出字节数（不含结尾 0），*out 指向 malloc 缓冲区（调用者
 * 用 push_text_filter_free 释放），始终 NUL 结尾。失败（OOM/非法参数）返回
 * SIZE_MAX 且 *out = NULL。stats 可为 NULL。
 */
size_t push_text_filter(const char* input, size_t len, char** out, push_filter_stats_t* stats);

/// 释放 push_text_filter 产出的缓冲区。
void push_text_filter_free(char* p);

#ifdef __cplusplus
}
#endif
