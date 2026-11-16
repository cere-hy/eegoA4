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
| B2–B11 | — | 未开始 | — |

## B1 验证补记

- 两次独立冷编 firmware sha256 完全一致（`db72df145572bcde4b727c9aa16675eef4a032ca0cba2d496e5868607af57d74`）。
- `archive_segments.py` 自建 factory 与 PlatformIO 自带 factory 逐字节一致（`2c72f26eb90b5bb14ae5330834c1523037b3e2539f226f9110a6bfbde527197f`），selfcheck 四段 PASS。
- 交付刷机包：`/workspace/eego_a4.factory.bin`（5,502,736 B）。
- 未验证项：K1 文字页 / 冷启动真机格位抽验（云端无硬件，顺延真机批次）。
