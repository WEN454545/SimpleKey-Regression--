﻿﻿﻿﻿﻿# SimpleKey 调试上位机 使用说明手册

> 版本：v0.5（2026-09-24，宏录制：0x0E 录制控制 + 键盘实按录制回填草稿 + 时长上限；同日修复 0x8A 宏回读解析 off-by-one 与双通路 [EVT] 录制去重）
> 历史版本：v0.4（2026-09-22，P6：宏编辑器 + 旋钮输出配置 + 媒体/宏键码分组 + 右栏属性面板三栏布局）；v0.3（2026-09-21，P5：WiFi 热点通路 + SendInput 注入 + 左栏可收起）
> 定位：SimpleKey 三模键盘的开发调试工具，后续将渐进成长为完整配置上位机（键位编辑 / Profile 管理 / 宏等）
> 技术路线决策记录见 AGENTS.md（2026-09-19 对齐：Tauri 2 起步、监控+控制、[EVT] 结构化事件、VIA 风格浅色主题）
> 排障手册见 `docs/上位机与联调排障手册.md`（联调问题的修理思想与知识点汇总）

---

## 1. 功能总览（v0.5）

| 功能 | 说明 |
|------|------|
| 串口连接 | **顶栏收成「串口」小按钮（带连接状态点），点击弹出浮层**：自动枚举系统 COM 口，一键连接 / 断开（115200 8N1）；自动置 DTR（USB-Serial-JTAG 要求，见 §8）。固件 v2.3 起 USB 模式经 TinyUSB CDC 虚拟串口（PID 0x4006）走同一套协议，无需拔线 |
| 蓝牙监控 | 连接键盘自定义监控服务（0xFFE0/0xFFE1），BLE 模式下按键/旋钮事件同样实时显示（Windows 独占 HID 服务 0x1812，无法订阅，故走自定义服务） |
| WiFi 热点通路 | 键盘 WiFi 模式开热点「SimpleKey_绝希」（密码默认 simplekey），点「连接 WiFi」自动连热点 + TCP 192.168.4.1:8266 + 帧协议握手；按键事件经 SendInput 注入本机（WiFi 模式键盘不发 HID），键位编辑/Profile/电量与串口、蓝牙完全同体验（cfg_send 优先级：串口 → NET → BLE） |
| 键位矩阵实时测试 | 4×5 网格，按键按下高亮（蓝色）、释放恢复，**未映射键位也会响应**（矩阵测试用） |
| 键位在线编辑 | 点击网格键位 → Bottom Sheet 键码选择器（分组网格，含媒体/宏分组）→ 立即下发（RAM 生效）→「保存配置到键盘」持久化到 NVS；连接后自动读取设备当前键位回显到网格（三通路均支持） |
| 宏编辑器（P6） | 4 槽位页签，步骤化编辑（按下/松开/延迟/轻点，≤32 步），名称 ≤7 字节；帧协议 0x0A/0x0B 读写（RAM 生效，「保存配置到键盘」持久化）；USB/BLE 模式固件本地播 HID，WiFi 模式经 [EVT] MACRO 行由上位机 SendInput 播放 |
| 宏录制（v0.5） | 「● 录制」开关 + 最长时长（5s/10s/30s/60s，超时自动停）+ 录制中倒计时；录制期间键盘抑制 HID 输出（字符不外打），上位机收集 [EVT] KEY 行打时间戳生成步骤（间隔 ≥20ms 生成延迟，超 32 步截断提示，串口+BLE 双通路在线时同键同状态 50ms 内去重防重复步骤），停止后回填草稿，确认「下发宏」保存；仅串口/BLE 通路可用（帧协议 0x0E） |
| 旋钮输出配置（P6） | 顺/逆时针输出可改为任意媒体功能（音量/静音/播放/切曲/亮度），下拉选择即下发（0x0C/0x0D，「保存配置到键盘」持久化）；v1.9 起短按也可配（切换 Profile 默认/媒体功能/触发宏 M1~M4，协议 3 字节 [cw,ccw,press]） |
| Profile 管理 | **移入右侧「配置中心」覆盖式抽屉**（⚙ 竖条入口）：3 槽位点击切换 / 名称重命名 / 导出导入 JSON（帧协议 0x06~0x08），缩略图 19 键与主网格同构；切换后自动回读键位/宏/旋钮 |
| 电量监测 | POWER_GET（0x09）5s 轮询，状态条 + 120 点电压曲线 |
| 编码器事件 | 顺/逆时针计数、按下/释放/长按状态显示 |
| 事件通道自检 | 启动时后端自动 emit 一条测试事件，验证 Rust→前端事件链路（排障用，见排障手册 §3.8） |
| 命令控制区 | 左栏图形化按钮发送命令（模式/Profile/状态查询/LED）+ 自定义命令输入；**面板可收起（« 收起），收起后左侧留细条按钮（»）随时弹出** |
| 配置中心抽屉 | 右缘「⚙ 配置中心」竖条 → 覆盖式抽屉第一级显示分类（Profile 管理 / 旋钮配置 / 宏编辑），选定后抽屉内切换到对应配置界面；点遮罩或 × 关闭，← 返回分类列表；键位网格保留常驻主区不进抽屉 |
| 日志台 | 全量串口日志 + 「只看事件」过滤 + 自动滚动 + 按级别着色；**「▼ 收起」可整体收起为底部细条（显示条数，点击展开），主工作区随之变大** |

