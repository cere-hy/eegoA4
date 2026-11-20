# GIT_BASELINE — eegoA4-port 重建基线账本

> 每批 commit 同步更新。当前基线 commit + sha256 + 各产物指纹 + 批次状态。
> 目标：对齐 s11（b09556e5）行为与读数，非字节一致。

## 底座三件套（白皮书锚点，已核对）

| 组件 | 引用/锚点 | 实测 | 证据 |
|---|---|---|---|
| crosspoint-reader (app 层) | 上游 HEAD | `490813ff4541e74e654f54f66ece9efc908361a0` (2026-11-16) | submodule status 引用 freeink-sdk@`3f3e4c1` |
| freeink-sdk (SDK overlay) | `3f3e4c1`（白皮书 §1） | `3f3e4c1126e2cb9a278c2c1076fb4a3334ffdc30`，工作区 **1052 文件** | ls-tree 与工作区一致，与 §1 指纹判据数吻合 |
| pico-plus (推文 sink 权威件，B6.5) | `e2544e3`（白皮书三端互证基准） | `e2544e3e934afea281e130a246be4602a75419e6` | push_proto.h 常量 file:line 逐条对上白皮书 |

## 平台/工具链

- PlatformIO Core 6.2.0；平台 `espressif32@55.03.311`（pioarduino）= ESP-IDF 5.5.5 / Arduino core 3.3.11
- 编译走本地 `file:///root/.platformio/platforms/espressif32`（local.ini 覆盖，绕开 URL 重装卡下载）
- `-j 4` 防 OOM；`SOURCE_DATE_EPOCH=1794813449`（2026-11-16 07:17:29 UTC，取自 app HEAD 提交时间）

## 构建产物指纹（B1 首编，冷编1）

| 产物 | sha256 | 尺寸 |
|---|---|---|
| firmware.bin (app) | `db72df145572bcde4b727c9aa16675eef4a032ca0cba2d496e5868607af57d74` | 5,437,200 B |
| firmware.factory.bin | 见各批回执（由 archive_segments.py 生成，可复现） | 5,502,736 B |
| bootloader.bin | `15f88ae11793eddc5b7e1f770fba54b10a6f59809c14d7c51cfbf0db8d2fa3ca` | 18,720 B |
| partitions.bin | 随编译 | 3,072 B |
| boot_app0.bin (otadata) | `f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0` | 8,192 B |

数账：factory − app = 5,502,736 − 5,437,200 = **65,536** ✓（恒 0x10000）

## 批次状态

