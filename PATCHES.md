# PATCHES.md

本文件登记本仓库相对上游依赖/原主仓的私有补丁。每条补丁必须说明：根因、改动、验证、回退方法。

## P-001：NimBLE host task 生命周期修复（b65-s3c-pf9c）

### 根因

`nimble_port_stop()` 的两次 `sem_pend` 只证明 stop 回调已经执行，host task 仍在 `nimble_port_run()` 栈内，尚未返回，也未执行 host task 清理。主仓随后调用 `nimble_port_deinit()` 会释放 host task 仍在使用的事件队列，导致 `InstrFetchProhibited` / `PC=0x0`。pf9b-2 真机三次切换一成一崩，确认这是崩溃根因，而不是 WiFi 内存不足导致的次生现象。

### 改动

vendor 依赖：`third_party/NimBLE-Arduino/`（NimBLE-Arduino 2.5.1，源码从 registry 原样保留，仅叠加本补丁）。

1. `nimble_port.h`：新增两个封装 API，避免主仓裸 `extern` 私有符号：
   - `void nimble_port_host_exited_signal(void);`
   - `esp_err_t nimble_port_wait_host_exit(uint32_t timeout_ms);`
2. `nimble_port.c`：
   - 新增私有 `static EventGroupHandle_t s_host_task_exited`。
   - `nimble_port_init()` 先 `xEventGroupCreate()`，随后立即 `xEventGroupClearBits(HOST_TASK_EXITED)`，避免第二次会话读到上一轮陈旧 bit。
   - `nimble_port_deinit()` 最后一步 `vEventGroupDelete()` 并置 NULL。
   - `nimble_port_stop()` 在两个 `sem_pend` 后等待 host 真实退出；事件组为 NULL 时走幂等分支直接 `return ESP_OK`，不等待、不 delay；等待成功才 `vTaskDelay(2)`；超时返回 `ESP_ERR_TIMEOUT`，调用方不得继续 deinit。
3. `NimBLEDevice.cpp::host_task()`：`nimble_port_run()` 返回后、`nimble_port_freertos_deinit()` 前调用 `nimble_port_host_exited_signal()`。
4. 主仓调用侧：`lib/BlePushGatt` 恢复 `STOPPED_FOR_WIFI / RESUMING / DEAD` 状态机；`stopForWifi()` 在 deinit 失败或 host 退出超时时进入 DEAD，不继续拆栈；`resumeForWifi()` 采用保守方案 A，按 `esp_bt_controller_get_status()` 自适应回到 IDLE 后，通过 `NimBLEDevice::init()/begin()` 正常路径恢复。DEAD 为终态，不再重试。
5. 恢复点：`CrossPointWebServerActivity::onExit()` 在应用层服务关闭、`esp_wifi_disconnect()/stop()/deinit()` 全部完成后才调用 `resumeForWifi()`；WiFi 选择页退出或连接失败均不提前恢复，避免 pf9b-2 的“恢复太早吃掉 41KB、Web 服务器起不来”。

### 验证

- host 规格断言 `pf9c_host_nimble.cpp`：30 checks，0 RED，覆盖 task 未退出、已退出、超时、重复 stop、deinit 后 stop、resume 状态机。
- vendor NimBLE 源码除本补丁外与原 registry 逐字节一致。
- A 级双冷编结果见本批施工回执；验收门槛为冷编 rc=0、四产物逐字节一致。

### 回退 / 同步上游

- 完全回退：用 NimBLE-Arduino 2.5.1 原版替换 `third_party/NimBLE-Arduino/`，并 revert `lib/BlePushGatt`、`WifiSelectionActivity`、`CrossPointWebServerActivity`、i18n 中 pf9c 段落；主仓回到 pf9b-3a 的 stop-only 行为。
- 同步上游新版本：先在 vendor 目录重新打 P-001 的 3 处 NimBLE 改动，再核对主仓 side 的 DEAD/resume 适配；未打补丁前不得启用 `resumeForWifi()`。

## P-002：`TtfEpdFont::advance12_4FromRaw` 的 x_scale 对齐（PF9a）

### 根因

共享 `FtFont`/`FT_Face` 在同一字族多个变体间复用。若 face 当前 ppem 仍停留在兄弟变体的 `size26_6_`，直接用 `rawAw` 乘 `x_scale` 会取到错误 ppem 下的缩放系数，表现为中文 advance 热表错配、字距异常。

### 改动

文件：`lib/EpdFont/TtfEpdFont.h` / `TtfEpdFont.cpp`。

`advance12_4FromRaw()` 改为非 const；在读取 `x_scale` 前调用 public `lineMetrics26_6(size26_6_, alignSink)`，通过外部 target 让 `ensureSize26_6()` 真正把共享 face 对齐到当前变体尺寸，再执行 `unitsToAdvance26_6(rawAw, adv26)` 与 26.6→12.4 截断。热表命中 H 与 FontMap 命中 M 均走该单一口径。

不使用私有 forwarder，也不再依赖 `freeink-sdk` 内补丁，主仓独立自洽。

### 验证

- `pf9a_linemetrics_verify.log` 证明 `lineMetrics26_6()` 非 no-op：target 不同会重建 size，target 相同为缓存 no-op。
- `pf9a_syntax_check*.log` 与 PF8e-8 / PF9a 双冷编通过；排版侧不再出现 x_scale/advance 热表错配。

### 回退 / 同步上游

将 `advance12_4FromRaw()` 改回 const 版本，删除 `lineMetrics26_6()` 对齐调用并直接 `unitsToAdvance26_6(rawAw, adv26)`；同时把 H/M 两个源统一改回旧口径。该回退会恢复共享 face 的 x_scale 错配，不应在无替代修复时执行。

## 环境注意事项：PlatformIO 全局包 `.piopm` URI

`~/.platformio/packages/framework-arduinoespressif32-libs/.piopm` 的 `spec.uri` 曾带 `gh-proxy.com` 前缀，standalone clone 首次冷编时 registry 判定源不匹配并触发网络重装，网络中断后施工被打断。编译与提交已分离：原 worktree 负责编译，standalone 只负责版本控制与推送。不要通过长期修改全局缓存 `.piopm` 规避该问题；若必须临时调整，先备份并在本文件登记恢复方法。