## 2. 环境要求

| 组件 | 版本要求 | 用途 |
|------|----------|------|
| Node.js | ≥ 20（开发用 24） | 前端构建与包管理 |
| Rust | stable（MSVC target） | Tauri 后端编译 |
| VS Build Tools | 2022 + 「使用 C++ 的桌面开发」工作负载 | Rust MSVC 链接器（link.exe） |
| WebView2 Runtime | Win10/11 一般自带 | 应用内嵌浏览器内核 |
| 键盘固件 | 支持 `[EVT]` 事件输出（v1.2 协议起） | 事件数据源 |

## 3. 安装与运行

```powershell
cd host-app

# 首次：安装前端依赖
npm install

# 开发模式（热重载，Rust 增量编译）
npm run tauri dev

# 正式打包（产出 exe / msi，位于 src-tauri/target/release/bundle/）
npm run tauri build
```

**注意事项**：
- `npm run tauri dev` 需要终端能找到 `cargo`（rustup 安装后新开终端即生效；老终端需手动把 `%USERPROFILE%\.cargo\bin` 加入 PATH）
- 首次编译 Rust 后端需下载 300+ crate 并编译，耗时数分钟；此后增量编译只需数秒
- 若 Vite 报 `EBUSY ... src-tauri\target\...pdb`：确认 `vite.config.ts` 已配置 `server.watch.ignored`（本仓库已配置，勿删）

## 4. 使用步骤

1. **烧录固件**：确认键盘固件已烧录最新版（含 `[EVT]` 事件输出，协议见 `docs/串口命令协议.md` §3.5）
2. **连接键盘**：USB 线插上键盘，设备管理器/上位机端口下拉框会出现对应 COM 口
3. **连接**：选择 COM 口 → 点「连接」（绿色按钮，成功后右侧圆点变绿）
4. **测试按键**：按下键盘任意键，中间网格对应 `(row,col)` 格子变蓝高亮；释放恢复灰色
5. **测试旋钮**：旋转 → 顶部「顺时针/逆时针」计数变化；按下 → 「按下」高亮；长按 >800ms → 显示「(长按)」且模式自动切换
6. **编辑键位**：点击网格任一键位 → 底部弹出键码选择器（分组网格，当前键码高亮）→ 点选新键码立即经串口下发（RAM 生效）；点「保存键位到键盘」写入 NVS，断电不丢
7. **蓝牙监控**：键盘与电脑配对后（BLE 模式），点顶部「连接监控」，BLE 模式下的按键/旋钮事件同样在网格高亮显示
8. **发送命令**：左栏点按钮或底部自定义输入框敲命令回车，日志台可看到固件响应（`[OK]/[INFO]/[ERR]`）；左栏不需要时点「« 收起」隐藏，左侧细条「»」可重新弹出
9. **WiFi 热点通路**：键盘切 WiFi 模式（旋钮长按或 `mode wifi`，绿灯）→ 电脑连接热点「SimpleKey_绝希」（密码 simplekey）→ 点状态栏「连接 WiFi」→ 自动连热点 + TCP + 帧协议握手；此后键盘按键经 SendInput 注入本机（需上位机保持连接，属设计使然），键位编辑/Profile/电量与串口蓝牙同体验
10. **配置宏与旋钮（P6）**：右栏「宏编辑」卡片选槽位页签 → 加按键/加延迟构建步骤序列 → 「下发宏」（RAM 生效）；「旋钮配置」卡片改顺/逆时针/按下输出功能（下拉即下发；按下默认为切换 Profile）；改完点「保存配置到键盘」一并持久化到当前 Profile；键盘上把任意键位设为 M1~M4（或旋钮设为媒体/宏功能）即触发。**宏按了没反应？先看日志有没有 `[EVT] MACRO EMPTY`——有就是该槽位宏为空（没下发或没保存）**
11. **排查问题**：勾选「只看事件」只显示 `[EVT]` 行；取消勾选看全量日志（含 ESP-IDF 启动日志）