| 批次 | 变量 | 状态 | 回执 |
|---|---|---|---|
| B0 | 计划 | 完成（v1.6 权威白皮书校准） | `/workspace/REBUILD_MASTER_PLAN.md` |
| B1 | 底座构建 | **完成**：双冷编同 sha256 `db72df14...`，双源 factory `2c72f26e...`，selfcheck PASS；commit `993c4e63` | `docs/rebuild/B1_底座构建回执.md` |
| B2 | S8 裁剪 | **完成**：30 删/5 保留/17 引用清理 + i18n 23 键；strings 四计数全 0；双冷编 app/factory 同 sha（A 级闭合）；commit `704912a1`（+ i18n 补行 `8e34ed38`） | `docs/rebuild/B2_裁剪回执.md` + `B2_路径普查报告.md` |
| B3 | BLE 连接层 | **完成 + 真机通过 + 点核闭环（v1.7）**：peripheral GATT 推文服务（PicoPush 全局广播 + WWR 三特征 + MTU 247）+ 被动蓝牙状态栏图标 + 一射频一主人排他闸；双冷编 app/factory 同 sha（A 级闭合）；commit `4ad75499`。真机（11-17 哨位）：翻书流畅/蓝牙好/图标在位；`PicoPush-424C`、MTU=247、5f13 订阅成功；`reason=531`=APK 双按钮互斥通道切换（公主补充精化，非缺陷）。点核增量：协议行为档案立档（§7.5）、B6.5 收 RESUME 必需 + 通道切换断流防冲突、token 口径中性化。详见回执 §七/§八 | `B3_BLE连接层回执.md` |
| B4a | 0x1A 编码器 | **完成（纯 host 逻辑核批）**：白皮书 §2.1 权威布局 0x1A scene 编码器（lib/PushSceneCodec，241B 定长/零填充/UTF-8 边界/首帧元数据 4+wl）；host 五向量断言 248 checks/0 red（28B→1帧/payload36、300B→2、1024B→5、空词拒、199B 截 63）；B3 实现 commit `4ad75499` revert 临时 worktree 实跑零冲突。commit `94167d18`（5 文件/+467）。本批不出固件件。MAC 原文勘误 v1.7.1 + 副本非原件归档随本批入档 | `B4a_0x1A编码器回执.md` |
| B4′ | 0x1A 接线（B4 固件半） | **完成 + A 级闭合**：点核 v1.8 裁定接线是 B4 本体变量不可推 B11，立本补批。ScenePush 桥接（查词→codec→STATUS 5f13 notify）+ CCCD 订阅闸 + 阅读页 onEnter/onExit 注册注销回调不停广播 + 互不等待双路；LDF 自动链入（ELF 符号实证非空壳）。commit `c2ef4338`（8 文件/+223）。双冷编 app `f39b5e5a…`/factory `edda4500…` 同 sha、epoch `1794813449`、delta 65536。交付件 `crosspoint_eegoA4_b4-20261117-c2ef4338_factory.bin`。真机仅剩「APK 收 0x1A 弹卡」归 B11 | `B4prime_0x1A接线回执.md` |
| B5 | 0x07 单帧接收 | **完成 + A 级闭合（v1.9 放行）**：白皮书 §2.2 权威偏移硬解（带头/裸帧双接）+ 扫可打印段交叉校验（不一致 hex 留证告警）+ `dn=170` 拒收；双槽 + per-slot seqlock 原子代际号（BLE 写/main-loop 读不撕裂，host pthread 2 万帧并发零撕裂、代际单调）；CTRL 0x07 专用 `AiResultHandler` hook（与 B6.5 推文 CtrlHandler 分离）+ 全局常驻 + 主循环 pump；上屏追加重排→跳末页、失败回滚；241B hex 三连留证。host 36 checks/0 red。commit `6820763e`（8 文件）。双冷编 app `ef368c53…`/factory `e138e41e…` 同 sha、epoch `1794813449`、delta 65536。交付件 `crosspoint_eegoA4_b5-20261117-6820763e_factory.bin`。真机可见项（追加/跳末页/回滚）以 s11 oracle 归 B11 | `B5_0x07接收回执.md` |
| B11-AI | AI 解读链路（0x1A→APK→0x07/0x08→全屏） | **完成 + 真机验收通过（v1.28, 2026-11-18）**：0x1A 分帧发送（独立 mbuf notify 修共享 buffer 覆盖）→ APK 重组/AI 调用 → 0x07 单帧 / 0x08 分帧回传（修 169B 截断）→ 设备双槽 seqlock + `ai_segasm` 重组 → 全屏页（16pt + 全程 Gray3）；0x18 CAPS 补齐 APK 切换依据。真机三项全过（planB 无 `[cut]`、单词/整段全中、16pt 易读、Gray3 无闪屏感）。commit `58b41ca8`（**正式功能基线**，替代 `9f147876`） | `AI解读链路_完整验收记录.md`、`字号灰度小批_施工回执.md` |
| B6.5-S0 | 取消重查代际修复 | **真机验收通过 + 闭环入账（v1.30）**：全局 sticky 作废水位（`currentGeneration/invalidateUpTo/discardFloor`），cancel 冻结水位+清半重组 0x08，re-query clamp baseGen≥floor，poll 双门槛，迟到 SegError 丢弃；水位存全局 sink 跨 Activity 重建。仅改 AiResultSink.h/.cpp + ClipSelectionActivity.cpp。commit `2b1b060a`。A 级双冷编闭合、host 248+36+28+23 全绿。件 `crosspoint_eegoA4_s0-b65-20261118-2b1b060a_factory.bin`（factory `f07d990c…`）。真机：取消后立刻重查弹新词、未串味。构建非确定性已字节级诊断（管道 source 丢 epoch，非机制退化，三次无管道冷编一致、交付件不变），见诊断回执。 | `S0_代际漏洞修复_施工回执.md`、`S0_MANIFEST.json`、`S0构建非确定性_诊断回执.md` |
| B6.5-S1 | 文本推送纯核（完整能力版） | **完成并采信（v1.31/v1.32）**：撤销激进裁剪，按"设备主输入通道"长期定位重做。commit `a2728d27`（裁剪版 `502ce522` 作废）。`lib/PushTextCore/` 移植 push_proto+push_session+push_sink_core，完整保留：七态机(含 cooldown)、断点续传(RESUME_QUERY/adopt/resume/has_part + ST_RESUME(0x11) 六字节 + 同 session BEGIN 续传 + 外部 .part CRC 钩子)、事务 commit 四拍能力、BEGIN.type 五种全解析(URL→ERR FETCH、未知→ERR BAD_FRAME、WORD 按文本落库 type)、512KB 上限、CRC 512B 分块。**仍不搬**：AI 帧 0x06-0x09(独立 codec、0x07/0x08 判据不动)、TTS/伴读/URL 抓取。host 210 checks/0 red（ASan/UBSan）。四问(内存<2KB/.part 单文件不堆积/转换放手机侧/工期6.5~7.5天)见回执。 | `S1_文本推送纯核_施工回执.md` |
| B6.5-S2 | GATT 挂接 + STATUS 握手 | **完成 + A 级双冷编出件 + 停等真机验收（v1.32）**：commit `fa8f2e04`。新建 `lib/PushTextSink/`(h/cpp/library.json)+main.cpp 两处，把 PushTextCore 挂到 BlePushGatt 现成 `setCtrlHandler`(0x01-0x05)/`setDataHandler`(DATA) hook。**B3 源码/PushTextCore/协议文本/0x07-0x08 判据/APK 全零改**。BEGIN→`driveConnected()` 状态机驱到 CONNECTED→`0x10 READY` 经独立 mbuf `sendStatusFrame` 即时发（APK 唯一等待信号）；RESUME_QUERY→6B RESUME；DATA 内存计数+PROGRESS 400ms 节流；END→DONE/ERR；STATUS take 语义防陈旧帧。core 仅 host task 访问、无锁、不碰文件系统、不消费 commit/open。LDF 链接 ELF nm + 固件 strings 实证非空壳。分发边界四路互斥复验通过(0x07 AI/0x08 seg/0x01-0x05 推流/DATA 推流)。host 210/0 绿。**A 级双冷编**：app `5dda0fed…`、factory `8068874d…` 两次逐字节一致、epoch 1794813449、四段 selfcheck PASS、前三段与 S0 逐字节同仅 app 变。件 `crosspoint_eegoA4_s2-b65-20261118-8068874d_factory.bin`。**格式分工定案**：APK 数据处理中心(本轮不动、时序非能力边界)、设备显示终端、md 轻量清洗 S3 实现；S3 工期重估 4~5.5 天、总 7.5~9.5 天。**S2 真机验收 ①–⑤ 通过**（BEGIN→READY→DATA→END→DONE 无超时、推流后监听恢复、AI 查词走 0x08 正常、屏幕无变化），放行 S3。 | `S2_GATT握手_施工回执.md`、`S2_MANIFEST.json` |
| B6.5-S3 | 落盘校验 + 六步清洗 + 文档 + 主页 PUSH 按钮 | **完成 + A 级双冷编出件 + 停等真机验收（v1.33）**：commit `66c48f84`，从 S2 `fa8f2e04` 继续（S2 真机 ①–⑤ 通过）。①落盘：DATA 按 `PUSH_ACT_APPEND` 流式 append `/.readpico/push.part`(241B/帧、512B CRC 分块、不整块进内存)；END 回读 .part 全量 CRC32+长度双判据、注入 `set_verified_crc` 后喂 core，通过→事务四拍 `PUSH.txt→.bak→tmp→PUSH.txt→清.bak`(失败回滚+留 .part)，失败→ERR(CHECKSUM)+留 .part 供续传；RESUME_QUERY 从 .part 实际长度 adopt 带真实 offset。②新 `lib/PushTextFilter/` 纯 C11 六步幂等清洗(控制符/UTF8 尾/换行/空白折叠跳过代码块/Markdown 剥除/图片剔除，按内容探测不看 type，代码块缩进·有序数字·诗歌短行红线保留、未闭合围栏整篇当代码块)，**清洗在落盘阶段完成、push.txt 即干净文本、打开不清洗不分叉**，host 96/0 绿。③书名硬编码 PUSH(文件名 stem、省 sidecar)、设备侧一本整篇替换、type/title 仅日志；复用 `ReaderActivity→EpubReaderActivity`(TXT→TxtToHtml) Gray3+字号跟随；提交后 `clearCache` 保证第 1 页。④主页第5按钮 PUSH(复用 drawButtonMenu 同款样式+goToReader 链路、无内容点击弹 `No push content` 常显不隐藏、不主动刷新)。**渲染优化令核实**：eegoA4 TXT 首开同步排 8 页(15KB≈5–10 页、翻页不需现场排版)、首屏 HALF 整屏刷一次、无整页预绘、页码确定 `"1/N"`、重复打开读缓存——三项优化只做"清洗移到落盘"(已完成)，后两项现状已具备、**未新增渲染机制**。**残留**：TXT 首次打开弹一次元数据 Indexing(毫秒级、清缓存所致、非排版提示)。B3/协议/0x07-0x08 判据/APK 零改、不做自动打开。**A 级双冷编**：app `55cfba0d…`、factory `eebcaa12…` 逐字节一致、epoch 1794813449、四段 selfcheck PASS、前三段与 S0 逐字节同仅 app 变。件 `crosspoint_eegoA4_s3-b65-20261119-eebcaa12_factory.bin`。host 210/0+96/0 绿、分发边界复验不回归。**真机验收暴露 P0 崩溃(PC 0x4224ed47)/P1 开书 13.7s/无自动打开/无封面，已由 S3b `82a26bef` 修复，S3 转前序件。** | `S3_落盘清洗文档按钮_施工回执.md`、`S3_MANIFEST.json` |
| B6.5-S3b | 收尾：P0 崩溃修复 + P1 字体预加载 + P3 自动打开 + P4 封面 | **完成 + A 级双冷编出件 + 停等真机验收（v1.34）**：commit `a58a30ac`，从 S3 `66c48f84` 继续。**P0**——addr2line 定位崩溃 `0x4224ed47`=`Serialization.h:65 readString`→`string::resize` 垃圾长度 abort(根因 S3 commit `clearCache` 删当前会话读的缓存树,非 MaxAlloc)。两道防线：①commit 写 sticky 标记 `/.crosspoint/push_dirty`(缓存目录外)替代 clearCache，reader `loadBook` 消耗时选择性清缓存(删 book.bin/progress/sections/html/css、保留 thumb_400.bmp)+删标记；②`Serialization.h readString(HalFile&)` clamp len≤64KB 且≤available→clear，保护全部 9 处调用点防 SD 损坏。Activity 替换时序核实无竞态(旧会话析构先于清缓存)。**P1 乙方案**——`EpubReaderActivity::prewarmPushGlyphs()` 读全文提唯一 UTF-8 codepoint→`fcm->prewarmCache(accumulate=true)`→`TtfEpdFont::addCoverage` 一次性 fault 字形进 PSRAM；实测 5000 字≈720 唯一字、≈216KB、缓存上限 1MB/4096 不 flush；非 PUSH 书不碰；缺字>20% 用 `hasCodepoint` 检测并复用 ASCII `STR_FONT_DAMAGED` 提示(**自动换字体未实现:无逐字 family fallback API,待裁定**)。**P3 自动打开前移**——`PushTextSink` commit 成功置 `std::atomic<bool>`，main loop `takePushCompleted()` 消费在 loop 任务决策；新增 `ActivityManager::isForegroundPushReader()`(friend 访问 bookPath)；三策略：读 PUSH→goToReader 新建(第1页+HALF 整屏过渡)/读其他书→不切走不弹窗/主页其他→自动打开；与 BLE 监听恢复不冲突。**P4 封面**——新 `lib/PushThumb/`(纯内存零依赖)内嵌 8×8 ASCII 点阵→240×400 1-bit BMP(同 `thumb_400.bmp` 格式路径、color0黑/color1白)，白底大字 PUSH+两行小字(推送时间 YYYY-MM-DD HH:MM + 字节数)，commit 时 remove 旧再写、失败静默降级；灰度查证:现有书库缩略图均 1-bit、显示走主题，封面 1-bit 与之一致。**ASCII 自查**新增可见串全 ASCII、中文仅正文。**A 级双冷编**：app `fbcb2611…`、factory `82a26bef…` 逐字节一致、epoch 1794813449、四段 selfcheck PASS、前三段与 S0 逐字节同仅 app 变。件 `crosspoint_eegoA4_s3b-b65-20261119-82a26bef_factory.bin`。host 210/0+96/0+16/0 绿、分发边界 0x07/0x08 提前 return 保留、ELF nm+strings 实证四特性非空壳。**三项待裁定**：①验收①"翻页读旧内容"与自动打开策略①"立即重开"冲突、②缺字自动换字体是否下批、③封面 Gray3 是否需灰阶 BMP。**停 S3b 等真机验收 ①–⑬ + 裁定，未开 S4。** | `S3b_崩溃修复字体预加载自动打开封面_施工回执.md`、`S3b_MANIFEST.json` |
| B6.5-S3c | 字体方案重做：内嵌四层回退 + 封面条纹修复 | **完成 + A 级双冷编出件 + 停等真机验收（v1.35）**：commit `3b414fb0`，从 S3b `a58a30ac` 继续。S3b 真机暴露乙方案 prewarm 34-37s 更慢、封面横向条纹。**字体**——撤销 prewarm 全文预载；PUSH 改专用内嵌黑体（不跟随全局字体、字号跟随用户设置），四层按字回退：①`builtin.ttf` 388KB resident via `addResidentSource`+FreeType（覆盖常用中文 84-92% 主力）、②新 `lib/PushEmbeddedFont` PIF1 位图（5637+13 标点高频 GB2312 补丁 8-16%）、③SD LXGW 生僻字（`rasterizedBitmap`）、④占位符+ASCII miss 日志；`TtfEpdFont` 新增 `SecondaryHandler`+`injectGlyph`+`rasterizedBitmap` 二级注入（默认 nullptr 其他字体零影响），三层字形统一进同 face arena，`vectorBitmapHandler` 不改；PIF1 纯逻辑 `Pif1Codec`（注入 tinfl/分配，host 可测）+ 2×2 覆盖率插值移植 Pico 三公式。`board_build.embed_files` 三 A4 环境内嵌两资源；`scripts/gen_push_punct_bin.py` 用 OFL Noto Sans CJK SC 补《》〈〉【】等 13 标点（5637→5650、188,496B、177 块解压全过），四类语料回退率 0%。Section 排版/渲染 fontId 统一走 `effectiveReaderFontId()`（PUSH→专用 id、其余→全局），section 缓存按 fontId 自然隔离。**封面**——canvas stride 30B→32B BMP 对齐、padding 填白、setBlack y*32+x/8，消除错位 2B+padding 全黑条纹。**A 级双冷编**：app `c2659e39…`、factory `9249c61f…` 逐字节一致（cmp rc=0）、epoch 1794813449、`-j3`（cgroup 8G，32 并行 OOM）、四段 selfcheck PASS、factory 6,066,272B。件 `crosspoint_eegoA4_s3c-b65-20261119-c2659e39_factory.bin`。host 210/0+96/0+18/0+PIF1 22/0 绿；PIF1 位图层 800 字 0.3ms 非瓶颈。**停 S3c 等真机验收 ①-⑰（2 秒目标以验收①为准，沙箱无法证伪），未开 S4。** 远期登记：内嵌字库全局化、AI 释义卡片字形路径（S4 后）本批不动。 | `S3c_内嵌字体回退链与封面修复_施工回执.md`、`S3c_MANIFEST.json` |
| B6.5-S3c-P0fix | P0 栈溢出崩溃修复（tinfl decomp + cover/packed 移出栈） | **完成 + A 级双冷编出件 + 停等真机验收（v1.35.1）**：commit `761059dd`，从 S3c `3b414fb0` 继续。S3c 真机崩溃：推 4713B→自动打开→section build 首次取字形→`Stack canary watchpoint triggered (ActivityManager)`，连崩 2 次，ELF SHA `cfb60d59c` 精确匹配。**定位**——addr2line 点名 5 帧全在 tinfl 链：`crosspoint_tinfl_decompress`(miniz.c:2587)←`tinfl_decompress_mem_to_mem`(:2974)←`inflateBlock`(PushEmbeddedFont.cpp:36)←`decodeBlock`(Pif1Codec.h:80)←`Pif1Reader::get`(:99)，外链 `getTextAdvanceX→getGlyph→missThunk`，确认崩在首次取字形非首次排版；栈跨度 18,576B > 16,384B 任务栈，堆余 83,728B 健康（栈溢出非堆不足）。**根因**——假设 A+B 成立、C 不成立：A `mem_to_mem` 栈上声明 `tinfl_decompressor`(host 实测 8,376B，与 backtrace 8,432B 帧吻合；本路径 NON_WRAPPING 输出缓冲即字典，非 32KB 字典问题)；B `secondary()` 栈上 `cover[5184]`+`packed[1308]`(第二处 6,688B 跳变)；C 回退链顺序 if/else 非递归。**修复**——`inflateBlock` 改文件级 `static tinfl_decompressor g_inflateState`+`tinfl_init` 直接调 `tinfl_decompress`（单任务复用）；cover/packed 改文件静态；渲染栈 16KB→24KB(vector 板)；渲染后 `uxTaskGetStackHighWaterMark` 打 `[ACT] render stack HWM`；canary 保持开。512KB 字体缓存确认 `PsramVector`/`fiFontMalloc` 走 PSRAM。**host 盲区**——既有测试只编译 Pif1Codec.h 纯逻辑+zlib 堆、不编译 device glue、无 16KB 栈约束→结构性盲区；新增 `stack_budget_host_test.cpp` 断言 decomp 大小/源码静态化/栈配置。全量 host：210/0+96/0+18/0+PIF1 22/0+**stack budget 10/0**。**A 级双冷编**：app `4d0e2c90…`、factory `bc414167…` 逐字节一致（app/factory/MANIFEST/ELF 4 文件 cmp rc=0）、epoch 1794813449、`-j3`、四段 selfcheck PASS、factory 6,066,176B（app 6,000,640B，较前减 96B）。件 `crosspoint_eegoA4_s3c-p0fix-20261119-761059dd_factory.bin`。**停等真机验收 P0 ①②③④ + HWM 余量；未开 S4。** | `S3c_P0栈溢出_诊断与修复回执.md`、`S3c_P0栈溢出_MANIFEST.json` |
| B6.5-S3c-P1fix | P1 回退字体尺寸失配修复（em ppem -> asc-desc px） | **完成 + A 级双冷编出件 + 停等真机验收（v1.35.2）**：commit `5b4961f0`，从 S3c-P0fix `761059dd` 继续。P0fix 真机采信（栈余 15KB/24576、封面条纹修、自动打开正常）但暴露 P1：GB2312 位图回退字与矢量字大小/基线失配 1.3-1.5 倍，中英混排高低起伏。**根因证实（用户判断正确：px 定义混用）**——`PushEmbeddedFont.cpp:61` 传入 `px=getSizeInPixels()=pt×150/72` 是 em 高度 ppem；但 ui-hanzi.bin record 由 `stbtt_ScaleForPixelHeight(24)=24/(asc-desc)` 生成，base-24 是 **asc-desc 高度**制式（全角 CJK adv=17px）。builtin.ttf 与建 bin 源字体 hhea 同 asc=1160/desc=-288/upem=1000 → 失配比 `(asc-desc)/upem=1448/1000=1.448`，正落在 1.3-1.5 倍。**修复**——`Pif1Codec.h` 新增共享 `kPif1AscDescPerEm=1448/1000` + `pif1PxFromEmPx()`（device/host 共用防漂移），`PushEmbeddedFont.cpp` px 改走该函数（×1.448）；**Pico 三公式一字未动、单一全局常数无逐字号调参**。**host 实测对照**（stbtt 复刻两路径，pt=14：ppem 29.17→px 42）：矢量 CJK h≈27.8/adv≈29/top≈24.8；修复前位图 h 19-21(k≈0.70,反向 1.43)/adv 21/top 低 6-9px；修复后 h 27-30(**k 0.973-1.081**)/adv 30(±1)/top±2px。新增 `glyph_size_host_test.cpp`（断言 k∈[0.88,1.12]/adv±2/基线±3 + 旧路径 k<0.85 反证）+ run 脚本。全量 host 210/0+96/0+18/0+PIF1 22/0+stack 10/0+**glyph 16/0**。附带 8s 排版评估：量宽经 `getGlyph→faultGlyph` 含 2×FT load+rasterize，已有 `metricsGlyph26_6`/`SdCardFont::advanceTableLookup` advance-only 先例，改后 `parse and build` 估 8s→2-3s 量级（未实测），登记下一批候选本批不改。**停等真机验收 P1 ①-⑥（混排高度/基线/字宽/生僻字/速度不劣化/崩溃不回归）+ S3c 原项继续有效，未开 S4。** | `S3c_P1字体尺寸失配_诊断与修复回执.md`、`S3c_P1字体尺寸失配_MANIFEST.json` |
| B6.5-S3c-merge | 合并令：位图回退字发糊修复（P1）+ 封面从零重做（P2）一次出件 | **完成 + A 级双冷编出件 + 停等真机验收（v1.35.3）**：commit `8bcb5b7e`，从 P1fix `5b4961f0` 继续。 diagnostics（动手前必答，全实测）：观 U+89C2/倍 U+500D/摇 U+6447 均不在 builtin.ttf(1260字)、均在 PIF1 bin(5650字)→确走位图层；国/中/文 走矢量。糊的量化——三字源 record 17×16×17/16×17×17/16×16×17，pt14→px42；2-bit 量化后中灰(档1淡灰+档2)占比 观28.6/倍22.7/摇29.1%，矢量国20.6/中12.4/文9.9%，糊集中在档1淡灰（2×2 双线性把 1-bit 硬边缘摊灰）。**修复层次一即达标**：`Pif1Codec.h::coverage()` 加边缘检测（2×2 邻域 max−mn>128 in 0..255 域=笔画边界）取最近邻/主方向、不做平均；四邻域一致保留精确双线性；中灰 23-29%→**0%**、笔画清晰无断笔。**层次二(32px bin)/层次三(对比度曲线)未启用**（层次一已达标，避免动 flash 余量与第二套参数）。矢量路径零改动（本次 5 文件无 TtfEpdFont/FtFont）、Pico 三公式未动、单一全局规则无逐字号调参；尺寸/基线/advance 不回归（glyph 16/0 k≈1.0）。**P2 封面从零重写** `PushThumb.cpp`：大字 PUSH 矢量笔画按 48px 目标 2×2 超采样栅格化（禁整数倍放大）、时间先算宽自适应降档至放下（scale1 127px）+拆行兜底、字节数固定 `%zu bytes`、垂直重心 195.5≈200。中部重复 PUSH 黑块定性为**主题叠加层**（BaseTheme::drawRecentBookCover 第651-681行书名框，title="PUSH" 来自文件名），**非封面 BMP**（导出 BMP 仅 3 段墨迹无此块），按令说明处理方式、本批不动通用主题代码。**导出 thumb_400.bmp 逐像素**：240×400/1-bit/stride32/白底95.75%/padding 全白无条纹/3 段墨迹包围盒 x≥8 且右≤232 且居中120（PUSH x23-216 y100-147、时间 x56-182 y200-206、4458 bytes x40-197 y252-265）/无裁切。全量 host 210/0+96/0+248/0+36/0+28/0+23/0+24/0+PIF1 24/0+stack 10/0+glyph 16/0+**PushThumb 34/0**。**A 级双冷编** app `d5526c61…`/factory `52f95f76…`/MANIFEST `cd766f98…`/ELF `2a7b1c10…` 四文件逐字节 cmp rc=0、epoch 1794813449、`-j3`、四段 selfcheck PASS、factory 6,067,824B（app 6,002,288B，较 P1fix +1,568B）。件 `crosspoint_eegoA4_s3c-merge-20261119-8bcb5b7e_factory.bin`。**停等真机验收 ①-⑮；未开 S4；advance-only 排版优化登记下一批。** | `S3c合并令_位图发糊修复与封面重做_施工回执.md`、`S3c合并令_MANIFEST.json` |
| B6.5-S3c-final | 收尾批：封面时间行下移（甲方案）+ advance-only 排版量宽 | **完成 + A 级双冷编出件 + 停等真机验收（b65-s3c-final）**：commit `cfa4ef5f`，从 S3c-merge `8bcb5b7e` 继续。**封面**：动手前必答量化（BaseTheme.cpp + ubuntu_12/10 advanceY 29/24 核验）——书名框 y177~222、Continue 按钮 y361~391，旧时间行 y200~206 正在书框内被遮。甲方案仅改 `PushThumb.cpp` 时间行 y200→**y290~296**（拆行兜底 y286/300），大字 PUSH y100~147/字节 y252~265/字号/宽度/fitScale8/居中/stride/白底全不动，不改 BaseTheme。导出 thumb_400.bmp 逐像素：3 段墨迹 PUSH x23-216 y100-147、字节 x40-197 y252-265、时间 x56-182 y290-296，均居中(cx118.5~119.5)/x≥8/右≤232/无裁切；时间距字节 25px、在书框底下 222 下、按钮顶 361 上、≤320。host PushThumb 40/0。**advance-only**：根因 `getTextAdvanceX` 对 TTF 无快路径→`getGlyphMetrics→missThunk→faultGlyph` 每次 2×FT load+rasterize+打包；`TtfEpdFont` 新增 per-face advance 表(`{u32 cp,u16 adv12_4}` 有序,regular 4096/其他 512,PSRAM~44KB,跨页保留/换字号与 release 时清)与 `getAdvance()`(表命中零 FT；未命中 1×metrics load 无 render；cmap miss 走 AdvanceHandler)；`PushEmbeddedFont` 注册 advance handler：PIF1 新增 `pif1AdvanceOnly()` 直取 record[4]（无 coverage/打包）、SD 调其 getAdvance；`Pif1Codec.h` 加纯逻辑 `pif1AdvanceOnly()`；`GfxRenderer.cpp` getTextAdvanceX 加 TTF 快路径（与通用路径逐项对齐 bidi/连字/kerning/tracking/sup-sub/small-caps/合成回退，仅换 advance 来源）。advance 与 faultGlyph **同源**（矢量 truncate26_6_to_4(gm.advance26_6)/PIF1 scaledAdvance(rec[4],px)<<4/SD getAdvance），measure/draw 逐字节一致；PIF1 host 新增断言验证。绘制路径零改动（glyph arena/flush 不动，无空档无重复光栅化）。硬约束全过：glyph 16/0、PIF1 37/0、stack 10/0、位图边缘保护/Pico 三公式/矢量参数零改动、无逐字号调参。真机工具链编译 rc=0 零 warning Flash 91.5%/RAM 44.6%。全量 host 210/0+96/0+248/0+36/0+28/0+23/0+24/0+37/0+10/0+16/0+40/0。**A 级双冷编** app `a4c39043…`/factory `f2a9f3cc…` 四文件逐字节 cmp rc=0、epoch 1794813449、前三段 SHA 与前序一致、factory 6,069,248B。件 `crosspoint_eegoA4_b65-s3c-final-20261119-cfa4ef5f_factory.bin`（标注 b65-s3c-final）。**停等真机验收①③…⑯（含 43KB parse/build 实际值回填）；未开 S4；内嵌字库全局化/AI 卡片字形/S4b 框选均登记远期不启。** | `S3c收尾批_时间行下移与advance-only_施工回执.md`、`S3c收尾批_MANIFEST.json` |
| B6.5-S3c-P0fix3 | P0 第三次崩溃（Cache error / Dbus write to cache rejected @0x3c5afffc）修复 | **完成 + A 级双冷编出件 + 停等真机验收（b65-s3c-p0fix3）**：commit `23d08038`，从 S3c-final `cfa4ef5f` 继续。定位：ELF SHA 前9位 `0efc7c200` 核对一致；addr2line 5 帧=`__copy_move_backward`(stl_algobase.h:766)←`advanceOnly`(PushEmbeddedFont.cpp:140)←`getAdvance`(TtfEpdFont.cpp:706)←`getTextAdvanceX`(GfxRenderer.cpp:2236)←`measureWordWidth`(ParsedText.cpp:185)；`0x3c5afffc` 落 Flash DROM 只读窗口(map: drom0_0_seg 0x3c000020 r; .flash.rodata 止0x3c5a69ac/noload 止0x3c5add2b)。**假设判定：B 成立(reserve 扩容后 lower_bound 的 pos 悬空→insert 用失效迭代器 copy_backward 写非法地址)，A 推翻(fiFontMalloc=heap_caps_malloc SPIRAM 可写,表非const/非rodata)，C 推翻(单渲染任务 RenderLock 串行)。** host 盲区：此前 getAdvance 测试全是 SdCardFont 的另一实现，TtfEpdFont::getAdvance 表零覆盖。修复：先 reserve 后取 pos（抽共享头 `AdvanceTable.h` 设备/host 同一份代码）、hard cap 不突破、写入前 esp_ptr_internal/external_ram 可写校验否则走慢路径；advOnly 改运行时开关(GfxRenderer::setAdvanceOnlyEnabled + settings.json `advanceOnlyLayout:false` 可关,默认开,无需重刷/改APK/协议)。新增 host `AdvanceTable` 617/0 覆盖扩容边界/cap/OOM veto/重复/并发/迭代器失效 canary；全量 host 11 套 0RED。真机工具链 rc=0 零警告 Flash91.5%/RAM44.6%。**A 级双冷编** app `d19ebe7f…`/factory `d72ed75a…` 四文件逐字节 cmp rc=0、epoch 1794813449、新 ELF `90f6c9bf…`(≠崩溃件)。件 `crosspoint_eegoA4_b65-s3c-p0fix3-20261119-23d08038_factory.bin`（标注 b65-s3c-p0fix3）。**停等真机验收①-⑤；未开 S4；封面 y290 成果保留。** | `S3c_P0第三次崩溃_诊断与修复回执.md`、`S3c_P0第三次崩溃_MANIFEST.json` |
| B6.5-S3c+ | 全局字库：P1 字级回退 + Built-in (Fast) 字体项 + [EHP-PROF] 打点 | **完成 + A 级双冷编出件 + 停等真机验收（b65-s3c-plus）**：commit `d038a4a3`，从 S3c-P0fix3 `23d08038` 继续。基线三档实测 PUSH3.7KB=1169ms/PUSH46KB=11162ms/epub楷体=18031ms；采信六问答复（advOnly 默认开→建页零栅格化→18s 主体是解析+布局 CPU 非字形），裁定三项修正照办：①验收②改报建页+首屏渲染两个数；②FAST **不删 SD 层**（保留 tier③ 生僻字兜底，零 seek 靠绝大多数走内嵌）；③`g_sdFont` 按 face ctx 化。**任务一 P1**：`TtfEpdFont` 加 public `glyphForGlobal/bitmapForGlobal` 桥接（走 faultGlyph+bitmapThunk 成熟路径），`PushEmbeddedFont::installGlobalFallback` 给全局点阵 Noto Sans 可变 EpdFontData 副本挂 glyphMissHandler+vectorBitmapHandler，`glyphMissCtx` 指稳定 holder（face 重建零重挂），main.cpp 建 4 字号副本；缺字四层 builtin.ttf→PIF1→SD→占位，量宽/绘制同源不错位。**任务二 FAST**：复用 PUSH face（同一 TtfEpdFont+pushFontId，零额外 PSRAM、无第二套 handler、section cache 按书路径分文件不串页）；枚举/存储ID/UI 两入口/i18n 全ASCII/getReaderFontId resolver 早退(9处统一)/toJson-fromJson(缺字段默认FAST)/重启生效；UI FAST 优先避免显示不一致；不落 .cpfont。**任务三打点**：rasterizeGlyph26_6 前后累计→`[EHP-PROF]` 全ASCII 行，ERS Page render 后输出。裁定四问落地：LXGW 原路径加载、PSRAM ~1.6MB(6.2MB 充裕)、tier③ per-face ctx 指向 ttf_、tier③ null 优雅降级不崩。保持不变全过：P0静态缓冲+24KB栈、S3b dirty marker+clamp、AdvanceTable.h+617断言、pif1PxFromEmPx、位图边缘保护、封面y290、文案全ASCII。全量 host 11 套 0RED（AdvanceTable 617/0、stack 10/0、glyph 16/0 等）；可靠编译 SUCCESS RAM45.2%/Flash91.6%。**A 级双冷编** app `3b3ed92f…`/factory `6ecd4af7…`/MANIFEST `daf43c99…`/ELF `3fcb590b…` 四文件逐字节 cmp rc=0、epoch 1794813449、factory 6,071,120B(app 6,005,584B,+65536)。件 `crosspoint_eegoA4_b65-s3c-plus-20261120-d038a4a3_factory.bin`。**停等真机验收①-⑧（验收②回填建页/首屏渲染两数、[EHP-PROF] 栅格化累计）；未开 S4；.cpfont 分支与 LXGW advance 预热登记后续项。** | `S3c+_施工回执_全局字库与Built-inFast.md`、`S3cPlus_MANIFEST.json` |
| B6.5-S3c+2 | S3c+ 缺口补修：SD 矢量 TTF 字级回退链 | **完成 + A 级双冷编出件 + 停等真机验收（b65-s3c-plus2）**：commit `2cbdaea1`，从 S3c+ `d038a4a3` 继续。缺口：S3c+ 只给内置点阵 Noto Sans 挂了回退，SD 矢量 TTF（DejaVuSans/LXGW）走 `loadTtfFamily` 时 `secondary_=nullptr`，CJK cmap miss 直接方块——用户选 DejaVuSans 读中文仍乱码。补修：`TtfEpdFont` 加 `primaryAdvanceOnly`（纯 primary 表+cmap metrics，不调 advanceHandler_/不栅格化，tier① 取 builtin 矢量 advance 零递归）；`FallbackContext` 加 `builtinFace`；`PushEmbeddedFont` 的 secondary/advanceOnly 开头插 tier① builtin 矢量（`rasterizedBitmap` 纯 cmap）+ tier② PIF1 + tier③ 自引用守卫；新增 `installSdVectorFallback` 挂到 SD 矢量 face；`SdCardFontSystem` 独立 `sdTtfFallback_` ctx + `refreshSdVectorFallback()`（loadTtfFamily 两成功路径 + ensurePushFont 末尾调用）。**tier③ 语义裁定：SD 矢量 face 自身即用户所选 SD 字体，tier③ 不设（指向自身=no-op/递归，不偷加载 LXGW），中文由 tier①②（builtin 1260+PIF1 5650 字，97%+）覆盖，tier④ 占位兜底**。递归安全：tier① 用纯 primary 查询不回环、tier③ self-guard 双保险；PUSH/FAST 用独立 ctx（builtinFace=nullptr、tier③ 指 ttf_）零覆盖。共享头 `FallbackChain.h` 设备/host 同一份，新增 17 host 断言（tier 顺序+自引用守卫+两套 ctx 不变量）堵 host 盲区。cold_build 脚本修 REAL_BUILD_RC：tee+PIPESTATUS[0] 显式断言真实码。全量 host 12 套 0RED（AdvanceTable 617/0、fallback 17/0 等）；编译 rc=0。**A 级双冷编** app `0a70058c…`/factory `14a44ed5…`/MANIFEST `20d14cec…`/ELF `370cb643…` 四文件逐字节 cmp rc=0、epoch 1794813449、脚本两轮打印 `pio run real exit code = 0 OK`、factory 6,071,856B（app 6,006,320B）。件 `crosspoint_eegoA4_b65-s3c-plus2-20261120-2cbdaea1_factory.bin`。**停等真机验收①-⑩（①DejaVuSans 中文不方块为核心、④建页/渲染两数、⑩缓存对比挂账）；未开 S4；.cpfont/LXGW advance 预热登记后续项。** | `S3c+2_施工回执_SD矢量回退缺口补修.md`、`S3cPlus2_MANIFEST.json` |
| B6.5-S4~S5 / B6 / B11 其余 | — | 未开始（S4 自动打开+角落提示+UI+失败可见+完整互斥 UI；S5 联调；四问答复见 `B6.5开工前_四问答复.md`；B11 其余真机格位待验） | — |

