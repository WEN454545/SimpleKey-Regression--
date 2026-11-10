//! SimpleKey 三模键盘调试上位机 - Tauri 后端
//!
//! 职责：串口管理（枚举/开关/读写）+ 串口数据实时转发到前端
//! 协议：纯文本，见 docs/串口命令协议.md（UTF-8）
//!
//! 线程模型（单属主 + 命令通道，规避 serialport Windows try_clone 读不可靠问题）：
//! - 端口句柄只归读线程所有，读取循环内同时处理「写命令请求」
//! - send_line 仅向通道投递，读线程每轮循环（<=50ms）取出并写入端口
//! - close_port 投递 Stop 指令并 drop 发送端，读线程退出并释放端口

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use serialport::{DataBits, FlowControl, Parity, StopBits};
use std::io::{Read, Write};
use std::sync::mpsc::{self, Sender};
use std::sync::Mutex;
use std::time::Duration;
use tauri::{AppHandle, Emitter, State};

// ==================== BLE 监控 ====================
//
// Windows 将 HID 服务（0x1812）独占给系统 HID 驱动，第三方应用订阅其 Input Report
// 会得到 GattOpenStatus=AccessDenied，因此固件提供独立的自定义监控服务：
//   Service 0xFFE0 / Characteristic 0xFFE1（Notify，二进制事件，帧长 ≤5 字节）
//   [0x01, down, row, col, keycode] 按键 / [0x02/0x03, delta] ENC CW/CCW
//   [0x04..0x06] ENC PRESS/RELEASE/LONG / [0x07, mode] 模式 / [0x08, profile] Profile

use btleplug::api::{Central, Characteristic, Manager as _, Peripheral as _, ScanFilter};
use btleplug::platform::{Adapter, Peripheral};
use futures::stream::StreamExt;
use uuid::Uuid;
use windows::core::GUID;
use windows::Devices::Bluetooth::BluetoothLEDevice;
use windows::Devices::Bluetooth::GenericAttributeProfile::{
    GattCharacteristic, GattCharacteristicProperties, GattSharingMode,
    GattClientCharacteristicConfigurationDescriptorValue, GattValueChangedEventArgs,
};
use windows::Devices::Enumeration::DeviceInformation;
use windows::Foundation::TypedEventHandler;
use windows::Storage::Streams::{DataReader, DataWriter};


fn hex_dump(data: &[u8]) -> String {
    data.iter().map(|b| format!("{b:02X}")).collect::<Vec<_>>().join(" ")
}

// ==================== AA 55 二进制配置帧协议 ====================
//
// 帧格式：AA 55 | CMD(1B) | SEQ(2B LE) | LEN(2B LE) | PAYLOAD(LEN B) | CRC16(2B LE)
// - CRC16：CRC-16/CCITT-FALSE（poly 0x1021，初值 0xFFFF），范围 CMD..PAYLOAD
// - SEQ：命令方向单调递增（模 65536）；设备端滑动窗口 ±16 去重；
//   上位机连接后必须先发 PING（0x01）完成重同步
// - 应答帧 CMD|0x80 回显请求 SEQ
// - 与文本行共存于同一串口流：读线程逐字节分流（帧状态机 vs 文本行缓冲）
// - 详见 docs/串口命令协议.md §7

// 命令字（上位机 -> 键盘）
pub const CFG_CMD_PING: u8 = 0x01;
pub const CFG_CMD_VERSION: u8 = 0x02;
pub const CFG_CMD_KEYMAP_GET: u8 = 0x03;
pub const CFG_CMD_KEYMAP_SET: u8 = 0x04;
pub const CFG_CMD_KEYMAP_SAVE: u8 = 0x05;

// P6：MACRO_GET/SET payload = idx(1B) + macro_t(137B) = 138B，上限对齐固件 cfg_frame.h 160
const CFG_MAX_PAYLOAD: usize = 160;

// SEQ 发号器：命令方向单调递增
static CFG_SEQ: std::sync::atomic::AtomicU16 = std::sync::atomic::AtomicU16::new(0);

/// CRC-16/CCITT-FALSE（poly 0x1021，初值 0xFFFF，不反转）
fn crc16_ccitt(data: &[u8]) -> u16 {
    let mut crc: u16 = 0xFFFF;
    for &b in data {
        crc ^= (b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x1021 } else { crc << 1 };
        }
    }
    crc
}

/// 构建一帧完整报文
fn build_frame(cmd: u8, seq: u16, payload: &[u8]) -> Vec<u8> {
    let mut buf = Vec::with_capacity(9 + payload.len());
    buf.extend_from_slice(&[0xAA, 0x55, cmd]);
    buf.extend_from_slice(&seq.to_le_bytes());
    buf.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    buf.extend_from_slice(payload);
    let crc = crc16_ccitt(&buf[2..]);
    buf.extend_from_slice(&crc.to_le_bytes());
    buf
}

/// 解析十六进制字符串（容忍空格与 0x 前缀），长度必须为偶数
fn parse_hex(s: &str) -> Result<Vec<u8>, String> {
    let cleaned: String = s
        .split_whitespace()
        .map(|p| p.trim_start_matches("0x").trim_start_matches("0X"))
        .collect();
    if cleaned.len() % 2 != 0 {
        return Err("十六进制长度必须为偶数".into());
    }
    (0..cleaned.len() / 2)
        .map(|i| u8::from_str_radix(&cleaned[i * 2..i * 2 + 2], 16).map_err(|e| e.to_string()))
        .collect()
}

/// 帧状态机：与固件 cfg_frame.c 同一套字节流规则
struct FrameParser {
    buf: Vec<u8>,
    in_frame: bool,
}

enum FrameOutcome {
    /// 字节被帧解析器消费
    Consumed(Option<Result<(u8, u16, Vec<u8>), String>>),
    /// 字节属于文本流
    Text,
}

impl FrameParser {
    fn new() -> Self {
        FrameParser { buf: Vec::new(), in_frame: false }
    }

    fn feed(&mut self, b: u8) -> FrameOutcome {
        if !self.in_frame {
            if b == 0xAA {
                self.in_frame = true;
                self.buf.clear();
                self.buf.push(b);
                FrameOutcome::Consumed(None)
            } else {
                FrameOutcome::Text
            }
        } else if self.buf.len() == 1 {
            // 等待第二个帧头字节 0x55
            if b == 0x55 {
                self.buf.push(b);
                FrameOutcome::Consumed(None)
            } else if b == 0xAA {
                FrameOutcome::Consumed(None) // 0xAA 0xAA：继续等 0x55
            } else {
                self.in_frame = false; // 伪帧头，丢弃
                FrameOutcome::Consumed(None)
            }
        } else {
            self.buf.push(b);
            // 收齐固定字段（帧头2 + CMD1 + SEQ2 + LEN2）后计算总长
            if self.buf.len() == 7 {
                let payload_len =
                    self.buf[5] as usize | ((self.buf[6] as usize) << 8);
                if payload_len > CFG_MAX_PAYLOAD {
                    self.in_frame = false;
                    return FrameOutcome::Consumed(None);
                }
            }
            if self.buf.len() >= 7 {
                let payload_len =
                    self.buf[5] as usize | ((self.buf[6] as usize) << 8);
                let total = 7 + payload_len + 2;
                if self.buf.len() == total {
                    self.in_frame = false;
                    let cmd = self.buf[2];
                    let seq = u16::from_le_bytes([self.buf[3], self.buf[4]]);
                    let crc_calc = crc16_ccitt(&self.buf[2..total - 2]);
                    let crc_recv = u16::from_le_bytes([
                        self.buf[total - 2],
                        self.buf[total - 1],
                    ]);
                    if crc_calc != crc_recv {
                        return FrameOutcome::Consumed(None);
                    }
                    let payload = self.buf[7..7 + payload_len].to_vec();
                    return FrameOutcome::Consumed(Some(Ok((cmd, seq, payload))));
                }
            }
            FrameOutcome::Consumed(None)
        }
    }
}