## 5. 事件与命令协议（速查）

上位机与固件之间是**纯文本串口协议**（UTF-8），完整规范见 `docs/串口命令协议.md`。

**上位机 → 固件**：`mode ble`、`profile set 2`、`led blink 3` 等，`\r\n` 结尾

**固件 → 上位机**（上位机只把行首精确为 `[EVT]` 的行当事件解析，其余当日志）：

```
[EVT] KEY DOWN <row> <col> <0x键码> <键名>   矩阵键按下（键码与键名供扩展解析）
[EVT] KEY UP <row> <col> <0x键码> <键名>     矩阵键释放
[EVT] ENC CW / CCW <格数>                    旋钮旋转（四倍频合并后格数）
[EVT] ENC PRESS / RELEASE                    旋钮按下/释放
[EVT] ENC LONG                               旋钮长按确认
[EVT] MODE usb / ble                         模式切换成功
[EVT] PROFILE <N>                            Profile 切换成功
[EVT] CC <功能名>                             Consumer 功能键按下（WiFi 模式，P6；SendInput 注入）
[EVT] MACRO KD/KU 0x<键码> <名> / DL <ms> / CC <名> / END   宏播放步骤（WiFi 模式，P6；EMPTY <N>=空宏触发诊断）
```

keymap 命令回显格式（上位机据此把设备当前键位同步到网格）：

```
Row <n>: 0xXX(键名) 0xXX(键名) 0xXX(键名) 0xXX(键名)
```

## 6. 技术栈与原理简介

### 6.1 整体架构：Tauri 2

Tauri = **Rust 原生后端 + 系统 WebView 渲染的前端**。与 Electron 的区别：不打包整个 Chromium，Windows 上复用系统自带的 WebView2（Edge 内核），安装包小一个数量级、内存占用低。前后端通过 **IPC 命令**（前端 `invoke()` → Rust `#[tauri::command]`）和 **事件**（Rust `app.emit()` → 前端 `listen()`）通信。

### 6.2 前端：React 19 + TypeScript + Vite + Tailwind CSS 4

- **React**：组件化 UI，本应用核心状态只有四块（串口连接、键位网格、编码器、日志），全部用 `useState` 管理，无额外状态库
- **TypeScript**：`parseEventLine()` 对 `[EVT]` 行做类型化解析（可辨识联合类型），拼错字段名在编译期就报错
- **Vite**：开发服务器 + 打包器，提供热重载（改前端代码无需重编 Rust）
- **Tailwind CSS**：原子化 CSS，浅色主题（VIA 风格三栏布局）直接用 class 组合，无需手写样式表
- **UI 参考**：VIA（the-via/via，键盘配置器事实标准）的三栏布局——左侧功能面板 / 中间键位可视化 / 底部日志

### 6.3 后端：Rust + serialport + btleplug