## B2 指纹（本批基线）

- app sha256 `ea080a81bb789bbf355a3307e083b0fb211108b0126592639865c0d7ee9745e1`（5,370,752 B，较 B1 −66,448 B）
- factory sha256 `7ab026ee2e02959fa17738aefa857473beb1b23022f2c0c1449c4d424af99892`（5,436,288 B；factory−app=65,536）
- 双冷编（clean + 空缓存 + epoch 锚 `490813ff`）app/factory 两次同 sha，selfcheck PASS（A 级形式闭合，见点核 §四）
- strings 四计数：clipping=0 / Clippings=0 / My Clippings=0 / Bookmark=0 ✓
- 回滚点：git tag `b1-baseline` + `rebuild/b2_backup/`（29MB）
- 交付刷机包：`/workspace/crosspoint_eegoA4_b2-20261117-704912a1_factory.bin`（三段式整改后命名，sha 不变；底座裁剪件，仍非日常用件）

## B3 指纹（本批基线）

- commit `4ad75499`（6 文件 / +340 / −7）；前一干净基线 `8e34ed38`
- app sha256 `38ebe92315b1271cd13329c4bfeb65e13eca01846f6855c1538f3650034139b4`（5,374,912 B，较 B2 +4,160 B = BLE 连接层净增）
- factory sha256 `7402f16935c52240d5b71e9198d60447315876ff6d3d8b2d75a70a492a45c458`（5,440,448 B；factory−app=65,536）
- 双冷编（clean + 清 .cache + epoch 锚 `490813ff`=1794813449，无管道 source + 断言）app/factory 两次同 sha，selfcheck PASS（A 级形式闭合，见 B3 回执 §五）
- 关键能力：`PicoPush-%04X`（token=Wi-Fi STA MAC `mac[4]<<8|mac[5]`）全局长驻广播；GATT 服务 `e2f5…5f10` + DATA(5f11)/CTRL(5f12) WWR + STATUS(5f13) READ\|NOTIFY；MTU 247 无 bond；`g_bleConnected`/`g_bleReceiving` 原子标志；状态栏被动蓝牙图标（禁角标/闪烁）
- 交付刷机包：`/workspace/crosspoint_eegoA4_b3-20261117-4ad75499_factory.bin`（BLE 连接层验证件，仍非日常用件）

