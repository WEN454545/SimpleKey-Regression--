import type {
  SerialEvent,
  BleSlotInfo,
  BleSlotsData,
  MacroStep,
  MacroData,
  RecEvt,
} from "./types";

// 解析 [EVT] 事件行，非事件行返回 null
export function parseEventLine(line: string): SerialEvent | null {
  if (!line.startsWith("[EVT] ")) return null;
  const parts = line.split(/\s+/); // ["[EVT]", "KEY", "DOWN", "0", "0"] ...
  switch (parts[1]) {
    case "KEY": {
      const down = parts[2] === "DOWN";
      const row = Number(parts[3]);
      const col = Number(parts[4]);
      const kc = parts[5] ? parseInt(parts[5], 16) : NaN; // "0x1E" 形式键码（宏录制用）
      if (!isNaN(row) && !isNaN(col)) return { kind: "key", down, row, col, kc };
      return null;
    }
    case "ENC": {
      const action = parts[2] as "CW" | "CCW" | "PRESS" | "RELEASE" | "LONG";
      if (["CW", "CCW", "PRESS", "RELEASE", "LONG"].includes(action))
        return { kind: "enc", action };
      return null;
    }
    case "MODE": {
      const mode = parts[2] as "usb" | "ble";
      if (mode === "usb" || mode === "ble") return { kind: "mode", mode };
      return null;
    }
    case "PROFILE": {
      const index = Number(parts[2]);
      if (!isNaN(index)) return { kind: "profile", index };
      return null;
    }
    default:
      return null;
  }
}

// 解析 keymap 回显行（固件 keymap_print 输出）：
//   "Row 0: 0x1E(1) 0x1F(2) 0x20(3) 0x21(4)" → { row: 0, codes: [...] }
// 非该格式返回 null
export function parseKeymapLine(line: string): { row: number; codes: number[] } | null {
  const m = line.match(/^Row (\d+):(.+)$/);
  if (!m) return null;
  const codes = [...m[2].matchAll(/0x([0-9A-Fa-f]{2})/g)].map((h) => parseInt(h[1], 16));
  if (codes.length !== MATRIX_COLS) return null;
  return { row: Number(m[1]), codes };
}

// ==================== 常量 ====================

// 矩阵尺寸：ROW 0-4 × COL 0-3
export const MATRIX_ROWS = 5;
export const MATRIX_COLS = 4;

export const PROFILE_COUNT = 3;

// 字节数组 → 十六进制 payload 字符串（cfg_send 参数格式）
export const hexOf = (arr: number[]): string =>
  arr.map((n) => (n & 0xff).toString(16).padStart(2, "0")).join("");

// 名称 → 16 字节（UTF-8，最长 15 字节保留 \0 结尾；截断回退到字符边界，防截半个多字节字符）
export const nameToBytes = (name: string): number[] => {
  const all = Array.from(new TextEncoder().encode(name));
  let end = Math.min(all.length, 15);
  while (end > 0 && (all[end] & 0xc0) === 0x80) end--;
  const b = all.slice(0, end);
  while (b.length < 16) b.push(0);
  return b;
};

// ==================== BLE 配对槽位（协议 v2.5） ====================

export const BLE_SLOT_COUNT = 3;
export const BLE_SLOT_NAME_LEN = 32;

// UTF-8 字节序列 → 字符串（丢弃末尾不完整的多字节字符，防解码产生 U+FFFD）。
// 只检查最后一个多字节序列：若截断发生，被截的必然是末尾字符
export const utf8DecodeSafe = (bytes: number[]): string => {
  let end = bytes.length;
  if (end > 0) {
    let j = end - 1;
    while (j >= 0 && (bytes[j] & 0xc0) === 0x80) j--; // 回退到末尾序列的首字节
    if (j >= 0) {
      const lead = bytes[j];
      const len =
        lead < 0x80 ? 1 : (lead & 0xe0) === 0xc0 ? 2 : (lead & 0xf0) === 0xe0 ? 3 : 4;
      if (j + len > end) end = j; // 尾字符不完整 → 剔除
    }
  }
  return new TextDecoder().decode(new Uint8Array(bytes.slice(0, end)));
};