#[derive(Serialize, Clone)]
struct CfgFrameEvent {
    cmd: u8,
    seq: u16,
    payload_hex: String,
}

// BLE 通知流的配置帧解析器：应答帧可能被拆成多个 notify 到达，需持久状态。
// 监控数据（[EVT] 二进制事件）经同一状态机时会以 Text 结果透出，互不干扰
static BLE_FRAME_PARSER: Mutex<FrameParser> = Mutex::new(FrameParser {
    buf: Vec::new(),
    in_frame: false,
});

// 监控特征值（btleplug Uuid，自定义监控服务 0xFFE0 下的 0xFFE1）
const MONITOR_CHAR_UUID: Uuid = Uuid::from_u128(0x0000ffe1_0000_1000_8000_00805f9b34fb);

// WinRT 版监控服务/特征值 UUID（GUID）
const MONITOR_SERVICE_GUID: GUID = GUID::from_u128(0x0000ffe0_0000_1000_8000_00805f9b34fb);
const MONITOR_CHAR_GUID: GUID = GUID::from_u128(0x0000ffe1_0000_1000_8000_00805f9b34fb);

/// WinRT fallback：设备已连接系统后停止广播，btleplug 扫描不到。
/// 通过 DeviceInformation 枚举系统已配对/已连接的 BLE 设备，直接打开 GATT 订阅
/// （与系统 HID 共享同一条蓝牙连接，互不干扰）。
fn winrt_find_and_subscribe(app: AppHandle) -> Result<(BluetoothLEDevice, Option<GattCharacteristic>), String> {
    // 枚举系统已知的所有 BLE 设备（含已连接/已配对但不广播的）
    let selector = BluetoothLEDevice::GetDeviceSelector().map_err(|e| e.to_string())?;
    let coll = DeviceInformation::FindAllAsyncAqsFilter(&selector)
        .map_err(|e| e.to_string())?
        .get()
        .map_err(|e| e.to_string())?;
    let mut target: Option<DeviceInformation> = None;
    for i in 0..coll.Size().map_err(|e| e.to_string())? {
        let info = coll.GetAt(i).map_err(|e| e.to_string())?;
        let name = info.Name().map_err(|e| e.to_string())?.to_string();
        if name.starts_with("SimpleKey") {
            target = Some(info);
            break;
        }
    }
    let info = target.ok_or_else(|| "系统蓝牙设备列表中未找到 SimpleKey（请确认已与电脑配对）".to_string())?;

    let device = BluetoothLEDevice::FromIdAsync(&info.Id().map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?
        .get()
        .map_err(|e| format!("打开 BLE 设备失败: {}", e))?;

    // 定位自定义监控服务 (0xFFE0) 下的监控特征值 (0xFFE1)
    let svc_result = device
        .GetGattServicesForUuidAsync(MONITOR_SERVICE_GUID)
        .map_err(|e| e.to_string())?
        .get()
        .map_err(|e| e.to_string())?;
    if svc_result.Status().map_err(|e| e.to_string())?.0 != 0 {
        return Err("HID 服务查询失败（设备可能已断开）".into());
    }
    let service = svc_result
        .Services()
        .map_err(|e| e.to_string())?
        .GetAt(0)
        .map_err(|e| e.to_string())?;

    // 系统驱动可能持有部分服务，尝试以共享模式打开；对自定义监控服务失败不阻断
    if let Ok(open) = service
        .OpenAsync(GattSharingMode::SharedReadAndWrite)
        .and_then(|o| o.get())
    {
        if open.0 > 2 {
            return Err(format!("监控服务打开失败（GattOpenStatus={}）", open.0));
        }
    }

    // 查询监控特征值（0xFFE1）；为空则兜底枚举服务下全部特征值
    let mut chars_vec: Vec<GattCharacteristic> = Vec::new();
    let mut last_status: i32 = -1;
    if let Ok(chr_result) = service
        .GetCharacteristicsForUuidAsync(MONITOR_CHAR_GUID)
        .and_then(|o| o.get())
    {
        last_status = chr_result.Status().map(|s| s.0).unwrap_or(-1);
        if let Ok(v) = chr_result.Characteristics() {
            for i in 0..v.Size().unwrap_or(0) {
                if let Ok(c) = v.GetAt(i) {
                    chars_vec.push(c);
                }
            }
        }
    }
    if chars_vec.is_empty() {
        if let Ok(chr_result) = service
            .GetCharacteristicsAsync()
            .and_then(|o| o.get())
        {
            last_status = chr_result.Status().map(|s| s.0).unwrap_or(-1);
            if let Ok(v) = chr_result.Characteristics() {
                for i in 0..v.Size().unwrap_or(0) {
                    if let Ok(c) = v.GetAt(i) {
                        chars_vec.push(c);
                    }
                }
            }
        }
    }
    if chars_vec.is_empty() {
        return Err(format!(
            "HID 服务下未找到任何特征值（GattCommunicationStatus={}）",
            last_status
        ));
    }

    // 订阅所有可 Notify 的报告特征值（键盘 + Consumer）；同时记录可写的 0xFFE1 特征值
    let mut subscribed = 0usize;
    let mut write_char: Option<GattCharacteristic> = None;
    for c in &chars_vec {
        let props = c.CharacteristicProperties().map_err(|e| e.to_string())?;
        let writable = props.0 & (GattCharacteristicProperties::Write.0
            | GattCharacteristicProperties::WriteWithoutResponse.0)
            != 0;
        if writable && write_char.is_none() {
            write_char = Some(c.clone());
        }
        if props.0 & GattCharacteristicProperties::Notify.0 == 0 {
            continue;
        }
        // 通知回调：解析报告并转发前端
        let app2 = app.clone();
        let handler = TypedEventHandler::<GattCharacteristic, GattValueChangedEventArgs>::new(
            move |_, args| {
                if let Some(args) = args {
                    if let Ok(buf) = args.CharacteristicValue() {
                        let len = buf.Length().unwrap_or(0) as usize;
                        let mut data = vec![0u8; len];
                        if let Ok(reader) = DataReader::FromBuffer(&buf) {
                            if reader.ReadBytes(&mut data).is_ok() {
                                handle_monitor(&app2, &data);
                            }
                        }
                    }
                }
                Ok(())
            },
        );
        c.ValueChanged(&handler).map_err(|e| e.to_string())?;
        // 使能 CCCD（Notify）
        c.WriteClientCharacteristicConfigurationDescriptorAsync(
            GattClientCharacteristicConfigurationDescriptorValue::Notify,
        )
        .map_err(|e| e.to_string())?
        .get()
        .map_err(|e| e.to_string())?;
        subscribed += 1;
    }
    if subscribed == 0 {
        return Err(format!(
            "找到 {} 个特征值但无可订阅的 Notify 报告特征值（固件报告描述符可能未更新，请烧录最新固件）",
            chars_vec.len()
        ));
    }
    Ok((device, write_char))
}

#[derive(Default)]
struct BleState {
    peripheral: Mutex<Option<Peripheral>>,
    // WinRT 直连句柄（设备已连接系统、不广播时的 fallback 订阅通道）
    winrt_device: Mutex<Option<BluetoothLEDevice>>,
    // WinRT 0xFFE1 可写特征值（BLE 配置帧下行用；None 表示不可写）
    winrt_char: Mutex<Option<GattCharacteristic>>,
}

/// 解析监控事件（二进制）并转为 [EVT] 文本行转发前端（前端与串口共用一套解析）。
/// AA 55 配置帧应答（BLE 配置通路上行）先经帧状态机解析后以 cfg-frame 事件转发
fn handle_monitor(app: &AppHandle, data: &[u8]) {
    // 配置帧优先：喂全局帧解析器（应答可能拆包，状态需跨 notify 持久）
    let mut frame_ev: Option<CfgFrameEvent> = None;
    {
        let mut parser = BLE_FRAME_PARSER.lock().unwrap();
        for &b in data {
            if let FrameOutcome::Consumed(Some(Ok((cmd, seq, payload)))) = parser.feed(b) {
                frame_ev = Some(CfgFrameEvent {
                    cmd,
                    seq,
                    payload_hex: hex_dump(&payload),
                });
            }
        }
    }
    if let Some(ev) = frame_ev {
        let _ = app.emit("cfg-frame", ev);
        return;
    }

    let line = match data.first() {
        Some(&0x01) if data.len() >= 5 => format!(
            "[EVT] KEY {} {} {} 0x{:02X}",
            if data[1] == 1 { "DOWN" } else { "UP" },
            data[2], data[3], data[4]
        ),
        Some(&0x02) if data.len() >= 2 => format!("[EVT] ENC CW {}", data[1]),
        Some(&0x03) if data.len() >= 2 => format!("[EVT] ENC CCW {}", data[1]),
        Some(&0x04) => "[EVT] ENC PRESS".to_string(),
        Some(&0x05) => "[EVT] ENC RELEASE".to_string(),
        Some(&0x06) => "[EVT] ENC LONG".to_string(),
        Some(&0x07) if data.len() >= 2 => format!(
            "[EVT] MODE {}",
            match data[1] {
                0 => "usb",
                1 => "ble",
                _ => "wifi",
            }
        ),
        Some(&0x08) if data.len() >= 2 => format!("[EVT] PROFILE {}", data[1]),
        _ => return,
    };
    let _ = app.emit("ble-evt", line);
}

#[tauri::command]
async fn ble_scan_connect(app: AppHandle, state: State<'_, BleState>) -> Result<String, String> {
    // 已连接则直接返回（先 clone 出句柄再检查，避免 MutexGuard 跨 await）
    let existing = state.peripheral.lock().unwrap().clone();
    if let Some(p) = existing {
        if p.is_connected().await.map_err(|e| e.to_string())? {
            return Ok("already".into());
        }
    }

    let manager = btleplug::platform::Manager::new().await.map_err(|e| e.to_string())?;
    let adapters = manager.adapters().await.map_err(|e| e.to_string())?;
    let adapter: Adapter = adapters
        .into_iter()
        .next()
        .ok_or_else(|| "未找到蓝牙适配器".to_string())?;

    adapter
        .start_scan(ScanFilter::default())
        .await
        .map_err(|e| e.to_string())?;
    let _ = app.emit("ble-status", "scanning");

    // 扫描最多 3 秒，寻找 SimpleKey 设备；未发现则走 WinRT 直连
    // （已连接系统的设备不广播，扫描注定找不到，因此扫描时间不宜过长）
    let mut target: Option<Peripheral> = None;
    for _ in 0..6 {
        for p in adapter.peripherals().await.map_err(|e| e.to_string())? {
            if let Ok(Some(props)) = p.properties().await {
                let name = props.local_name.unwrap_or_default();
                if name.starts_with("SimpleKey") {
                    target = Some(p);
                    break;
                }
            }
        }
        if target.is_some() {
            break;
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
    adapter.stop_scan().await.ok();
    let peripheral = match target {
        Some(p) => {
            p
        }
        None => {
            // 扫描未发现：设备可能已连接系统（连接后停止广播）。
            // 改用 WinRT 设备枚举直连订阅（与系统 HID 共享连接），在阻塞线程执行
            let app2 = app.clone();
            let device = tokio::task::spawn_blocking(move || winrt_find_and_subscribe(app2))
                .await
                .map_err(|e| format!("WinRT 调度失败: {}", e))?;
            match device {
                Ok((d, ch)) => {
                    *state.winrt_device.lock().unwrap() = Some(d);
                    *state.winrt_char.lock().unwrap() = ch;
                    let _ = app.emit("ble-status", "connected");
                    return Ok("connected".into());
                }
                Err(e) => {
                    return Err(format!("未发现 SimpleKey 设备（{}）", e));
                }
            }
        }
    };

    // 未连接则主动连接（已由系统连上时会立即成功）
    if !peripheral.is_connected().await.map_err(|e| e.to_string())? {
        peripheral.connect().await.map_err(|e| format!("连接失败: {}", e))?;
    }
    peripheral.discover_services().await.map_err(|e| e.to_string())?;

    // 订阅监控特征值（0xFFE1，自定义监控服务 0xFFE0）
    let chars: Vec<Characteristic> = peripheral
        .characteristics()
        .into_iter()
        .filter(|c| {
            c.uuid == MONITOR_CHAR_UUID
                && c.properties.contains(btleplug::api::CharPropFlags::NOTIFY)
        })
        .collect();
    if chars.is_empty() {
        return Err("设备上未找到监控特征值（0xFFE1，请烧录最新固件）".into());
    }
    for c in &chars {
        peripheral.subscribe(c).await.map_err(|e| e.to_string())?;
    }

    // 通知转发线程
    let app2 = app.clone();
    let notif_chars = chars.clone();
    let notify_peripheral = peripheral.clone();
    std::thread::spawn(move || {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("tokio runtime");
        rt.block_on(async move {
            let mut stream = match notify_peripheral.notifications().await {
                Ok(s) => s,
                Err(_) => return,
            };
            while let Some(note) = stream.next().await {
                if notif_chars.iter().any(|c| c.uuid == note.uuid) {
                    handle_monitor(&app2, &note.value);
                }
            }
        });
    });

    *state.peripheral.lock().unwrap() = Some(peripheral);
    let _ = app.emit("ble-status", "connected");
    Ok("connected".into())
}

#[tauri::command]
async fn ble_disconnect(state: State<'_, BleState>) -> Result<(), String> {
    // 先在锁作用域内取出句柄，避免 MutexGuard 跨 await（future 需 Send）
    let p = state.peripheral.lock().unwrap().take();
    if let Some(p) = p {
        // 通知流随连接断开自然结束；同时不影响系统 HID 连接（WinRT 层共享连接）
        p.disconnect().await.map_err(|e| e.to_string())?;
    }
    // WinRT fallback 通道：关闭设备句柄即取消订阅
    let d = state.winrt_device.lock().unwrap().take();
    if let Some(d) = d {
        let _ = d.Close();
    }
    *state.winrt_char.lock().unwrap() = None;
    Ok(())
}

// ==================== WiFi 热点通路（P5 方案 A） ====================
//
// 键盘 WiFi 模式开 SoftAP（SimpleKey-XXXX，键盘 IP 固定 192.168.4.1），上位机：
//   1) netsh wlan 生成/复用 WLAN profile 并连接热点
//   2) TCP 连接 192.168.4.1:8266（固件 net_input.c）
//   3) 下行 AA 55 配置帧（固件 cfg_frame NET 传输），上行 [EVT] 文本行
// 热点对 Windows 是网络设备非 HID，上行 [EVT] 由 SendInput 注入器转为真实输入。

/// 读线程命令：写原始帧字节 / 停止（TCP 通路无文本命令）
enum NetCmd {
    WriteRaw(Vec<u8>),
    Stop,
}

/// WiFi 通路状态（前端展示用）
#[derive(Serialize, Clone)]
struct WifiStatus {
    tcp_connected: bool,
}

// ==================== SendInput 注入器 ====================
//
// 语义对齐固件 hid_manager（项目记忆硬约束）：
// - 维护「当前按住键集合」状态机（8 位修饰键位域 + 6 个普通键槽）
// - 事件增删后只对差量发 SendInput（新按下的发 KEYDOWN，新松开的发 KEYUP）
// - 无变化不注入；切模式/断开时清空并补发全量 KEYUP 防卡键

mod injector {
    use std::collections::BTreeSet;
    use windows::Win32::UI::Input::KeyboardAndMouse::{
        SendInput, INPUT, INPUT_0, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP, VIRTUAL_KEY,
    };

    pub const KEY_MAX_SLOTS: usize = 6;

    pub struct Injector {
        pub modifiers: u8,
        pub keys: [u8; KEY_MAX_SLOTS],      // 当前按住的 HID 键码（0=空槽）
        injected: BTreeSet<u16>,            // 已注入（按下态）的 VK
    }

    impl Injector {
        pub fn new() -> Self {
            Injector { modifiers: 0, keys: [0u8; KEY_MAX_SLOTS], injected: BTreeSet::new() }
        }

        /// 清空按键集并补发全量 KEYUP（模式切换/断开时防卡键）
        pub fn reset(&mut self) {
            self.modifiers = 0;
            self.keys = [0u8; KEY_MAX_SLOTS];
            self.sync();
        }

        /// 应用一次按键事件（HID 键码；0xE0~0xE7 为修饰键）
        pub fn apply(&mut self, keycode: u8, pressed: bool) {
            if (0xE0..=0xE7).contains(&keycode) {
                let bit = 1u8 << (keycode - 0xE0);
                if pressed {
                    self.modifiers |= bit;
                } else {
                    self.modifiers &= !bit;
                }
            } else if keycode != 0x00 {
                if pressed {
                    if !self.keys.contains(&keycode) {
                        if let Some(slot) = self.keys.iter().position(|&k| k == 0) {
                            self.keys[slot] = keycode;
                        }
                        // 6 槽满：与固件一致丢弃（SendInput 无硬性上限，但保持语义一致）
                    }
                } else {
                    for k in self.keys.iter_mut() {
                        if *k == keycode {
                            *k = 0;
                        }
                    }
                }
            }
            self.sync();
        }

        /// 把按键集合差量注入系统（新按下 KEYDOWN / 新松开 KEYUP，无变化不发）
        fn sync(&mut self) {
            let mut want: BTreeSet<u16> = BTreeSet::new();
            for bit in 0..8u8 {
                if self.modifiers & (1 << bit) != 0 {
                    want.insert(modifier_vk(0xE0 + bit));
                }
            }
            for &kc in &self.keys {
                if kc != 0 {
                    if let Some(vk) = hid_to_vk(kc) {
                        want.insert(vk);
                    }
                }
            }

            let mut inputs: Vec<INPUT> = Vec::new();
            for &vk in want.difference(&self.injected) {
                inputs.push(kb_input(vk, false));
            }
            for &vk in self.injected.difference(&want) {
                inputs.push(kb_input(vk, true));
            }
            self.injected = want;

            if !inputs.is_empty() {
                let _n = unsafe {
                    SendInput(&mut inputs, std::mem::size_of::<INPUT>() as i32)
                };
                // n < 提交数 = 注入被系统拒绝（UIPI 权限墙/前台窗口拒绝），需证据定位
            }
        }
    }

    fn kb_input(vk: u16, keyup: bool) -> INPUT {
        INPUT {
            r#type: INPUT_KEYBOARD,
            Anonymous: INPUT_0 {
                ki: KEYBDINPUT {
                    wVk: VIRTUAL_KEY(vk),
                    wScan: 0,
                    dwFlags: if keyup { KEYEVENTF_KEYUP } else { Default::default() },
                    time: 0,
                    dwExtraInfo: 0,
                },
            },
        }
    }

    /// 独立按键 tap（音量键等一次性注入：按下后立即抬起，重复 times 次）
    pub fn tap_key(vk: u16, times: u32) {
        let mut inputs: Vec<INPUT> = Vec::new();
        for _ in 0..times.min(10) {
            inputs.push(kb_input(vk, false));
            inputs.push(kb_input(vk, true));
        }
        if !inputs.is_empty() {
            let _n = unsafe {
                SendInput(&mut inputs, std::mem::size_of::<INPUT>() as i32)
            };
        }
    }

    /// HID 修饰键码 → VK（0xE0 起：LCtrl/LShift/LAlt/LGui/RCtrl/RShift/RAlt/RGui）
    fn modifier_vk(kc: u8) -> u16 {
        const MOD_VKS: [u16; 8] = [0xA2, 0xA0, 0xA4, 0x5B, 0xA3, 0xA1, 0xA5, 0x5C];
        MOD_VKS[(kc - 0xE0) as usize]
    }

    /// HID 键码 → Windows VK（覆盖键盘常用键；未映射返回 None 忽略）
    fn hid_to_vk(kc: u8) -> Option<u16> {
        Some(match kc {
            // 字母 A-Z
            0x04..=0x1D => b'A' as u16 + (kc - 0x04) as u16,
            // 数字 1-9,0
            0x1E..=0x26 => b'1' as u16 + (kc - 0x1E) as u16,
            0x27 => b'0' as u16,
            0x28 => 0x0D, // Enter
            0x29 => 0x1B, // Esc
            0x2A => 0x08, // Backspace
            0x2B => 0x09, // Tab
            0x2C => 0x20, // Space
            0x2D => 0xBD, // - _
            0x2E => 0xBB, // = +
            0x2F => 0xDB, // [ {
            0x30 => 0xDD, // ] }
            0x31 => 0xDC, // \ |
            0x32 => 0xBA, // ; :
            0x33 => 0xDE, // ' "
            0x34 => 0xC0, // ` ~
            0x35 => 0xBC, // , <
            0x36 => 0xBE, // . >
            0x37 => 0xBF, // / ?
            0x39 => 0x14, // Caps Lock
            0x3A..=0x45 => 0x70 + (kc - 0x3A) as u16, // F1-F12
            0x4F => 0x27, // Right
            0x50 => 0x25, // Left
            0x51 => 0x28, // Down
            0x52 => 0x26, // Up
            0x54 => 0x6F, // Keypad /
            0x55 => 0x6A, // Keypad *
            0x56 => 0x6D, // Keypad -
            0x57 => 0x6B, // Keypad +
            0x58 => 0x0D, // Keypad Enter
            0x59..=0x61 => 0x61 + (kc - 0x59) as u16, // Keypad 1-9
            0x62 => 0x60, // Keypad 0
            0x63 => 0x6E, // Keypad .
            0x65 => 0x22, // App/Menu -> Page Down 近似? 保留: 不常用，忽略
            _ => return None,
        })
    }
}

// 全局注入器（[EVT] 行只从 net 读线程到达，单线程访问；Mutex 兜底）
static NET_INJECTOR: Mutex<Option<injector::Injector>> = Mutex::new(None);

/// Consumer 功能名（固件 keymap.c key_names，[EVT] CC 行回显）→ Windows VK。
/// 亮度键 SendInput 无法注入（需 WMI/DDC 或 WinRT，暂不支持），返回 None 仅记日志。
fn consumer_vk(name: &str) -> Option<u16> {
    Some(match name {
        "VOL+" => 0xAF, // VK_VOLUME_UP
        "VOL-" => 0xAE, // VK_VOLUME_DOWN
        "MUTE" => 0xAD, // VK_VOLUME_MUTE
        "PLAY" => 0xB3, // VK_MEDIA_PLAY_PAUSE
        "NEXT" => 0xB0, // VK_MEDIA_NEXT_TRACK
        "PREV" => 0xB1, // VK_MEDIA_PREV_TRACK
        _ => return None,
    })
}

/// 解析一行 [EVT] 并驱动 SendInput 注入器（WiFi 通路输入主路径）
fn injector_handle_line(line: &str) {
    // [EVT] KEY DOWN/UP <row> <col> <keycode> <name>
    if let Some(rest) = line.strip_prefix("[EVT] KEY ") {
        let parts: Vec<&str> = rest.split_whitespace().collect();
        if parts.len() >= 4 {
            if let Ok(kc) = u8::from_str_radix(parts[3].trim_start_matches("0x"), 16) {
                let mut guard = NET_INJECTOR.lock().unwrap();
                let inj = guard.get_or_insert_with(injector::Injector::new);
                inj.apply(kc, parts[0] == "DOWN");
            }
        }
        return;
    }
    // [EVT] ENC CW/CCW <格数>：音量键注入（下/上成对）
    if let Some(rest) = line.strip_prefix("[EVT] ENC ") {
        let parts: Vec<&str> = rest.split_whitespace().collect();
        if parts.len() >= 2 {
            let vk = match parts[0] {
                "CW" => 0xAFu16,    // VK_VOLUME_UP
                "CCW" => 0xAEu16,   // VK_VOLUME_DOWN
                _ => return,
            };
            let n: u32 = parts[1].parse().unwrap_or(1);
            injector::tap_key(vk, n);
        }
        return;
    }
    // [EVT] CC <funcname>：Consumer 键按下（WiFi 模式经 SendInput 注入一次 tap）
    if let Some(rest) = line.strip_prefix("[EVT] CC ") {
        let name = rest.trim();
        match consumer_vk(name) {
            Some(vk) => injector::tap_key(vk, 1),
            None => {}
        }
        return;
    }
    // [EVT] MACRO KD/KU/DL/CC/END：固件宏引擎（WiFi 模式）推送的播放步骤，
    // 延迟由键盘端真实执行，此处按到达顺序即时注入即可保持时序
    if let Some(rest) = line.strip_prefix("[EVT] MACRO ") {
        let parts: Vec<&str> = rest.split_whitespace().collect();
        match (parts.first().copied(), parts.get(1).copied()) {
            // KD/KU <keycode>：普通键/修饰键按下与松开（走 6KRO 快照状态机）
            (Some("KD"), Some(kc_s)) | (Some("KU"), Some(kc_s)) => {
                if let Ok(kc) = u8::from_str_radix(kc_s.trim_start_matches("0x"), 16) {
                    let mut guard = NET_INJECTOR.lock().unwrap();
                    let inj = guard.get_or_insert_with(injector::Injector::new);
                    inj.apply(kc, parts[0] == "KD");
                }
            }
            // DL <ms>：延迟（NET 读线程阻塞 sleep，期间后续行自然排队）
            (Some("DL"), Some(ms_s)) => {
                let ms: u64 = ms_s.parse().unwrap_or(0);
                if ms > 0 {
                    std::thread::sleep(Duration::from_millis(ms.min(10_000)));
                }
            }
            // CC <funcname>：宏内的 Consumer 步骤（脉冲注入）
            (Some("CC"), Some(name)) => match consumer_vk(name) {
                Some(vk) => injector::tap_key(vk, 1),
                None => {}
            },
            // END：播放结束，逆序释放已在固件端通过 KU 行完成，此处清空兜底防卡键
            (Some("END"), _) => {
                if let Some(inj) = NET_INJECTOR.lock().unwrap().as_mut() {
                    inj.reset();
                }
            }
            _ => {}
        }
        return;
    }
    // 模式切换/断开等：清空按键集防卡键
    if line.starts_with("[EVT] ENC LONG") || line.starts_with("[EVT] MODE ") {
        if let Some(inj) = NET_INJECTOR.lock().unwrap().as_mut() {
            inj.reset();
        }
    }
}

/// 键盘热点 SSID（与固件 BLE_DEVICE_NAME 一致，见固件 tasks_config.h；UTF-8 中文）
const WIFI_DEFAULT_SSID: &str = "SimpleKey_绝希";
/// WLAN profile 名称（纯 ASCII——netsh 控制台 GBK 输出会毁掉中文 SSID，
/// profile 内 SSID 用 <hex> 字节编码，连接按 profile 名，完全绕开编码问题）
const WIFI_PROFILE_NAME: &str = "SimpleKeyWiFi";

/// 生成 WLAN profile XML 并导入（已存在同名 profile 则跳过）
fn wifi_ensure_profile(ssid: &str, psk: &str) -> Result<(), String> {
    let show = std::process::Command::new("netsh")
        .args(["wlan", "show", "profiles"])
        .output()
        .map_err(|e| format!("netsh 执行失败: {e}"))?;
    if String::from_utf8_lossy(&show.stdout).contains(WIFI_PROFILE_NAME) {
        return Ok(());
    }
    let hex_ssid: String = ssid.as_bytes().iter().map(|b| format!("{b:02X}")).collect();
    let xml = format!(
        r#"<?xml version="1.0" encoding="utf-8"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLANProfile/v1">
  <name>{WIFI_PROFILE_NAME}</name>
  <SSIDConfig><SSID><hex>{hex_ssid}</hex></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security>
    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>{psk}</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>"#
    );
    let path = std::env::temp_dir().join("simplekey_wlan_profile.xml");
    std::fs::write(&path, xml).map_err(|e| format!("写 profile 失败: {e}"))?;
    let add = std::process::Command::new("netsh")
        .args(["wlan", "add", "profile"])
        .arg(format!("filename={}", path.to_string_lossy()))
        .args(["user=current"])
        .output()
        .map_err(|e| format!("netsh 执行失败: {e}"))?;
    if !add.status.success() {
        return Err(format!(
            "导入 WLAN profile 失败: {}",
            String::from_utf8_lossy(&add.stderr).trim()
        ));
    }
    Ok(())
}

/// 连接键盘热点并建立 TCP 通路（阻塞，放阻塞线程池执行）
fn wifi_connect_blocking(app: AppHandle, ssid_hint: Option<String>, psk_hint: Option<String>) -> Result<String, String> {
    use std::net::TcpStream;

    // 1. 确定 SSID（默认固定名与蓝牙广播一致；中文 SSID 不走 netsh 扫描解析，避免 GBK 乱码）
    let ssid = match ssid_hint {
        Some(s) if !s.is_empty() => s,
        _ => WIFI_DEFAULT_SSID.to_string(),
    };
    let psk = psk_hint.unwrap_or_else(|| "simplekey".to_string());

    // 2. 优先按 SSID 名直连（Windows 手动连过一次就有同名 profile；netsh 参数走 UTF-16，中文没问题）。
    //    没有 profile 才走 XML 导入兜底（注意：部分 Windows 版本 netsh add profile 有 0x80002
    //    "WLAN profile element is missing" 兼容 bug，直连优先可完全绕开）
    let connect_by = |name: &str| -> Result<(), String> {
        let conn = std::process::Command::new("netsh")
            .args(["wlan", "connect"])
            .arg(format!("name={name}"))
            .output()
            .map_err(|e| format!("netsh 执行失败: {e}"))?;
        if !conn.status.success() {
            return Err(String::from_utf8_lossy(&conn.stderr).trim().to_string());
        }
        Ok(())
    };
    if connect_by(&ssid).is_err() {
        wifi_ensure_profile(&ssid, &psk)?;
        connect_by(WIFI_PROFILE_NAME)?;
    }

    // 3. 等待 TCP 可达（重试 20s；DHCP 获取地址需数秒）
    let mut stream: Option<TcpStream> = None;
    for i in 0..20 {
        match TcpStream::connect(("192.168.4.1", 8266)) {
            Ok(s) => {
                stream = Some(s);
                break;
            }
            Err(e) => {
                if i == 19 {
                    return Err(format!("连接键盘 TCP 失败（192.168.4.1:8266）: {e}"));
                }
                std::thread::sleep(Duration::from_millis(1000));
            }
        }
    }
    let stream = stream.unwrap();
    let _ = stream.set_nodelay(true);
    // 读超时：写帧处理嵌在读线程里，若无超时，键盘侧无上行流量（无 [EVT]）时 read()
    // 永久阻塞，发送队列里的下行帧全部饿死（2026-10-10「WiFi 模式配置零应答」根因）
    let _ = stream.set_read_timeout(Some(Duration::from_millis(100)));

    let _ = app.emit("wifi-status", WifiStatus { tcp_connected: true });

    // 4. 读线程：帧解析 + [EVT] 行（注入器 + serial-line 事件复用）
    let (tx, rx) = mpsc::channel::<NetCmd>();
    // 关闭旧线程
    *NET_SENDER.lock().unwrap() = None;
    *NET_SENDER.lock().unwrap() = Some(tx);

    std::thread::spawn(move || {
        let mut reader = stream;
        let mut frame_parser = FrameParser::new();
        let mut line: Vec<u8> = Vec::new();
        let mut stop = false;
        let mut byte_buf = [0u8; 512];
        while !stop {
            loop {
                match rx.try_recv() {
                    Ok(NetCmd::WriteRaw(data)) => {
                        use std::io::Write as _;
                        let _ = reader.write_all(&data);
                        let _ = reader.flush();
                    }
                    Ok(NetCmd::Stop) | Err(mpsc::TryRecvError::Disconnected) => {
                        stop = true;
                        break;
                    }
                    Err(mpsc::TryRecvError::Empty) => break,
                }
            }
            if stop {
                break;
            }
            match reader.read(&mut byte_buf) {
                Ok(0) => {}
                Ok(n) => {
                    for &b in &byte_buf[..n] {
                        match frame_parser.feed(b) {
                            FrameOutcome::Text => {
                                if b == b'\n' {
                                    if !line.is_empty() {
                                        if line.last() == Some(&b'\r') {
                                            line.pop();
                                        }
                                        let text = String::from_utf8_lossy(&line).into_owned();
                                        // 输入注入（WiFi 模式唯一输入路径）
                                        injector_handle_line(&text);
                                        let _ = app.emit("serial-line", text);
                                    }
                                    line.clear();
                                } else if line.len() < 256 {
                                    line.push(b);
                                }
                            }
                            FrameOutcome::Consumed(Some(Ok((cmd, seq, payload)))) => {
                                let _ = app.emit(
                                    "cfg-frame",
                                    CfgFrameEvent {
                                        cmd,
                                        seq,
                                        payload_hex: hex_dump(&payload),
                                    },
                                );
                            }
                            FrameOutcome::Consumed(_) => {}
                        }
                    }
                }
                Err(e)
                    if e.kind() == std::io::ErrorKind::WouldBlock
                        || e.kind() == std::io::ErrorKind::TimedOut =>
                {
                    // 读超时（100ms）：正常轮空，回到循环顶部先清空发送队列再继续读
                    continue;
                }
                Err(_) => {
                    break;
                }
            }
        }
        // 断开：清空注入器按键集防卡键
        if let Some(inj) = NET_INJECTOR.lock().unwrap().as_mut() {
            inj.reset();
        }
        *NET_SENDER.lock().unwrap() = None;
        let _ = app.emit("wifi-status", WifiStatus { tcp_connected: false });
    });

    Ok(ssid)
}

// NET 发送端句柄（cfg_send 下行 / wifi_disconnect 用）
static NET_SENDER: std::sync::Mutex<Option<Sender<NetCmd>>> = std::sync::Mutex::new(None);

/// 连接键盘热点（netsh + TCP），ssid/psk 省缺时自动扫描 + 默认密码 simplekey
#[tauri::command]
async fn wifi_connect(
    app: AppHandle,
    ssid: Option<String>,
    psk: Option<String>,
) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || wifi_connect_blocking(app, ssid, psk))
        .await
        .map_err(|e| format!("WiFi 连接线程失败: {e}"))?
}

/// 断开 TCP 通路（保持 WLAN 连接，方便重连）
#[tauri::command]
fn wifi_disconnect() -> Result<(), String> {
    if let Some(sender) = NET_SENDER.lock().unwrap().take() {
        let _ = sender.send(NetCmd::Stop);
    }
    Ok(())
}

// ==================== 串口管理 ====================

/// 读线程命令：写一行 / 写原始字节（二进制帧）/ 停止
enum PortCmd {
    SendLine(String),
    WriteRaw(Vec<u8>),
    Stop,
}

#[derive(Default)]
struct SerialState {
    sender: Mutex<Option<Sender<PortCmd>>>,
}

#[derive(Serialize)]
struct PortInfo {
    name: String,
    desc: String,
}

/// 枚举系统可用串口
#[tauri::command]
fn list_ports() -> Result<Vec<PortInfo>, String> {
    let ports = serialport::available_ports().map_err(|e| e.to_string())?;
    let mut result = Vec::new();
    for p in ports {
        let desc = match &p.port_type {
            serialport::SerialPortType::UsbPort(info) => info
                .product
                .clone()
                .unwrap_or_else(|| format!("USB {:04x}:{:04x}", info.vid, info.pid)),
            serialport::SerialPortType::BluetoothPort => "蓝牙串口".into(),
            _ => String::new(),
        };
        result.push(PortInfo { name: p.port_name, desc });
    }
    Ok(result)
}

/// 打开串口并启动读取线程（115200 8N1）
#[tauri::command]
fn open_port(app: AppHandle, state: State<SerialState>, name: String, baud: u32) -> Result<(), String> {
    // 停掉旧连接：投递 Stop 并断开发送端（读线程随后退出释放端口）
    *state.sender.lock().unwrap() = None;

    // 打开端口（带重试：旧读线程退出释放端口最多需要约一个超时周期）
    let mut port = None;
    for attempt in 1..=3 {
        match serialport::new(&name, baud)
            .data_bits(DataBits::Eight)
            .flow_control(FlowControl::None)
            .parity(Parity::None)
            .stop_bits(StopBits::One)
            .timeout(Duration::from_millis(50))
            .open()
        {
            Ok(p) => {
                port = Some(p);
                break;
            }
            Err(e) => {
                if attempt == 3 {
                    return Err(format!("打开 {} 失败: {}（若提示拒绝访问，请确认串口未被其它程序占用）", name, e));
                }
                std::thread::sleep(Duration::from_millis(200));
            }
        }
    }
    let mut port = port.unwrap();

    // ESP32-S3 USB-Serial-JTAG 控制台要求主机置位 DTR 才输出数据（终端软件默认会拉，
    // serialport 不会）——不拉的话固件输出会被驱动阻塞，表现为「收不到任何数据」。
    // 注意：只能拉 DTR，不能拉 RTS！USB-Serial-JTAG 的 RTS 参与芯片复位逻辑
    // （esptool 正是用 DTR/RTS 组合复位芯片），DTR+RTS 同时置位会把芯片
    // 按进复位状态，表现为「端口能打开但一个字节都收不到」。
    port.write_data_terminal_ready(true)
        .map_err(|e| format!("设置 DTR 失败: {}", e))?;

    // 建立命令通道，句柄交给读线程独占
    let (tx, rx) = mpsc::channel::<PortCmd>();
    *state.sender.lock().unwrap() = Some(tx);

    std::thread::spawn(move || {
        let mut byte_buf = [0u8; 256];
        let mut line = Vec::new();
        let mut frame_parser = FrameParser::new();
        let mut stop = false;
        loop {
            // 处理待发送命令 / 停止指令（非阻塞）
            loop {
                match rx.try_recv() {
                    Ok(PortCmd::SendLine(s)) => {
                        let data = format!("{}\r\n", s);
                        let _ = port.write_all(data.as_bytes());
                        let _ = port.flush();
                    }
                    Ok(PortCmd::WriteRaw(data)) => {
                        let _ = port.write_all(&data);
                        let _ = port.flush();
                    }
                    Ok(PortCmd::Stop) => {
                        stop = true;
                        break;
                    }
                    Err(mpsc::TryRecvError::Disconnected) => {
                        // 发送端已全部 drop（重连/关闭）：视为停止
                        stop = true;
                        break;
                    }
                    Err(mpsc::TryRecvError::Empty) => break,
                }
            }
            if stop {
                break;
            }
            match port.read(&mut byte_buf) {
                Ok(0) => {
                    // 无数据：继续轮询，稍作让步避免忙等
                    std::thread::sleep(Duration::from_millis(10));
                }
                Ok(n) => {
                    // 逐字节分流：帧状态机优先，其余进文本行缓冲
                    for &b in &byte_buf[..n] {
                        match frame_parser.feed(b) {
                            FrameOutcome::Text => line.push(b),
                            FrameOutcome::Consumed(Some(Ok((cmd, seq, payload)))) => {
                                let _ = app.emit(
                                    "cfg-frame",
                                    CfgFrameEvent {
                                        cmd,
                                        seq,
                                        payload_hex: hex_dump(&payload),
                                    },
                                );
                            }
                            FrameOutcome::Consumed(_) => {}
                        }
                    }
                    while let Some(pos) = line.iter().position(|&b| b == b'\n') {
                        let mut l: Vec<u8> = line.drain(..=pos).collect();
                        l.pop();
                        if l.last() == Some(&b'\r') {
                            l.pop();
                        }
                        let text = String::from_utf8_lossy(&l).into_owned();
                        if !text.is_empty() {
                            let _ = app.emit("serial-line", text);
                        }
                    }
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {
                    // 超时属正常轮询；未完结部分行（如 ">" 提示符）先行转发
                    if !line.is_empty() {
                        let text = String::from_utf8_lossy(&line).into_owned();
                        line.clear();
                        let _ = app.emit("serial-line", text);
                    }
                }
                Err(_) => {
                    break; // 端口错误（拔线等）
                }
            }
        }
        drop(port);
        let _ = app.emit("serial-closed", ());
    });

    Ok(())
}

/// 关闭串口：投递 Stop，读线程退出后端口彻底释放
#[tauri::command]
fn close_port(state: State<SerialState>) -> Result<(), String> {
    if let Some(sender) = state.sender.lock().unwrap().take() {
        let _ = sender.send(PortCmd::Stop);
    }
    Ok(())
}

/// 发送一行命令（自动补 \r\n）
#[tauri::command]
fn send_line(state: State<SerialState>, line: String) -> Result<(), String> {
    let guard = state.sender.lock().unwrap();
    match guard.as_ref() {
        Some(sender) => sender.send(PortCmd::SendLine(line)).map_err(|e| format!("发送失败: {}", e)),
        None => Err("串口未连接".into()),
    }
}

/// WinRT 通道写 0xFFE1 特征值（阻塞，需在非主线程调用）
fn winrt_write_char(c: &GattCharacteristic, data: &[u8]) -> Result<(), String> {
    let writer = DataWriter::new().map_err(|e| e.to_string())?;
    writer.WriteBytes(data).map_err(|e| e.to_string())?;
    let buf = writer.DetachBuffer().map_err(|e| e.to_string())?;
    // 默认写语义：特征值支持 Write Without Response 时以无响应写执行
    let status = c
        .WriteValueAsync(&buf)
        .map_err(|e| e.to_string())?
        .get()
        .map_err(|e| format!("BLE 写入失败: {}", e))?;
    if status.0 != 0 {
        return Err(format!("BLE 写入失败（GattCommunicationStatus={}）", status.0));
    }
    Ok(())
}

/// BLE 配置帧下行：优先 btleplug 通路，无则 WinRT 通路
async fn ble_write_frame(ble: &State<'_, BleState>, frame: &[u8]) -> Result<(), String> {
    // btleplug 通路
    let p = ble.peripheral.lock().unwrap().clone();
    if let Some(p) = p {
        let write_char: Option<Characteristic> = p
            .characteristics()
            .into_iter()
            .find(|c| {
                c.uuid == MONITOR_CHAR_UUID
                    && (c.properties
                        .contains(btleplug::api::CharPropFlags::WRITE)
                        || c.properties
                            .contains(btleplug::api::CharPropFlags::WRITE_WITHOUT_RESPONSE))
            });
        if let Some(c) = write_char {
            return p
                .write(&c, frame, btleplug::api::WriteType::WithoutResponse)
                .await
                .map_err(|e| e.to_string());
        }
    }
    // WinRT 通路（阻塞调用，放阻塞线程池避免卡 UI）
    let ch = ble.winrt_char.lock().unwrap().clone();
    if let Some(c) = ch {
        let data = frame.to_vec();
        return tauri::async_runtime::spawn_blocking(move || winrt_write_char(&c, &data))
            .await
            .map_err(|e| format!("BLE 写线程失败: {}", e))?;
    }
    Err("无可用配置通路（串口与蓝牙均未连接）".into())
}

/// 发送一帧 AA 55 二进制配置帧（SEQ 自动发号，单调递增）。
/// payload_hex 为十六进制字符串（容忍空格/0x 前缀），返回本次使用的 SEQ。
/// 通路选择：串口已连接走串口 WriteRaw；否则 WiFi TCP 已连接走 NET；否则走 BLE 0xFFE1 Write（三通路同一协议）。
/// 应答以 cfg-frame 事件（cmd/seq/payload_hex）转发前端。
#[tauri::command]
async fn cfg_send(
    serial: State<'_, SerialState>,
    ble: State<'_, BleState>,
    cmd: u8,
    payload_hex: String,
) -> Result<u16, String> {
    let payload = parse_hex(&payload_hex)?;
    if payload.len() > CFG_MAX_PAYLOAD {
        return Err(format!("payload 超长（{} > {}）", payload.len(), CFG_MAX_PAYLOAD));
    }
    // fetch_add 原子发号（返回旧值并自增）：电量轮询线程与连接流程并发调用 cfg_send 时，
    // load/store 两步会让两个线程拿到同一 SEQ；load 后不回存则每帧 seq 恒 0，被固件
    // SEQ 滑动窗口以 ERR_SEQ 全部拒绝（2026-10-10「USB/BLE/WiFi 全通路配置失效」根因）
    let seq = CFG_SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let frame = build_frame(cmd, seq, &payload);
    // 串口优先，其次 WiFi TCP（NET），最后 BLE
    let via_serial = {
        let guard = serial.sender.lock().unwrap();
        guard
            .as_ref()
            .map(|s| s.send(PortCmd::WriteRaw(frame.clone())).is_ok())
            .unwrap_or(false)
    };
    if via_serial {
    } else {
        let via_net = {
            let guard = NET_SENDER.lock().unwrap();
            guard
                .as_ref()
                .map(|s| s.send(NetCmd::WriteRaw(frame.clone())).is_ok())
                .unwrap_or(false)
        };
        if via_net {
        } else {
            ble_write_frame(&ble, &frame).await?;
        }
    }
    Ok(seq)
}

/// 发送 PING 握手帧（0x01）：设备端将 SEQ 滑动窗口重同步到本帧序号。
/// 任一通路（串口/BLE）连接建立后应先调用一次，之后 SEQ 按单调递增使用。
#[tauri::command]
async fn cfg_ping(serial: State<'_, SerialState>, ble: State<'_, BleState>) -> Result<u16, String> {
    cfg_send(serial, ble, CFG_CMD_PING, String::new()).await
}


/// 导出 Profile JSON：弹出原生保存对话框，把前端组装好的 JSON 文本写入用户选择的文件。
/// 前端已把 3 个 Profile（名称+键位）序列化为 JSON，本命令只负责路径选择与写盘。
#[tauri::command]
async fn export_profile_json(content: String) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let Some(path) = rfd::FileDialog::new()
            .add_filter("SimpleKey Profile JSON", &["json"])
            .set_file_name("simplekey-profiles.json")
            .save_file()
        else {
            return Err("已取消".into());
        };
        std::fs::write(&path, content.as_bytes())
            .map_err(|e| format!("写入失败: {e}"))?;
        Ok(path.to_string_lossy().into_owned())
    })
    .await
    .map_err(|e| format!("导出线程失败: {e}"))?
}