## B1 验证补记（含点核裁定）

- 两次独立冷编 firmware sha256 完全一致（`db72df145572bcde4b727c9aa16675eef4a032ca0cba2d496e5868607af57d74`）。
- `archive_segments.py` 自建 factory 与 PlatformIO 自带 factory 逐字节一致（`2c72f26eb90b5bb14ae5330834c1523037b3e2539f226f9110a6bfbde527197f`），selfcheck 四段 PASS。三段闭环证据形态采信。
- MANIFEST 补 `source_date_epoch=1794813449`（真实编译值，可复跑要素），由 archive_segments.py `--source-date-epoch` 自动入册。
- 交付刷机包（命名整改后）：`/workspace/crosspoint_eegoA4_b1-993c4e63-20261117-993c4e63_factory.bin`（5,502,736 B）；定位标注"底座验证用，非日常用件"，见 `/workspace/刷机件说明.md`。
- 裁定：B1 **有条件通关**（编译面即时生效）；K1 文字页 / 冷启动真机格位抽验云端不可达，并入 **B11 追责条款**——红格时先刷 B1 底座件复测该格、再查后续批次，分离底座责任与批次责任。

## 点核领账 / 规则（审计）

1. **【账·终态锚适用性】** 白皮书 §1 体积锚 app ≈5,326,400 B 是**终态锚（B11 产物）**，出具时未注明适用批次，致 B1 app 5,437,200 B 需解读方能对表。B1 读数较该锚 +110,800 B 属"含裁剪面、未含 BLE"的预期形态；此锚不作 B1 硬判据，仅作 B11 终态对表。
2. **【账·上游漂移】** B1 app 较 P4 +52,624 B = 旧基线之后 crosspoint develop 已演进的**上游漂移实据**，构成 B2 路径普查令的依据。
3. **【规则·行号锚定锚点 commit】** 计划中 file:line 凡采自 HEAD 者，落码前必须对其所声明的锚点 commit 复核（白玉书三端互证基准）。已正确示范：pico-plus 锚点无 `push_part.c`（分片逻辑在 `push_sink.c` 2322 行）；B2 裁剪清单录自旧树，须在现树 HEAD 普查存在性后先报后删。
4. **【规则·语义版本前导零】** platformio 规范名 `espressif32@55.03.311` 被语义版本解析器拒（前导零 `03` 非法）；本地覆盖改用 `file:///root/.platformio/platforms/espressif32`，绕开 URL 重装 + 版本解析双坑。