- **serialport crate**：跨平台串口库，本应用用它完成端口枚举（`available_ports()`）、打开配置（115200/8N1/无流控）、读写
- **单属主读线程 + 命令通道**：`serialport` 在 Windows 上 `try_clone()` 并发读不可靠，因此端口句柄只归读线程所有；写命令经 mpsc 通道投递（`SendLine`/`Stop`），读线程每轮循环（50ms 超时）非阻塞取出写入。关闭 = 投递 Stop + drop 发送端，读线程自然退出并释放端口；打开端口带 3 次重试应对释放延迟
- **DTR 置位**：ESP32-S3 USB-Serial-JTAG 要求主机拉 DTR 才输出数据，且**绝不能拉 RTS**（RTS 参与芯片复位，DTR+RTS 同置会把芯片按进复位状态，详见排障手册 §3.1）
- **BLE 监控（btleplug + WinRT 双路径）**：优先 btleplug 扫描直连订阅 0xFFE1；设备已被系统连接（停止广播）时走 WinRT `DeviceInformation` 枚举直开 GATT（共享模式，与系统 HID 共存）。通知解析为 `[EVT]` 文本行统一转发前端
- **WiFi 热点通路（P5 方案 A）**：`wifi_connect` 经 netsh 连热点（按 SSID 直连已有 profile，XML 导入仅兜底——netsh add profile 在部分机器有 0x80002 环境 bug，且输出为 GBK 不可解析）→ TCP `192.168.4.1:8266`；读线程与串口同构（cfg-frame 帧解析 + [EVT] 文本行）；Rust **SendInput 注入器**维护 6KRO 快照状态机（语义对齐固件 hid_manager），把 [EVT] 转成真实按键输入；cfg_send 优先级 串口 → NET → BLE
- **为何不用 tauri-plugin-serialplugin**：需求只有「枚举/开关/按行读写」三件事，直接用 serialport 一百多行搞定，少一层抽象少一堆依赖

### 6.4 键盘侧知识（理解界面为什么这么设计）

- **矩阵扫描**：键盘 19 键走 4 列×5 行扫描矩阵（列驱动/行读取），固件硬件定时器扫描 + 20ms 软件防抖。上位机网格的 `(row,col)` 与矩阵物理坐标一一对应，所以按键没反应时能立刻看出是「矩阵没扫到」还是「键位映射为空」（`KC_NONE` 不发 HID 但矩阵事件仍上报）
- **三模 HID**：USB（TinyUSB）/ BLE（NimBLE）/ 2.4G（ESP-NOW+Dongle）三种通路发同一个 HID 报告；上位机经串口「旁路」观察，与当前模式无关，任何模式下都能测矩阵
- **[EVT] 与日志分离**：ESP-IDF 日志给人看，`[EVT]` 给机器解析——这也是为什么协议文档规定上位机禁止从日志文本正则抠事件

## 7. 目录结构

```
host-app/
├── README.md                 本手册
├── package.json              前端依赖与脚本
├── vite.config.ts            Vite 配置（含 watcher 排除 Rust 目录）
├── index.html                应用入口 HTML
├── make-icon.ps1             图标生成脚本（PowerShell System.Drawing）
├── fix-encoding.ps1          编码检查修复脚本（GBK→UTF-8）
├── src/                      前端源码（React）
│   ├── main.tsx              入口
│   ├── App.tsx               全部界面（顶栏/命令区/键位网格/日志台）
│   └── index.css             全局样式
└── src-tauri/                Tauri 后端（Rust）
    ├── Cargo.toml            Rust 依赖（tauri / serialport / serde）
    ├── tauri.conf.json       Tauri 配置（窗口/打包/开发命令）
    ├── icons/                应用图标（由 make-icon.ps1 + tauri icon 生成）
    └── src/main.rs           串口管理 + BLE 监控 + WiFi 热点通路（TCP/netsh/SendInput 注入器）+ 事件转发（list_ports/open_port/close_port/send_line/ble_scan_connect/ble_disconnect/wifi_connect/wifi_disconnect/cfg_send/cfg_ping/profile_*/power_get）
```

## 8. 常见问题（FAQ）

