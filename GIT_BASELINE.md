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
| B2 | S8 裁剪 | **完成**：30 删/5 保留/17 引用清理 + i18n 23 键；strings 四计数全 0；双冷编 app/factory 同 sha（A 级闭合）；commit `704912a1` | `docs/rebuild/B2_裁剪回执.md` + `B2_路径普查报告.md` |
| B3–B11 | — | 未开始 | — |

## B2 指纹（本批基线）

- app sha256 `ea080a81bb789bbf355a3307e083b0fb211108b0126592639865c0d7ee9745e1`（5,370,752 B，较 B1 −66,448 B）
- factory sha256 `7ab026ee2e02959fa17738aefa857473beb1b23022f2c0c1449c4d424af99892`（5,436,288 B；factory−app=65,536）
- 双冷编（clean + 空缓存 + epoch 锚 `490813ff`）app/factory 两次同 sha，selfcheck PASS（A 级形式闭合，见点核 §四）
- strings 四计数：clipping=0 / Clippings=0 / My Clippings=0 / Bookmark=0 ✓
- 回滚点：git tag `b1-baseline` + `rebuild/b2_backup/`（29MB）
- 交付刷机包：`/workspace/crosspoint_eegoA4_b2-704912a1-20261117-704912a1_factory.bin`（底座裁剪件，仍非日常用件）

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