## S3c+++ (b65-s3c-cn) 指纹

- commit `2f16dbc6`（7 文件 / +953 / −25；前一干净基线 `f56449fd`）
- app sha256 `6ab56923c6971a38c35d1eeb8c1010d1620d208ad95a32148b9e872da94cd260`（6,020,320 B）
- factory sha256 `bfdee546d1561e275ed9fcedfa3ee676fb5266fadfe316236aeb422239a1c431`（6,085,856 B；factory−app=65,536）
- A 级双冷编（双删 .pio/build + .cache、无管道 source、`SOURCE_DATE_EPOCH=1794813449` 断言、PIPESTATUS[0] 真实退出码）：app/factory/elf/MANIFEST 四产物逐字节 IDENTICAL，CMP_RC=0，REPRODUCIBLE OK
- 关键能力：UI 字符串级链式 fallback（ubuntu 点阵 → SD 用户字体 → 内置 builtin.ttf variant → 占位符，UiFallbackChain.h device/host 同码）；三档常驻 UI variant 8/10/12pt（16KB/384 each）复用 installInto 的 PIF1 二级；中文语言包 647 key 全译、CHINESE/zh-Hans、36 语言、切换即生效
- 交付刷机件：`/workspace/crosspoint_eegoA4_b65-s3c-cn_factory.bin`（待真机验收，非日常用件）

