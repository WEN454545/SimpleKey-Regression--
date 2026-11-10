/**
 * @file protocol.test.ts
 * @brief 上位机纯逻辑单元测试（红绿灯法）
 *
 * 被测对象：protocol.ts 中导出的纯函数（协议解析/组包/宏录制步骤生成）。
 * 运行：npm test（vitest run）
 */
import { describe, it, expect } from "vitest";
import {
  parseEventLine,
  parseKeymapLine,
  hexOf,
  nameToBytes,
  macroToPayload,
  buildRecSteps,
  utf8DecodeSafe,
  parseBleSlotGet,
  fmtBleAddr,
} from "./protocol";

// ==================== parseEventLine ====================

describe("parseEventLine", () => {
  it("KEY DOWN 带键码", () => {
    expect(parseEventLine("[EVT] KEY DOWN 1 2 0x1E")).toEqual({
      kind: "key", down: true, row: 1, col: 2, kc: 0x1e,
    });
  });

  it("KEY UP 无键码 → kc=NaN", () => {
    const evt = parseEventLine("[EVT] KEY UP 0 3");
    expect(evt).toMatchObject({ kind: "key", down: false, row: 0, col: 3 });
  });

  it("ENC 动作", () => {
    expect(parseEventLine("[EVT] ENC CW")).toEqual({ kind: "enc", action: "CW" });
    expect(parseEventLine("[EVT] ENC PRESS")).toEqual({ kind: "enc", action: "PRESS" });
  });

  it("非法 ENC 动作返回 null", () => {
    expect(parseEventLine("[EVT] ENC SPIN")).toBeNull();
  });

  it("MODE / PROFILE", () => {
    expect(parseEventLine("[EVT] MODE ble")).toEqual({ kind: "mode", mode: "ble" });
    expect(parseEventLine("[EVT] PROFILE 2")).toEqual({ kind: "profile", index: 2 });
  });

  it("非事件行返回 null", () => {
    expect(parseEventLine("I (1234) CFG: hello")).toBeNull();
    expect(parseEventLine("")).toBeNull();
  });
});

// ==================== parseKeymapLine ====================

describe("parseKeymapLine", () => {
  it("标准 keymap 回显行", () => {
    expect(parseKeymapLine("Row 0: 0x1E(1) 0x1F(2) 0x20(3) 0x21(4)")).toEqual({
      row: 0,
      codes: [0x1e, 0x1f, 0x20, 0x21],
    });
  });

  it("键码数量不足 4 → null", () => {
    expect(parseKeymapLine("Row 1: 0x1E(1) 0x1F(2)")).toBeNull();
  });

  it("非 Row 行 → null", () => {
    expect(parseKeymapLine("KEYMAP saved")).toBeNull();
  });
});

// ==================== hexOf / nameToBytes ====================

describe("hexOf", () => {
  it("两位小写十六进制", () => {
    expect(hexOf([0x1e, 0x0f, 0xff])).toBe("1e0fff");
  });
});

describe("nameToBytes", () => {
  it("短名称补零到 16 字节", () => {
    const b = nameToBytes("AB");
    expect(b).toHaveLength(16);
    expect(b[0]).toBe(0x41);
    expect(b[2]).toBe(0);
  });

  it("15 字节 ASCII 保留 \\0 结尾", () => {
    const b = nameToBytes("123456789012345");
    expect(b.slice(0, 15)).toEqual(Array.from(new TextEncoder().encode("123456789012345")));
    expect(b[15]).toBe(0);
  });

  it("恰好 15 字节的纯中文不截断", () => {
    // 5 个汉字 = 15 字节
    const b = nameToBytes("测试测试测");
    expect(b.slice(0, 15).every((x) => x !== 0)).toBe(true);
    expect(b[15]).toBe(0);
    const decoded = new TextDecoder().decode(new Uint8Array(b.slice(0, 15)));
    expect(decoded).toBe("测试测试测");
  });

  it("超长中英混合名在 UTF-8 字符边界截断（不产生半个字符）", () => {
    // 5 字节 ASCII + 4 汉字 = 17 字节 → 截到 15 字节时第 15 字节落在汉字中间
    const b = nameToBytes("abcde测试试测");
    expect(b).toHaveLength(16);
    expect(b[15]).toBe(0);
    const body = b.slice(0, b.indexOf(0)); // 非 0 有效前缀
    // 截断结果必须是合法 UTF-8 序列（无 U+FFFD 替换符）
    const decoded = new TextDecoder().decode(new Uint8Array(body));
    expect(decoded).not.toContain("\uFFFD");
    expect(decoded.startsWith("abcde")).toBe(true);
  });
});

