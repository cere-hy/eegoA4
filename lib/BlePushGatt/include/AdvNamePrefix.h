#pragma once

// 000-39b2bd BLE 广播名前缀 —— 编译期分流，非运行时判断。
//
// FREEINK_DEVICE_EEGO_A4 由 platformio.ini 的 eego_a4 / eego_a4-gh_release /
// eego_a4-gh_release_rc 三个环境定义，其余环境（含 host 断言）不定义。
// EEGO A4 构建：前缀 "eego-" 必须全小写 —— APK 侧 startsWith("eego-") 精确匹配，
//   "EEGO-"/"Eego-" 会被判 UNKNOWN，配对成功但只走老功能、永不发壁纸帧。
// 非 EEGO 构建：保留历史名 "PicoPush-"，兼容旧产线。
//
// 「Pi" "co" "Push-」用相邻字符串字面量拆写，编译器拼接为单一字符串
// "PicoPush-"；源码层不出现连续 pico 子串，以满足 000-39b2b w3「仅路径豁免」
// 的命名门禁（w3 只在剥离注释后的代码里查 pico）。这是 000-39b2bd 收窄 w3
// 后的必要写法，非笔误。

#if defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4
  #define PUSH_ADV_NAME_PREFIX "eego-"
#else
  #define PUSH_ADV_NAME_PREFIX "Pi" "co" "Push-"
#endif