## S3cPF / S3c-cn+ (b65-s3c-cn-pushfont) 指纹

- commit `e32afd41`（5 文件 / +30 / −2；前一干净基线 `0a00d0c6`）
- app sha256 `e7d0452d213a44e3fe5bdb71c0ff8b56e140872fb743b6c78a2c0653e549bd5f`（6,020,480 B）
- factory sha256 `38bfc41d6eec22eae099d2306668be5549d0bb143abece973fb5e309fa019d29`（6,086,016 B；factory−app=65,536）
- A 级双冷编（双删 .pio/build + .cache、无管道 source、`SOURCE_DATE_EPOCH=1794813449` 断言、PIPESTATUS[0] 真实退出码）：app/factory/elf/MANIFEST 四产物逐字节 IDENTICAL，A_LEVEL_CLOSURE=PASS
- 关键能力：PUSH Font 设置项（Built-in=0 默认 / Follow Global=1），由 SettingInfo::Enum 自动持久化；Follow Global 下 PUSH 书复用全局 face + per-face 热 advance 表（跨书共享，改动量 0），Built-in 路径零改动；预加载按裁定不实施
- host 15/15 全绿；验证重点：设置项切换/持久化、Follow Global 首屏跟随全局、Built-in 零回归
- 交付刷机件：`/workspace/crosspoint_eegoA4_b65-s3c-cn-pushfont_factory.bin`（待真机验收，非日常用件）