// ==================== macroToPayload ====================

describe("macroToPayload", () => {
  it("payload 长度 138，布局 idx+name(8)+step_count+steps(32×4)", () => {
    const p = macroToPayload(2, { name: "M1", steps: [{ action: 0, keycode: 0x04, delayMs: 0 }] });
    expect(p).toHaveLength(138);
    expect(p[0]).toBe(2);
    expect(p[1]).toBe(0x4d); // 'M'
    expect(p[2]).toBe(0x31); // '1'
    expect(p[9]).toBe(1); // step_count
    expect(p.slice(10, 14)).toEqual([0, 0x04, 0, 0]); // action,keycode,delay LE
  });

  it("步骤数超 32 截断", () => {
    const steps = Array.from({ length: 40 }, (_, i) => ({ action: 2, keycode: 0, delayMs: i + 1 }));
    const p = macroToPayload(0, { name: "X", steps });
    expect(p[9]).toBe(32);
  });

  it("宏名在 UTF-8 字符边界截断（7 字节内不产生半个字符）", () => {
    // a(1)+b(1)+测(3)+试(3) = 8 字节 → 截到 7 字节时第 7 字节落在「试」中间
    const p = macroToPayload(0, { name: "ab测测试", steps: [] });
    const nameB = p.slice(1, 9);
    const decoded = new TextDecoder().decode(new Uint8Array(nameB.filter((x) => x !== 0)));
    expect(decoded).not.toContain("\uFFFD");
  });
});

// ==================== buildRecSteps ====================

describe("buildRecSteps", () => {
  it("DOWN/UP 配对；≥20ms 间隔插入 DELAY", () => {
    const steps = buildRecSteps(
      [
        { down: true, kc: 0x04, t: 100 },
        { down: false, kc: 0x04, t: 200 },
      ],
      0,
    );
    expect(steps).toEqual([
      { action: 2, keycode: 0, delayMs: 100 }, // 启动到首个按键的间隔
      { action: 0, keycode: 0x04, delayMs: 0 },
      { action: 2, keycode: 0, delayMs: 100 }, // 按住时长
      { action: 1, keycode: 0x04, delayMs: 0 },
    ]);
  });

  it("间隔 <20ms 不插入 DELAY", () => {
    const steps = buildRecSteps(
      [
        { down: true, kc: 0x04, t: 10 },
        { down: false, kc: 0x04, t: 20 },
      ],
      0,
    );
    expect(steps).toEqual([
      { action: 0, keycode: 0x04, delayMs: 0 },
      { action: 1, keycode: 0x04, delayMs: 0 },
    ]);
  });

  it("未配对的 UP 丢弃（录制前已按住的键）", () => {
    const steps = buildRecSteps([{ down: false, kc: 0x04, t: 100 }], 0);
    expect(steps).toEqual([]);
  });

  it("停止时仍按住的键逆序补 UP 防卡键", () => {
    const steps = buildRecSteps(
      [
        { down: true, kc: 0x04, t: 0 },
        { down: true, kc: 0x05, t: 10 },
      ],
      0,
    );
    expect(steps).toEqual([
      { action: 0, keycode: 0x04, delayMs: 0 },
      { action: 0, keycode: 0x05, delayMs: 0 },
      { action: 1, keycode: 0x05, delayMs: 0 },
      { action: 1, keycode: 0x04, delayMs: 0 },
    ]);
  });

  it("单步 DELAY 上限 60000ms", () => {
    const steps = buildRecSteps([{ down: true, kc: 0x04, t: 999999 }], 0);
    expect(steps[0]).toEqual({ action: 2, keycode: 0, delayMs: 60000 });
  });

  it("重复 DOWN 同键后首个 UP 配对，第二个 UP 丢弃", () => {
    const steps = buildRecSteps(
      [
        { down: true, kc: 0x04, t: 0 },
        { down: true, kc: 0x04, t: 10 },
        { down: false, kc: 0x04, t: 20 },
        { down: false, kc: 0x04, t: 30 },
      ],
      0,
    );
    expect(steps).toEqual([
      { action: 0, keycode: 0x04, delayMs: 0 },
      { action: 0, keycode: 0x04, delayMs: 0 },
      { action: 1, keycode: 0x04, delayMs: 0 },
    ]);
  });
});

// ==================== BLE 配对槽位（协议 v2.5） ====================