/// 导入 Profile JSON：弹出原生打开对话框，返回所选文件的路径与文本内容。
/// 校验与逐 Profile 写入（帧协议 PROFILE_WRITE）由前端完成。
#[tauri::command]
async fn import_profile_json() -> Result<(String, String), String> {
    tauri::async_runtime::spawn_blocking(move || {
        let Some(path) = rfd::FileDialog::new()
            .add_filter("SimpleKey Profile JSON", &["json"])
            .pick_file()
        else {
            return Err("已取消".into());
        };
        let content = std::fs::read_to_string(&path)
            .map_err(|e| format!("读取失败: {e}"))?;
        Ok((path.to_string_lossy().into_owned(), content))
    })
    .await
    .map_err(|e| format!("导入线程失败: {e}"))?
}

// ==================== P7 维护：esptool 固件刷写 / coredump 读取 ====================

#[cfg(windows)]
use std::os::windows::process::CommandExt;

/// 定位可运行 esptool 的 Python 解释器：
/// ① IDF_PYTHON_ENV_PATH 环境变量指向的虚拟环境；② 本机 ESP-IDF v5.4 默认路径；
/// ③ PATH 上的 python（需已 pip 安装 esptool）。
fn find_esptool_python() -> Option<std::path::PathBuf> {
    let candidates: Vec<std::path::PathBuf> = [
        std::env::var("IDF_PYTHON_ENV_PATH").ok().map(|p| {
            std::path::PathBuf::from(p).join("Scripts").join("python.exe")
        }),
        Some(std::path::PathBuf::from(
            r"D:\programme\ESP32\Espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe",
        )),
        Some(std::path::PathBuf::from("python")),
    ]
    .into_iter()
    .flatten()
    .collect();
    for c in candidates {
        if c.is_absolute() && !c.exists() {
            continue;
        }
        return Some(c);
    }
    None
}