| 现象 | 原因与解决 |
|------|-----------|
| 端口下拉框没有 COM 口 | 键盘没插好/驱动没装；点「刷新」；确认设备管理器里有端口 |
| 连接失败：拒绝访问 | 串口被占用——关闭其它串口监视器（idf.py monitor、串口助手） |
| 连接成功但没日志 | ① 上位机已自动拉 DTR，若仍无数据，用串口助手对照测试隔离问题；② 波特率必须 115200；③ 确认固件已烧录且正常运行。分段定位流程见排障手册 §4.1 |
| 按键网格不响应 | 固件需含 `[EVT]` 输出；看日志台有没有 `[EVT] KEY` 行；串口有 `[EVT]` 但网格不动 → 看启动自检行是否出现（排障手册 §3.8） |
| 蓝牙监控失败：未找到 SimpleKey | 键盘需先与电脑配对；改过固件 GATT 结构需删除配对重新配对（Windows 缓存 GATT 数据库） |
| 蓝牙监控失败：未找到 0xFFE1 | 固件版本旧（无自定义监控服务），重烧最新固件 |
| 连接 WiFi 失败：导入 WLAN profile 失败 | 本机 netsh add profile 有 0x80002 环境 bug——上位机已改为按 SSID 直连已有 profile；若仍失败，先在系统 WLAN 列表手动连一次热点记住密码，再点上位机连接（详见排障手册 §3.18） |
| 连上热点后按键没输出 | 正常：WiFi 模式键盘不发 HID，需上位机「连接 WiFi」启动 SendInput 注入；仍无输出按排障手册 §3.17 三段证据排查（net evt → SendInput sync → 焦点窗口）；`SendInput sync: n<N` = UIPI 权限墙（管理员窗口/反作弊会吞注入） |
| WiFi 模式下键位网格读不到/改不了 | 确认已点「连接 WiFi」（自动握手后前端 cfgConnected 三通路判断生效）；后端日志 `cfg-frame rx` 正常而界面不动 → 前端 gate 问题（排障手册 §3.19） |
| esptool 烧录报 Invalid head of packet | 串口被占用（上位机/串口工具），断开后再烧录 |
| 日志中文乱码 | 固件输出是 UTF-8；若乱码说明固件源文件被 GBK 保存过——用 `fix-encoding.ps1` 思路修复源码后重编固件 |
| `cargo` not found | 新开终端，或手动把 `%USERPROFILE%\.cargo\bin` 加入 PATH |
| Vite EBUSY 崩溃 | `vite.config.ts` 的 `server.watch.ignored` 配置被删了，恢复它 |
| 编辑器保存后界面文字变乱码 | 编辑器默认编码必须是 **UTF-8**（本仓库纪律，见 AGENTS.md 对外输出编码纪律） |

## 9. 后续路线（Backlog）

- [x] 键位编辑器（v0.2 已完成：Bottom Sheet 键码选择器 + `keymap set/save` 下发与持久化 + 连接自动回显）
- [x] 配置二进制协议（AA 55 帧 + CRC16，协议 §7；串口/BLE 双通路实测通过，`cfg_send` + `cfg-frame` 事件）
- [x] Profile 管理（v1.8：3 槽位点击切换/名称重命名/导出导入 JSON，帧协议 0x06~0x08，2026-09-21 实测通过）
- [x] 电量监测曲线（v1.8：POWER_GET 0x09 五秒轮询，状态条 + 120 点电压曲线；固件 power_mgr，2026-09-21 实测通过）
- [x] WiFi 热点通路（v0.3/P5 方案 A：连接热点 + TCP + SendInput 注入 + 自动握手，三通路 cfg_send；2026-09-22 游戏反作弊兼容实测通过，P5 关闭）
- [x] 宏功能 + 媒体键码 + 旋钮可配（v0.4/P6：宏编辑器 4 槽位步骤化 + 媒体/宏键码分组 + 旋钮配置卡 + 右栏属性面板；帧协议 0x0A~0x0D；2026-09-25 实测通过）
- [x] 宏录制（v0.5：帧协议 0x0E 录制控制 + 「● 录制」开关/时长上限/倒计时 + [EVT] KEY 收集回填草稿，双通路在线 50ms 去重；2026-09-25 实测通过）
- [ ] 键码目录扩展（鼠标键/多层功能 `MO(n)`/`LT(n,k)`；媒体键 v0.4 已完成）
- [ ] BLE 槽位管理（`ble slot/clear` 图形化）
