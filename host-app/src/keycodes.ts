// ==================== 键码目录（键码选择器） ====================

export interface KeyDef {
  code: number;
  label: string;
  name: string;
}

// 连续区段生成（数字 0x1E-0x27，字母 0x04-0x1D）
const mkKeys = (chars: string, base: number): KeyDef[] =>
  [...chars].map((ch, i) => ({ code: base + i, label: ch, name: `KC_${ch.toUpperCase()}` }));

// 键码分组目录（标准 HID 键盘键码，与固件 keymap 存储一致）
export const KEY_GROUPS: { title: string; keys: KeyDef[] }[] = [
  { title: "数字", keys: mkKeys("1234567890", 0x1e) },
  { title: "字母", keys: mkKeys("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 0x04) },
  {
    title: "功能",
    keys: Array.from({ length: 12 }, (_, i) => ({
      code: 0x3a + i,
      label: `F${i + 1}`,
      name: `KC_F${i + 1}`,
    })),
  },
  {
    title: "修饰键",
    keys: [
      { code: 0xe0, label: "LCtl", name: "KC_LCTL" },
      { code: 0xe1, label: "LSft", name: "KC_LSFT" },
      { code: 0xe2, label: "LAlt", name: "KC_LALT" },
      { code: 0xe3, label: "LGui", name: "KC_LGUI" },
      { code: 0xe4, label: "RCtl", name: "KC_RCTL" },
      { code: 0xe5, label: "RSft", name: "KC_RSFT" },
      { code: 0xe6, label: "RAlt", name: "KC_RALT" },
      { code: 0xe7, label: "RGui", name: "KC_RGUI" },
    ],
  },
  {
    title: "导航",
    keys: [
      { code: 0x29, label: "Esc", name: "KC_ESC" },
      { code: 0x2b, label: "Tab", name: "KC_TAB" },
      { code: 0x28, label: "Enter", name: "KC_ENT" },
      { code: 0x2c, label: "Space", name: "KC_SPC" },
      { code: 0x2a, label: "Bksp", name: "KC_BSPC" },
      { code: 0x4c, label: "Del", name: "KC_DEL" },
      { code: 0x39, label: "Caps", name: "KC_CAPS" },
      { code: 0x49, label: "Ins", name: "KC_INS" },
      { code: 0x4a, label: "Home", name: "KC_HOME" },
      { code: 0x4b, label: "PgUp", name: "KC_PGUP" },
      { code: 0x4d, label: "End", name: "KC_END" },
      { code: 0x4e, label: "PgDn", name: "KC_PGDN" },
    ],
  },
  {
    // 光标控制（0x4F~0x52，USB 描述符键码数组上限 0x65 内，两端可用）
    title: "方向键",
    keys: [
      { code: 0x52, label: "↑", name: "KC_UP" },
      { code: 0x51, label: "↓", name: "KC_DOWN" },
      { code: 0x50, label: "←", name: "KC_LEFT" },
      { code: 0x4f, label: "→", name: "KC_RIGHT" },
      { code: 0x46, label: "PrtSc", name: "KC_PSCR" },
      { code: 0x47, label: "ScrLk", name: "KC_SLCK" },
      { code: 0x48, label: "Pause", name: "KC_PAUS" },
    ],
  },
  {
    // 数字小键盘（0x53~0x63，与方向键同为标准键盘声明范围内键码）
    title: "小键盘",
    keys: [
      { code: 0x53, label: "Num", name: "KC_NUM" },
      { code: 0x54, label: "kp/", name: "KC_PSLS" },
      { code: 0x55, label: "kp*", name: "KC_PAST" },
      { code: 0x56, label: "kp-", name: "KC_PMNS" },
      { code: 0x57, label: "kp+", name: "KC_PPLS" },
      { code: 0x58, label: "kpEnt", name: "KC_PENT" },
      { code: 0x59, label: "kp1", name: "KC_P1" },
      { code: 0x5a, label: "kp2", name: "KC_P2" },
      { code: 0x5b, label: "kp3", name: "KC_P3" },
      { code: 0x5c, label: "kp4", name: "KC_P4" },
      { code: 0x5d, label: "kp5", name: "KC_P5" },
      { code: 0x5e, label: "kp6", name: "KC_P6" },
      { code: 0x5f, label: "kp7", name: "KC_P7" },
      { code: 0x60, label: "kp8", name: "KC_P8" },
      { code: 0x61, label: "kp9", name: "KC_P9" },
      { code: 0x62, label: "kp0", name: "KC_P0" },
      { code: 0x63, label: "kp.", name: "KC_PDOT" },
    ],
  },
  {
    title: "符号",
    keys: [
      { code: 0x2d, label: "-", name: "KC_MINS" },
      { code: 0x2e, label: "=", name: "KC_EQL" },
      { code: 0x2f, label: "[", name: "KC_LBRC" },
      { code: 0x30, label: "]", name: "KC_RBRC" },
      { code: 0x31, label: "\\", name: "KC_BSLS" },
      { code: 0x33, label: ";", name: "KC_SCLN" },
      { code: 0x34, label: "'", name: "KC_QUOT" },
      { code: 0x35, label: "`", name: "KC_GRV" },
      { code: 0x36, label: ",", name: "KC_COMM" },
      { code: 0x37, label: ".", name: "KC_DOT" },
      { code: 0x38, label: "/", name: "KC_SLSH" },
      { code: 0x00, label: "None", name: "KC_NO" },
    ],
  },
  {
    // P6：Consumer 功能（bit 序 = 键码-0xC0，与固件 hid_manager consumer 位域一致）
    title: "媒体",
    keys: [
      { code: 0xc0, label: "VOL+", name: "音量 +" },
      { code: 0xc1, label: "VOL-", name: "音量 -" },
      { code: 0xc2, label: "MUTE", name: "静音" },
      { code: 0xc3, label: "PLAY", name: "播放/暂停" },
      { code: 0xc4, label: "NEXT", name: "下一曲" },
      { code: 0xc5, label: "PREV", name: "上一曲" },
      { code: 0xc6, label: "BRI+", name: "亮度 +" },
      { code: 0xc7, label: "BRI-", name: "亮度 -" },
    ],
  },
  {
    // P6：宏触发键（按下触发对应槽位宏，序列在上位机「宏编辑」卡片配置）
    title: "宏",
    keys: [
      { code: 0xf0, label: "M1", name: "宏 1 触发" },
      { code: 0xf1, label: "M2", name: "宏 2 触发" },
      { code: 0xf2, label: "M3", name: "宏 3 触发" },
      { code: 0xf3, label: "M4", name: "宏 4 触发" },
    ],
  },
];

// 键码 → 显示标签反查（网格按键标题用）
export const CODE_TO_LABEL = new Map<number, string>(
  KEY_GROUPS.flatMap((g) => g.keys.map((k) => [k.code, k.label]))
);

// 特殊键中文副标签（与原版样式图一致：Shift/切换、Enter/输入、Ctrl/Ctrl+T、Space/空间）
export const SUB_LABEL = new Map<number, string>([
  [0x28, "输入"], // Enter
  [0x2c, "空间"], // Space
  [0xe0, "Ctrl+T"], // LCtrl
  [0xe4, "Ctrl+T"], // RCtrl
  [0xe1, "切换"], // LShift
  [0xe5, "切换"], // RShift
]);

// 默认键位键码（与固件 keymap.c default_keymap 一致，第 0 层）
export const DEFAULT_KEYMAP: number[][] = [
  [0x1e, 0x1f, 0x20, 0x21],
  [0x14, 0x17, 0x08, 0x15],
  [0x04, 0x16, 0x07, 0x09],
  [0x1d, 0x1b, 0x06, 0x19],
  [0xe0, 0xe1, 0x2c, 0x28],
];