/// 运行 esptool 子命令，输出逐行经 esptool-progress 事件转发前端（日志台可见进度）。
fn run_esptool(port: &str, args: &[String], app: &AppHandle) -> Result<(), String> {
    let py = find_esptool_python().ok_or(
        "未找到可用的 Python/esptool 环境（需 ESP-IDF 虚拟环境或 PATH 上的 esptool）",
    )?;
    let mut cmd = std::process::Command::new(&py);
    cmd.arg("-m").arg("esptool");
    cmd.arg("--chip").arg("esp32s3");
    cmd.arg("--port").arg(port);
    cmd.arg("--baud").arg("460800");
    for a in args {
        cmd.arg(a);
    }
    cmd.stdout(std::process::Stdio::piped());
    cmd.stderr(std::process::Stdio::piped());
    #[cfg(windows)]
    cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW，避免闪黑框
    let mut child = cmd.spawn().map_err(|e| format!("启动 esptool 失败: {e}"))?;
    // stdout/stderr 双线程逐行转发（esptool 进度主要在 stderr）
    let app_out = app.clone();
    let mut stdout = child.stdout.take().unwrap();
    let t_out = std::thread::spawn(move || {
        let mut buf = std::io::BufReader::new(&mut stdout);
        let mut line = String::new();
        loop {
            line.clear();
            match std::io::BufRead::read_line(&mut buf, &mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => {
                    let _ = app_out.emit("esptool-progress", line.trim_end().to_string());
                }
            }
        }
    });
    let app_err = app.clone();
    let mut stderr = child.stderr.take().unwrap();
    let t_err = std::thread::spawn(move || {
        let mut buf = std::io::BufReader::new(&mut stderr);
        let mut line = String::new();
        loop {
            line.clear();
            match std::io::BufRead::read_line(&mut buf, &mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => {
                    let _ = app_err.emit("esptool-progress", line.trim_end().to_string());
                }
            }
        }
    });
    t_out.join().ok();
    t_err.join().ok();
    let status = child.wait().map_err(|e| format!("等待 esptool 失败: {e}"))?;
    if status.success() {
        app.emit("esptool-progress", "[HOST] esptool 执行成功".to_string())
            .ok();
        Ok(())
    } else {
        Err(format!("esptool 执行失败（退出码 {:?}），详见日志台输出", status.code()))
    }
}

