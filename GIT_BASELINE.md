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