## S3cPF2 (b65-s3c-cn-pushfont2) 指纹

- commit `2f031f16`（3 文件 / +52 / −2-9；前一干净基线 `388408df`）
- app sha256 `e7fb6062a6bf50e8b0f64fadf68b3fda4e3f2c690fadb2ba20c25580cda06be3`（6,020,544 B）
- factory sha256 `83d3514d0755e39a7830e0d5e93e383ec2bb686bdc65313db8ba3c3aababc7e3`（6,086,080 B；factory−app=65,536）
- A 级双冷编：app/factory/elf/MANIFEST 四产物逐字节 IDENTICAL，A_LEVEL_CLOSURE=PASS，epoch=1794813449，真实退出码 0
- 关键修复：fontFamily/sdFontFamilyName 字段一致性。项A applyFamily SD 分支同步 fontFamily=NOTOSANS（消除 FAST+LXGW 半选态）；项B fromJson 缺字段默认回退 STORED_NOTOSANS（S3c+ 静默变更回滚，不改已存值）；项C relinkUiFallbacks FAST 下 builtin 提主 fallback、SD 降二级（不改 resolveTextFontId/UiFallbackChain 本体，不引入新字段），ensureLoaded 入口幂等重链修正开机时序。FAST 下 sdFontFamilyName 保留供 tier③ 兜底
- host 15/15 全绿；验收核心：UI 显示与正文渲染始终一致（⑤）、点选 LXGW 选中态立即保持（⑪）
- 交付刷机件：`/workspace/crosspoint_eegoA4_b65-s3c-cn-pushfont2_factory.bin`（待真机验收，非日常用件）

## S3cPF3 (b65-s3c-pf3) 指纹