/// 固件刷写：入参为 **.bin 文件**（仅刷应用 @0x10000）或 **build 目录**（完整刷写：
/// bootloader.bin @0x0 + partition-table.bin @0x8000 + 应用 bin @0x10000，分区表变更后必须用完整刷写）。
/// 注意：刷写前必须释放串口（前端先 close_port），esptool 自带进入下载模式时序。
#[tauri::command]
async fn flash_firmware(app: AppHandle, port: String, bin_path: String) -> Result<(), String> {
    let path = std::path::PathBuf::from(&bin_path);
    let args: Vec<String> = if path.is_dir() {
        let boot = path.join("bootloader").join("bootloader.bin");
        let part = path.join("partition_table").join("partition-table.bin");
        let appbin = std::fs::read_dir(&path)
            .map_err(|e| format!("读取目录失败: {e}"))?
            .flatten()
            .map(|e| e.path())
            .find(|p| p.extension().map(|x| x == "bin").unwrap_or(false))
            .ok_or("build 目录下未找到应用 .bin（如 SimpleKey_TriMode.bin）")?;
        if !boot.exists() || !part.exists() {
            return Err("build 目录缺少 bootloader/partition_table（请改选应用 .bin 仅刷应用）".into());
        }
        vec![
            "write_flash".into(),
            "0x0".into(),
            boot.to_string_lossy().into_owned(),
            "0x8000".into(),
            part.to_string_lossy().into_owned(),
            "0x10000".into(),
            appbin.to_string_lossy().into_owned(),
        ]
    } else {
        if !path.exists() {
            return Err(format!("固件文件不存在: {bin_path}"));
        }
        vec![
            "write_flash".into(),
            "0x10000".into(),
            path.to_string_lossy().into_owned(),
        ]
    };
    tauri::async_runtime::spawn_blocking(move || run_esptool(&port, &args, &app))
        .await
        .map_err(|e| format!("刷写线程失败: {e}"))?
}