// 构造 0x93 应答 payload：current_slot(1B) + 3×40B 槽位组
const buildSlotPayload = (
  currentSlot: number,
  slots: Array<{ paired: boolean; addr: number[]; type: number; name: string } | undefined>,
): number[] => {
  const out = [currentSlot];
  for (let i = 0; i < 3; i++) {
    const s = slots[i];
    if (!s) {
      for (let j = 0; j < 40; j++) out.push(0);
      continue;
    }
    const nameB = Array.from(new TextEncoder().encode(s.name)).slice(0, 32);
    while (nameB.length < 32) nameB.push(0);
    out.push(s.paired ? 1 : 0, ...s.addr, s.type, ...nameB);
  }
  return out;
};

describe("utf8DecodeSafe", () => {
  it("完整 UTF-8 序列正常解码", () => {
    const b = Array.from(new TextEncoder().encode("测试ABC"));
    expect(utf8DecodeSafe(b)).toBe("测试ABC");
  });

  it("末尾被截断的汉字被剔除（不产生 U+FFFD）", () => {
    // 测(3B)+试(3B)，截到 4 字节时「试」只剩前 2 个续字节中的 1 个完整性问题
    const full = Array.from(new TextEncoder().encode("测试"));
    const cut = full.slice(0, 4); // 试 的首字节+1 续字节
    const decoded = utf8DecodeSafe(cut);
    expect(decoded).toBe("测");
    expect(decoded).not.toContain("\uFFFD");
  });

  it("以 ASCII 结尾不受影响", () => {
    const b = Array.from(new TextEncoder().encode("测试A"));
    expect(utf8DecodeSafe(b)).toBe("测试A");
  });
});

describe("parseBleSlotGet", () => {
  it("payload 长度不是 121 → null", () => {
    expect(parseBleSlotGet([])).toBeNull();
    expect(parseBleSlotGet(new Array(120).fill(0))).toBeNull();
    expect(parseBleSlotGet(new Array(122).fill(0))).toBeNull();
  });

  it("解析 current_slot / 配对状态 / 地址 / 名称", () => {
    const addr = [0x34, 0x12, 0x0a, 0x1f, 0xeb, 0xd3]; // NimBLE 小端 val
    const bytes = buildSlotPayload(1, [
      undefined,
      { paired: true, addr, type: 1, name: "我的手机" },
      { paired: false, addr: [0, 0, 0, 0, 0, 0], type: 0, name: "" },
    ]);
    const data = parseBleSlotGet(bytes);
    expect(data).not.toBeNull();
    expect(data!.currentSlot).toBe(1);
    expect(data!.slots[0]).toMatchObject({ paired: false, name: "" });
    expect(data!.slots[1]).toMatchObject({
      paired: true,
      peerAddr: addr,
      addrType: 1,
      name: "我的手机",
    });
    expect(data!.slots[2]).toMatchObject({ paired: false });
  });

  it("名称区按 \\0 截断，尾部补零不进入名称", () => {
    const bytes = buildSlotPayload(0, [
      { paired: true, addr: [1, 2, 3, 4, 5, 6], type: 0, name: "KB" },
    ]);
    const data = parseBleSlotGet(bytes)!;
    expect(data.slots[0].name).toBe("KB");
  });

  it("名称恰满 32 字节且截在字符中间 → 边界回退无 U+FFFD", () => {
    // 30 字节 ASCII + 3 字节「测」= 33 字节 → 固件侧截到 32 时「测」不完整
    const name = "ab".repeat(15) + "测"; // 30 + 3 = 33B
    const enc = Array.from(new TextEncoder().encode(name)).slice(0, 32); // 截 32B
    const bytes = [0];
    bytes.push(1, 1, 2, 3, 4, 5, 6, 0, ...enc); // paired+addr+type = 8B 前缀
    while (bytes.length < 121) bytes.push(0);
    const data = parseBleSlotGet(bytes)!;
    expect(data.slots[0].name).not.toContain("\uFFFD");
    expect(data.slots[0].name.startsWith("ab")).toBe(true);
  });
});

describe("fmtBleAddr", () => {
  it("NimBLE 小端 val 按大端倒序冒号显示", () => {
    // val = [0x34,0x12,0x0a,0x1f,0xeb,0xd3] → 显示 D3:EB:1F:0A:12:34
    expect(fmtBleAddr([0x34, 0x12, 0x0a, 0x1f, 0xeb, 0xd3])).toBe("D3:EB:1F:0A:12:34");
  });

  it("单字节补零", () => {
    expect(fmtBleAddr([1, 2, 3, 4, 5, 0x0c])).toBe("0C:05:04:03:02:01");
  });
});