- commit `6dc04b07`（8 文件 / +101 / −8；前一干净基线 `bb6c42ba`）
- app sha256 `1c52ba659b5c4ef5b97b4892fccad0d0059ce184d5106d49d205fdc1ff7f7a5b`（6,021,712 B）
- factory sha256 `42b81386b34c1f9c0a313218effceb4a885ceaad9901afc5131515050116da35`（6,087,248 B；factory−app=65,536）
- A 级双冷编：app/factory/elf/MANIFEST 四产物逐字节 IDENTICAL，A_LEVEL_CLOSURE=PASS，epoch=1794813449，真实退出码 0（两次）
- D1：TextSettingsActivity Family tab 末尾加「PUSH 字体」独立行，复用 optionPopup_ 二选项（内置=0/跟随全局=1），沿用 pushFontMode 持久化；不改主设置页 inTextSettings 过滤，不加第五 tab。切换即 saveToFile，缓存靠 spec.fontId 失效
- P1 图标：Lyra PUSH 项从 UIIcon::Text（iconForName 无匹配→nullptr→不画）改为 UIIcon::Push→PushIcon（push.h 32×32 inbox，按 drawIcon (X=31-row,Y=col) 逆变换生成，真机方向正确）；封面网格 HomeActivity::appendPushToCoverGrid 把 /PUSH.txt 作虚拟书追加为末格，封面路径与 push thumb_400.bmp 一致
- D2 日志：effectiveReaderFontId 单出口打 [ERS] reader font: book/mode/fontId；ensurePushFont 重建分支打 [SDFS] push face rebuilt: pt/reason。仅观测，不动耗时/缓存逻辑
- host 15/15 全绿；验收核心：①-⑦ D1 可达与交互、⑧-⑫ 图标与网格、⑬-⑭ 日志、⑮-⑯ 不回归与三主题
- 交付刷机件：`/workspace/crosspoint_eegoA4_b65-s3c-pf3_factory.bin`（待真机验收，非日常用件）

## S3cPF4 (b65-s3c-pf4) 指纹

- commit `0dd0e0a7`（5 文件 / +75 / −30；前一干净基线 `edcc07d0`）
- app sha256 `d8fa3923fa7ff8171d94a022e7534cebec96b17a40ea0e558713845fcb789a0a`（6,021,984 B）
- factory sha256 `b185b99e7503bdad68164adb712a5f03a906aca613c95549cb70205d646a421a`（6,087,520 B；factory−app=65,536）
- A 级双冷编：app/factory/elf/MANIFEST 四产物逐字节 IDENTICAL，A_LEVEL_CLOSURE=PASS，epoch=1794813449，真实退出码 0（两次）
- 图标方向：pf3 对 push.h 做了多余的"逆drawIcon预变换"致真机马桶塞；pf4 按视觉 row/col 直接编码（与 folder.h 同约定，无预变换），字符画+实际尺寸对比图验证箭尖朝下/U口朝上、2px描边对齐、三点留白可辨。注意该图标上下对称，flipUD 无效
- 网格入口：根因=pf3 的 append 只在 if(coverGridUi) 分支，LYRA_3_COVERS/Lyra/Classic/RoundedRaff 走独立主题、coverGridUi==nullptr 从不执行。pf4 新增 ensurePushHomeTile(displaySlots)，所有主题 onEnter 都确保 PUSH 虚拟书入列（末格），点击走 onRecentBookSelected→goToReader("/PUSH.txt")
- 各主题 PUSH 封面：根因=PUSH 封面只由 PushThumb 生成固定 thumb_400.bmp，RecentBook 路径带 [HEIGHT] 占位符被主题替换成 226/300 → 请求不存在的 thumb_226/300.bmp → 退化为书本占位。pf4 在 ensurePushHomeTile 里把 PUSH 路径 pin 成 thumb_400.bmp（getThumbBmpPath(400)，无占位符），所有主题 getCoverThumbPath no-op、加载同一封面并各自 drawCoverThumbFill 居中裁切。经典布局/各主题绘制零改动，不新增落盘 bmp
- 日志去重：[ERS] reader font 从 4 个热路径调用（620ms~50条，115200baud 阻塞串口约250ms）；新增 mutable ReaderFontLogKey 三元组(book/mode/fontId)，仅变化时打印，翻10页≤2条，日志本身保留。Idle prewarm 两条内容随页变化不属刷屏，未动
- 速度评估(P2只答未优化)：37969B mode=0 的 parse ms 日志被用户截断无法给实测(不编造)；4.2ms/字线性外推是上界(重复字命中 per-face advance 表近乎零成本)；日志去重约省 250ms 量级；advance 预热是迁移成本非消除，先复测再定
- host 15/15 全绿；验收核心：①-② 图标方向/对比图、③-④ 网格PUSH格封面、⑤-⑥ 日志去重、⑦-⑧ 不回归与大字/时间可读
- 交付刷机件：`/workspace/crosspoint_eegoA4_b65-s3c-pf4_factory.bin`（待真机验收，非日常用件）

## S3cPF5 — PUSH 矢量封面 + 内置耗时（b65-s3c-pf5，2026-11-20）
- commit `fd66c33f`（15 文件 / +494 / −120；前一干净基线 `35488d6d`）
- app sha256 `d806a51841bef8ba52d6a7213eecd4fa27c782e27e64e39ba9a63922801a7d4b`（6,023,504 B）
- factory sha256 `361c29b90a3748ccfd8c0d385b30db5a7a260cab50f5164be3660bb0c1fd564e`（6,089,040 B；factory−app=65,536）
- A 级双冷编：app/factory/elf/MANIFEST 四产物逐字节 MATCH，A_LEVEL_CLOSURE=PASS，epoch=1794813449，真实退出码 0（两次）
- 封面根因：thumb_400.bmp(240×400 竖版)被 drawCoverThumbFill 居中裁切进各主题近方形格子（Lyra135×226/Lyra3Covers213×226/RoundedRaff180×300/网格113×170），横向切"P"、纵向切日期。修：抽出 pushthumb::renderCoverInRect()（纯矢量+ink回调、库内 clip），writeCoverBmp 复用同一核心（经典 240×400 经 cmp 字节一致零回归）；BaseTheme::drawPushCoverInRect() 把 ink 接 renderer.drawPixel，各主题传自己矩形（非PUSH书走原bmp逐字不改）；thumb_400.bmp 生成逻辑全保留。小卡 150×150 大字墨迹 18px(≥14阈值)，w<120&&h<150 省略时间行防裁断
- 内置耗时根因：secondary() tier④ 失败不缓存，缺失码点每次出现/每次翻页重跑 builtin→PIF1→SD 三级栅格化并打 PFNT 日志（O(出现次数)超线性）。修：FallbackContext 加 256 容量已确认缺失码点集合，secondary()/advanceOnly() 入口 isMissing 短路，tier④ markMissing 首次才打日志；face重建/setSdFallback 变更时 resetMissing。header +30 / cpp +31 行，不动 TtfEpdFont/阅读器。命中字零改动故 2640B(840ms)零回归；6236B 预期降到2秒级属机制预期，端到端待真机复测(不编造)
- host 15/15 全绿（PushThumb 40→71 checks、新增 MissingCache 14 checks）
- 交付刷机件：`/workspace/crosspoint_eegoA4_b65-s3c-pf5_factory.bin`（待真机验收）