// BLE_SLOT_GET 0x13 应答解析：current_slot(1B) + 3×[paired(1B)+peer_addr(6B)+addr_type(1B)+name(32B)] = 121B
// name 为定长 32B UTF-8 字段：先按 \0 截断，再丢弃末尾不完整的多字节字符
export const parseBleSlotGet = (bytes: number[]): BleSlotsData | null => {
  const SLOT_LEN = 1 + 6 + 1 + BLE_SLOT_NAME_LEN; // 40B
  if (bytes.length !== 1 + BLE_SLOT_COUNT * SLOT_LEN) return null;
  const slots: BleSlotInfo[] = [];
  for (let i = 0; i < BLE_SLOT_COUNT; i++) {
    const o = 1 + i * SLOT_LEN;
    const nameB = bytes.slice(o + 8, o + SLOT_LEN);
    const z = nameB.indexOf(0);
    slots.push({
      paired: bytes[o] !== 0,
      peerAddr: bytes.slice(o + 1, o + 7),
      addrType: bytes[o + 7],
      name: utf8DecodeSafe(z === -1 ? nameB : nameB.slice(0, z)),
    });
  }
  return { currentSlot: bytes[0], slots };
};

// 6B 地址 → AA:BB:CC:DD:EE:FF（NimBLE val 为小端存储，按人类习惯的大端倒序冒号显示）
export const fmtBleAddr = (addr: number[]): string =>
  [...addr]
    .reverse()
    .map((n) => (n & 0xff).toString(16).padStart(2, "0"))
    .join(":")
    .toUpperCase();

// ==================== 宏与旋钮（P6） ====================

// 宏步骤动作（与固件 macro_step_t.action 一致）
export const MACRO_ACT = { DOWN: 0, UP: 1, DELAY: 2, TAP: 3 } as const;

export const MACRO_COUNT = 4;
export const MACRO_STEP_MAX = 32;

// MacroData → 帧协议 payload：idx(1B) + name(8B) + step_count(1B) + steps(32×4B) = 138B
export const macroToPayload = (i: number, m: MacroData): number[] => {
  // 宏名 7 字节内截断（回退到 UTF-8 字符边界，防截半个多字节字符）
  const nameAll = Array.from(new TextEncoder().encode(m.name));
  let nameEnd = Math.min(nameAll.length, 7);
  while (nameEnd > 0 && (nameAll[nameEnd] & 0xc0) === 0x80) nameEnd--;
  const out = [i, ...nameAll.slice(0, nameEnd)];
  while (out.length < 9) out.push(0); // idx + name 补齐 9 字节
  const steps = m.steps.slice(0, MACRO_STEP_MAX);
  out.push(steps.length);
  for (const s of steps) {
    out.push(s.action & 0xff, s.keycode & 0xff, s.delayMs & 0xff, (s.delayMs >> 8) & 0xff);
  }
  while (out.length < 9 + 1 + MACRO_STEP_MAX * 4) out.push(0);
  return out;
};

// 录制事件序列 → 宏步骤：DOWN/UP + 真实间隔 DELAY（≥20ms 才插入，单步上限 60000ms）；
// 停止时仍按住的键逆序补 UP 防卡键；未配对 DOWN 的 UP 丢弃（录制前已按住的键）
export const buildRecSteps = (evts: RecEvt[], startedAt: number): MacroStep[] => {
  const steps: MacroStep[] = [];
  const held = new Set<number>();
  let prevT = startedAt;
  for (const e of evts) {
    const gap = e.t - prevT;
    if (gap >= 20) {
      steps.push({ action: MACRO_ACT.DELAY, keycode: 0, delayMs: Math.min(gap, 60000) });
    }
    prevT = e.t;
    if (e.down) {
      held.add(e.kc);
      steps.push({ action: MACRO_ACT.DOWN, keycode: e.kc, delayMs: 0 });
    } else if (held.delete(e.kc)) {
      steps.push({ action: MACRO_ACT.UP, keycode: e.kc, delayMs: 0 });
    }
  }
  for (const kc of [...held].reverse()) {
    steps.push({ action: MACRO_ACT.UP, keycode: kc, delayMs: 0 });
  }
  // 尾部悬空 DELAY 清理：结尾事件全被丢弃（如仅松开录制前已按住的键）时不留空转宏
  while (steps.length > 0 && steps[steps.length - 1].action === MACRO_ACT.DELAY) {
    steps.pop();
  }
  return steps;
};
