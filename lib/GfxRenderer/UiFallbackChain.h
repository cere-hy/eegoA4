#pragma once
// S3c+++ — UI 字形兜底的字符串级 fallback 链纯决策逻辑。
//
// device (GfxRenderer::resolveTextFontId) 与 host 测试编译同一份头，确保
// 「优先级（SD > 内置）+ 防环 + 链末兜底」不会像 advance 表那样 drift
// 成 host 全绿 / 真机崩。纯整数 + 模板逻辑，无分配、无平台头文件。
//
// 链语义（由 SdCardFontSystem 的 setFallbackFont 串接）：
//   ① ubuntu 点阵（主） → ② SD 用户字体（若装） → ③ 内置 builtin.ttf variant → ④ 占位符
// nextFallback(cur, out) 从 fallbackMap[cur] 取下一级；0 / 自环 = 链末。
#include <cstdint>

namespace uifallbackchain {

// 链跳转上限：主 → SD → 内置 → 链末，4 跳足够并作为防环护栏。
constexpr uint32_t kMaxFallbackHops = 4;

// 是否需要为该码点考虑重定向：非 ASCII 且主字体缺字。
// ASCII 恒被所有 UI 字体覆盖，留在主字体快速路径（英文书名零开销、零回归）。
inline bool needsFallback(uint32_t cp, bool primaryHas) {
  return cp >= 0x80 && !primaryHas;
}

// 沿链找第一个覆盖 cp 的 fallback；若链上各级 cmap 都不覆盖该字，
// 交给链上最后一级有效 fallback（内置 variant，其 PIF1 二级 + tier④
// 占位符能兜住任意码点）。这样即使 builtin.ttf cmap 缺某个生僻字，
// 整串仍切到内置 variant 并由字级 handler 处理，绝不落回方块主字体。
//
// nextFallback(cur, out) : 返回 true 且写 out=下一级 id；false/0/自环 = 链末。
// covers(id, cp)         : 该级字体 hasCodepoint。
// 返回选中的 fontId；primaryId 表示不重定向。
template <typename NextFn, typename CoverFn>
int resolveChain(int primaryId, uint32_t cp, NextFn nextFallback, CoverFn covers) {
  int cur = primaryId;
  int terminal = primaryId;  // 链末有效 fallback（无条件兜底）
  for (uint32_t hop = 0; hop < kMaxFallbackHops; ++hop) {
    int fb = 0;
    if (!nextFallback(cur, fb) || fb == 0 || fb == cur) break;
    terminal = fb;
    if (covers(fb, cp)) return fb;
    cur = fb;
  }
  return terminal;
}

}  // namespace uifallbackchain