/// 读取 coredump 分区（0x1F0000，0x10000 字节）保存为 .bin：
/// 之后可用 esp-coredump / gdb 解析崩溃现场（ELF 格式，sdkconfig 已开 DATA_FORMAT_ELF）。
#[tauri::command]
async fn read_coredump(app: AppHandle, port: String) -> Result<String, String> {
    let path = tauri::async_runtime::spawn_blocking(move || {
        rfd::FileDialog::new()
            .add_filter("CoreDump Binary", &["bin"])
            .set_file_name("simplekey-coredump.bin")
            .save_file()
            .map(|p| p.to_string_lossy().into_owned())
            .ok_or_else(|| "已取消".to_string())
    })
    .await
    .map_err(|e| e.to_string())??;
    let args = vec![
        "read_flash".into(),
        "0x1F0000".into(),
        "0x10000".into(),
        path.clone(),
    ];
    tauri::async_runtime::spawn_blocking(move || run_esptool(&port, &args, &app))
        .await
        .map_err(|e| format!("读取线程失败: {e}"))??;
    Ok(path)
}

/// 选择固件文件（.bin）或 build 目录，返回路径（前端据此调用 flash_firmware）。
#[tauri::command]
async fn pick_firmware() -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(|| {
        rfd::FileDialog::new()
            .add_filter("固件 bin / build 目录", &["bin"])
            .pick_file()
            .or_else(|| {
                rfd::FileDialog::new()
                    .set_title("或选择 build 目录（完整刷写）")
                    .pick_folder()
            })
            .map(|p| p.to_string_lossy().into_owned())
            .ok_or_else(|| "已取消".to_string())
    })
    .await
    .map_err(|e| e.to_string())?
}

fn main() {
    tauri::Builder::default()
        .manage(SerialState::default())
        .manage(BleState::default())
        .invoke_handler(tauri::generate_handler![
            list_ports,
            open_port,
            close_port,
            send_line,
            cfg_send,
            cfg_ping,
            export_profile_json,
            import_profile_json,
            flash_firmware,
            read_coredump,
            pick_firmware,
            wifi_connect,
            wifi_disconnect,
            ble_scan_connect,
            ble_disconnect
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
