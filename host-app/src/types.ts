export interface PortInfo {
  name: string;
  desc: string;
}

// 结构化事件（协议见 docs/串口命令协议.md §3.5）
export type SerialEvent =
  | { kind: "key"; down: boolean; row: number; col: number; kc?: number }
  | { kind: "enc"; action: "CW" | "CCW" | "PRESS" | "RELEASE" | "LONG" }
  | { kind: "mode"; mode: "usb" | "ble" }
  | { kind: "profile"; index: number };

// 单个 Profile 数据（帧协议 PROFILE_GET 0x86 应答解析结果）
export interface ProfileInfo {
  name: string;
  keymap: number[][]; // 5x4
  defaultMode: number;
}

// 电源状态（帧协议 POWER_GET 0x09 应答解析结果）
export interface BatteryStatus {
  mv: number;
  pct: number;
  charging: boolean;
  full: boolean;
  usb: boolean;
}

// 宏步骤 / 宏定义（与固件 macro_t packed 结构对应）
export interface MacroStep {
  action: number; // 0 按下 / 1 松开 / 2 延迟 / 3 轻点
  keycode: number; // 延迟步骤时为 0
  delayMs: number; // 延迟步骤的毫秒数（uint16 LE）
}
export interface MacroData {
  name: string;
  steps: MacroStep[];
}

// 录制事件（[EVT] KEY 行收集，时间戳本地生成）
export interface RecEvt {
  down: boolean;
  kc: number;
  t: number;
}

// 单个配对槽位（BLE_SLOT_GET 0x13 应答解析结果）
export interface BleSlotInfo {
  paired: boolean;
  peerAddr: number[]; // 6B，NimBLE ble_addr_t.val 字节序（val[0] 为地址低字节）
  addrType: number; // 0=公共地址 1=随机地址
  name: string;
}

export interface BleSlotsData {
  currentSlot: number; // 当前槽位 0~2
  slots: BleSlotInfo[];
}
