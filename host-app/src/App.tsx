import { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen, UnlistenFn } from "@tauri-apps/api/event";
import {
  MATRIX_ROWS,
  MATRIX_COLS,
  PROFILE_COUNT,
  MACRO_COUNT,
  MACRO_STEP_MAX,
  parseEventLine,
  parseKeymapLine,
  parseBleSlotGet,
  hexOf,
  nameToBytes,
  macroToPayload,
  buildRecSteps,
} from "./protocol";
import { DEFAULT_KEYMAP } from "./keycodes";
import type { KeyDef } from "./keycodes";
import type {
  PortInfo,
  SerialEvent,
  ProfileInfo,
  BatteryStatus,
  MacroData,
  MacroStep,
  RecEvt,
  BleSlotsData,
} from "./types";
import { ConnectionBar } from "./components/ConnectionBar";
import type { PathTab } from "./components/ConnectionBar";
import { BindingPanel } from "./components/BindingPanel";
import { CommandPanel } from "./components/CommandPanel";
import { KeyGrid } from "./components/KeyGrid";
import { LogConsole } from "./components/LogConsole";
import { SideNav } from "./components/SideNav";
import type { Workspace } from "./components/SideNav";
import { ProfilePanel } from "./components/ProfilePanel";
import { KnobPanel } from "./components/KnobPanel";
import { MacroPanel } from "./components/MacroPanel";
import { BatteryCurve } from "./components/BatteryPanel";
import { MaintenancePanel } from "./components/MaintenancePanel";

export default function App() {
  // 串口连接状态
  const [ports, setPorts] = useState<PortInfo[]>([]);
  const [selectedPort, setSelectedPort] = useState("");
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);

  // 键盘状态
  const [keyStates, setKeyStates] = useState<boolean[][]>(
    Array.from({ length: MATRIX_ROWS }, () => Array(MATRIX_COLS).fill(false))
  );
  const [encCounters, setEncCounters] = useState({ cw: 0, ccw: 0, pressed: false, long: false });

  // 日志
  const [logLines, setLogLines] = useState<string[]>([]);
  const logEndRef = useRef<HTMLDivElement>(null);
  const customCmdRef = useRef<HTMLInputElement>(null);

  // 蓝牙监控（btleplug，与系统 HID 并存）
  const [bleStatus, setBleStatus] = useState<"idle" | "scanning" | "connected">("idle");
  const [bleConnecting, setBleConnecting] = useState(false);

  // WiFi 热点通路（P5 方案 A）：TCP 连接状态 + 连接中标记
  const [wifiConnected, setWifiConnected] = useState(false);
  const [wifiConnecting, setWifiConnecting] = useState(false);

  // 顶栏三通路分段选择器：当前展示哪个通路的连接控件
  const [pathTab, setPathTab] = useState<PathTab>("usb");
  // 左侧一级导航：当前工作区
  const [workspace, setWorkspace] = useState<Workspace>("layout");
  // 键位未保存改动标记（设备键位回显覆盖网格 / 保存成功时清除）
  const [dirty, setDirty] = useState(false);

  // 键位定义（第 0 层，选择器修改后经串口下发固件）
  const [keymapDefs, setKeymapDefs] = useState<number[][]>(DEFAULT_KEYMAP);
  // 当前选中键（同屏右侧键位绑定面板驱动；再次点击同键或关闭面板时置 null）
  const [selectedKey, setSelectedKey] = useState<{ row: number; col: number } | null>(null);
  // 按键释放延时定时器（BLE 事件批量到达时保证高亮最短可见）
  const releaseTimers = useRef(new Map<string, ReturnType<typeof setTimeout>>());

  // Profile 管理（P4）：3 个槽位数据 + 当前 Profile（1-3）
  const [profiles, setProfiles] = useState<ProfileInfo[]>([]);
  const [currentProfile, setCurrentProfile] = useState(1);
  // PROFILE_GET 请求序号标记（应答 0x86 归属哪个槽位）
  const profileGetIdx = useRef(0);

  // 电量（P4）：最新状态 + 曲线历史（最多 120 点，5s 一点）
  const [battery, setBattery] = useState<BatteryStatus | null>(null);
  const [battHistory, setBattHistory] = useState<BatteryStatus[]>([]);

  // 维护（P7）：电池两点校准草稿 + 休眠参数草稿 + esptool 刷写中标记
  const [calDraft, setCalDraft] = useState({ loMeas: 0, loTrue: 0, hiMeas: 0, hiTrue: 0 });
  const [sleepDraft, setSleepDraft] = useState({ lightMin: 5, deepHr: 24 });
  const [flashBusy, setFlashBusy] = useState(false);

  // BLE 配对槽位（v2.5）：连接建立时读取一次（0x13），清除配对成功后重读
  const [bleSlots, setBleSlots] = useState<BleSlotsData | null>(null);

  // 宏与旋钮（P6）
  const [macros, setMacros] = useState<(MacroData | null)[]>([]);
  const [macroTab, setMacroTab] = useState(0);
  // 宏编辑草稿（切页签/读取回填时重置；下发用）
  const [macroDraft, setMacroDraft] = useState<MacroData>({ name: "宏 1", steps: [] });
  const [knobCfg, setKnobCfg] = useState<{ cw: number; ccw: number; press: number } | null>(null);

  // 宏录制（v2.2：0x0E 开始/停止；[EVT] KEY 行收集，键盘端抑制 HID 输出）
  const [recording, setRecording] = useState(false);
  const [recRemain, setRecRemain] = useState(0);
  const [recMaxMs, setRecMaxMs] = useState(10000);
  const recBufRef = useRef<{
    slot: number;
    startedAt: number;
    evts: RecEvt[];
    last: Map<number, { down: boolean; t: number }>; // 每键码上一次事件（双通路去重用）
  } | null>(null);
  const recTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const recTickRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const appendLog = useCallback((line: string) => {
    setLogLines((prev) => {
      const next = [...prev, line];
      return next.length > 1000 ? next.slice(next.length - 1000) : next;
    });
  }, []);

  // 应用一条 [EVT] 事件（串口与蓝牙监控共用；ENC CW/CCW 同时闪烁音量指示）
  const applyEvent = useCallback((evt: SerialEvent) => {
    switch (evt.kind) {
      case "key": {
        // 宏录制中：收集按键事件（过滤 KC_NONE 与 Consumer/宏键码 0xC0+，修饰键 0xE0~0xE7 正常录制）
        // 串口与蓝牙监控同时在线时同一 [EVT] KEY 会到达两份（SEQ 去重只保护帧命令，文本行不防重），
        // 同键码同状态 50ms 内视为重复事件丢弃
        const rb = recBufRef.current;
        if (rb && typeof evt.kc === "number" && !isNaN(evt.kc) && evt.kc !== 0 && evt.kc < 0xc0) {
          const prev = rb.last.get(evt.kc);
          const now = Date.now();
          if (!prev || prev.down !== evt.down || now - prev.t >= 50) {
            rb.last.set(evt.kc, { down: evt.down, t: now });
            rb.evts.push({ down: evt.down, kc: evt.kc, t: now });
          }
        }
        const id = `${evt.row}-${evt.col}`;
        setKeyStates((prev) => {
          const next = prev.map((r) => [...r]);
          if (next[evt.row]) next[evt.row][evt.col] = evt.down;
          return next;
        });
        // BLE 通知批量到达时 DOWN/UP 几乎同帧，高亮一闪而过无法观察：
        // DOWN 立即点亮，UP 延时 250ms 熄灭（期间再次 DOWN 则取消熄灭）
        if (evt.down) {
          const t = releaseTimers.current.get(id);
          if (t) {
            clearTimeout(t);
            releaseTimers.current.delete(id);
          }
        } else {
          const t = setTimeout(() => {
            releaseTimers.current.delete(id);
            setKeyStates((prev) => {
              const next = prev.map((r) => [...r]);
              if (next[evt.row]) next[evt.row][evt.col] = false;
              return next;
            });
          }, 250);
          releaseTimers.current.set(id, t);
        }
        break;
      }
      case "enc":
        if (evt.action === "CW") setEncCounters((c) => ({ ...c, cw: c.cw + 1 }));
        else if (evt.action === "CCW") setEncCounters((c) => ({ ...c, ccw: c.ccw + 1 }));
        else if (evt.action === "PRESS") setEncCounters((c) => ({ ...c, pressed: true, long: false }));
        else if (evt.action === "RELEASE") setEncCounters((c) => ({ ...c, pressed: false }));
        else if (evt.action === "LONG") setEncCounters((c) => ({ ...c, long: true }));
        break;
      case "profile":
        // 旋钮短按切换 Profile 时同步 UI 当前槽位（1-3）
        setCurrentProfile(evt.index);
        break;
    }
  }, []);

  // 监听串口数据
  // listen 返回 Promise（权限被拒等异常时 reject），必须接 catch 否则错误被静默吞掉
  useEffect(() => {
    const unlisteners: UnlistenFn[] = [];
    // StrictMode 会「挂载→清理→再挂载」；listen 异步 resolve 若晚于清理，
    // 监听器会漏退订 → 每条事件/日志重复两份（历史「帧重复 2~4 次」根因），故取消后立即退订
    let cancelled = false;
    const safeListen = <T,>(name: string, cb: (payload: T) => void) => {
      listen<T>(name, (e) => cb(e.payload))
        .then((fn) => {
          if (cancelled) fn();
          else unlisteners.push(fn);
        })
        .catch((err) => appendLog(`[HOST] 事件监听失败 ${name}: ${err}`));
    };

    safeListen<string>("serial-line", (line) => {
      const evt = parseEventLine(line);
      if (evt) applyEvent(evt);
      // keymap 回显行：把设备当前键位绑定同步到键位网格
      const km = parseKeymapLine(line);
      if (km) {
        setKeymapDefs((prev) => {
          const next = prev.map((r) => [...r]);
          if (next[km.row]) next[km.row] = km.codes;
          return next;
        });
        setDirty(false); // 设备键位回显覆盖网格：设备状态=网格状态
      }
      appendLog(line);
    });

    // 读线程退出（拔线/端口错误）时同步前端状态
    safeListen<null>("serial-closed", () => {
      setConnected(false);
      appendLog("[HOST] 串口已断开（端口释放）");
    });

    // 蓝牙监控状态
    safeListen<string>("ble-status", (s) => {
      const st = s as "idle" | "scanning" | "connected";
      setBleStatus(st);
      appendLog(`[HOST] 蓝牙状态: ${st}`);
    });

    // 蓝牙监控事件（后端订阅固件自定义监控服务 0xFFE0/0xFFE1，二进制事件已转为 [EVT] 文本行）
    // 日志加 [BLE] 前缀与串口来源区分
    safeListen<string>("ble-evt", (line) => {
      const evt = parseEventLine(line);
      if (evt) applyEvent(evt);
      appendLog(`[BLE] ${line}`);
    });

    // WiFi 热点通路状态（P5：TCP 192.168.4.1:8266，[EVT] 由后端 SendInput 注入器转为真实输入）
    safeListen<{ tcp_connected: boolean }>("wifi-status", (s) => {
      setWifiConnected(s.tcp_connected);
      appendLog(
        s.tcp_connected
          ? "[HOST] WiFi 热点通路已连接（TCP 8266，输入经 SendInput 注入）"
          : "[HOST] WiFi 热点通路已断开（注入器已清空按键集）"
      );
    });

    // esptool 刷写/读取进度（P7 维护：flash_firmware / read_coredump 子进程输出逐行转发）
    safeListen<string>("esptool-progress", (line) => {
      appendLog(`[ESPTOOL] ${line}`);
    });

    // AA 55 二进制配置帧应答（P2 协议链路联调）
    safeListen<{ cmd: number; seq: number; payload_hex: string }>("cfg-frame", (f) => {
      appendLog(
        `[FRAME] cmd=0x${f.cmd.toString(16).padStart(2, "0")} seq=${f.seq} payload=${f.payload_hex}`
      );
      // KEYMAP_GET 应答（0x83）：20 字节行优先键位表，直接回显网格
      if (f.cmd === 0x83 && f.payload_hex) {
        const bytes = f.payload_hex.split(" ").map((h) => parseInt(h, 16));
        if (bytes.length === 20 && bytes.every((n) => !Number.isNaN(n))) {
          setKeymapDefs((prev) => {
            const next = prev.map((r) => [...r]);
            for (let r = 0; r < 5; r++) next[r] = bytes.slice(r * 4, r * 4 + 4);
            return next;
          });
          setDirty(false); // 设备键位回显覆盖网格：设备状态=网格状态
          appendLog("[HOST] 帧协议键位读取成功（已回显网格）");
        }
      }
      // KEYMAP_SET/SAVE 应答（0x84/0x85）：payload[0] 为状态码（0x00 成功）
      if (f.cmd === 0x84 || f.cmd === 0x85) {
        const st = f.payload_hex ? parseInt(f.payload_hex.split(" ")[0], 16) : NaN;
        if (st === 0) {
          if (f.cmd === 0x85) setDirty(false); // 保存成功：设备状态=网格状态
          appendLog(`[HOST] 帧协议${f.cmd === 0x84 ? "写单键" : "保存键位"}成功`);
        } else if (!Number.isNaN(st)) {
          appendLog(
            `[HOST] 帧协议${f.cmd === 0x84 ? "写单键" : "保存键位"}失败，状态码 0x${st
              .toString(16)
              .padStart(2, "0")}（见协议 §7.3 错误码）`
          );
        }
      }
      // PROFILE_GET 应答（0x86）：status + name(16B) + keymap(20B) + default_mode(1B) 共 38B
      if (f.cmd === 0x86 && f.payload_hex) {
        const bytes = f.payload_hex.split(" ").map((h) => parseInt(h, 16));
        if (bytes.length === 38) {
          const st = bytes[0];
          const idx = profileGetIdx.current;
          if (st === 0) {
            const name = new TextDecoder()
              .decode(new Uint8Array(bytes.slice(1, 17)))
              .replace(/\0[\s\S]*$/, "");
            const km: number[][] = [];
            for (let r = 0; r < MATRIX_ROWS; r++)
              km.push(bytes.slice(1 + 16 + r * MATRIX_COLS, 1 + 16 + (r + 1) * MATRIX_COLS));
            const info: ProfileInfo = {
              name: name || `Profile ${idx + 1}`,
              keymap: km,
              defaultMode: bytes[37],
            };
            setProfiles((prev) => {
              const next = [...prev];
              next[idx] = info;
              return next;
            });
          } else {
            appendLog(`[HOST] PROFILE_GET ${idx + 1} 失败，状态码 0x${st.toString(16)}`);
          }
        }
      }
      // PROFILE_SET 应答（0x87）：status
      if (f.cmd === 0x87 && f.payload_hex) {
        const st = parseInt(f.payload_hex.split(" ")[0], 16);
        if (st === 0) {
          appendLog(`[HOST] 帧协议切换 Profile 成功（已重读键位回显网格）`);
        } else if (!Number.isNaN(st)) {
          appendLog(`[HOST] 帧协议切换 Profile 失败，状态码 0x${st.toString(16).padStart(2, "0")}`);
        }
      }
      // PROFILE_WRITE 应答（0x88）：status
      if (f.cmd === 0x88 && f.payload_hex) {
        const st = parseInt(f.payload_hex.split(" ")[0], 16);
        if (st === 0) {
          appendLog("[HOST] 帧协议写 Profile 成功（NVS 已持久化）");
        } else if (!Number.isNaN(st)) {
          appendLog(`[HOST] 帧协议写 Profile 失败，状态码 0x${st.toString(16).padStart(2, "0")}`);
        }
      }
      // POWER_GET 应答（0x89）：mv(2B LE) + pct + flags（bit0 充电中 / bit1 已充满 / bit2 USB 在位）
      if (f.cmd === 0x89 && f.payload_hex) {
        const bytes = f.payload_hex.split(" ").map((h) => parseInt(h, 16));
        if (bytes.length === 4) {
          const st: BatteryStatus = {
            mv: bytes[0] | (bytes[1] << 8),
            pct: bytes[2],
            charging: (bytes[3] & 0x01) !== 0,
            full: (bytes[3] & 0x02) !== 0,
            usb: (bytes[3] & 0x04) !== 0,
          };
          setBattery(st);
          setBattHistory((prev) => [...prev.slice(-(120 - 1)), st]);
        }
      }
      // CAL_GET 应答（0x8F）：lo_meas/lo_true/hi_meas/hi_true 各 2B LE 共 8B（v2.4，P7）
      if (f.cmd === 0x8f && f.payload_hex) {
        const bytes = f.payload_hex.split(" ").map((h) => parseInt(h, 16));
        if (bytes.length === 8) {
          const u16 = (i: number) => bytes[i] | (bytes[i + 1] << 8);
          setCalDraft({ loMeas: u16(0), loTrue: u16(2), hiMeas: u16(4), hiTrue: u16(6) });
          appendLog("[HOST] 电池校准点读取成功（真实电压为 0 表示未校准）");
        }
      }
      // CAL_SET 应答（0x90）：status（0x00 成功，已写 NVS）
      if (f.cmd === 0x90 && f.payload_hex) {
        const st = parseInt(f.payload_hex.split(" ")[0], 16);
        appendLog(
          st === 0
            ? "[HOST] 电池校准点已写入（立即生效，NVS 持久化）"
            : `[HOST] 电池校准写入失败，状态码 0x${st.toString(16)}（0x01 长度 / 0x02 数值非法或高点不大于低点）`
        );
      }
      // SLEEP_GET 应答（0x91）：light_min + deep_hr 各 2B LE 共 4B
      if (f.cmd === 0x91 && f.payload_hex) {
        const bytes = f.payload_hex.split(" ").map((h) => parseInt(h, 16));
        if (bytes.length === 4) {
          setSleepDraft({
            lightMin: bytes[0] | (bytes[1] << 8),
            deepHr: bytes[2] | (bytes[3] << 8),
          });
          appendLog("[HOST] 休眠参数读取成功");
        }
      }
      // SLEEP_SET 应答（0x92）：status（0x00 成功，立即生效）
      if (f.cmd === 0x92 && f.payload_hex) {
        const st = parseInt(f.payload_hex.split(" ")[0], 16);
        appendLog(
          st === 0
            ? "[HOST] 休眠参数已写入（立即生效，NVS 持久化）"
            : `[HOST] 休眠参数写入失败，状态码 0x${st.toString(16)}（范围 light 1~120 分钟 / deep 1~168 小时）`
        );
      }
      // BLE_SLOT_GET 应答（0x93）：current_slot(1B) + 3×[paired+peer_addr(6B)+addr_type+name(32B)] 共 121B
      if (f.cmd === 0x93 && f.payload_hex) {
        const bytes = f.payload_hex.split(" ").map((h) => parseInt(h, 16));
        const data = parseBleSlotGet(bytes);
        if (data) {
          setBleSlots(data);
          const n = data.slots.filter((s) => s.paired).length;
          appendLog(`[HOST] BLE 配对槽位读取成功（当前槽位 ${data.currentSlot + 1}，${n}/3 已配对）`);
        }
      }
      // BLE_SLOT_CLEAR 应答（0x94）：status（0x00 成功；成功后自动重读槽位列表）
      if (f.cmd === 0x94 && f.payload_hex) {
        const st = parseInt(f.payload_hex.split(" ")[0], 16);
        appendLog(
          st === 0
            ? "[HOST] BLE 配对槽位已清除（对应设备需重新配对才能连接）"
            : `[HOST] BLE 槽位清除失败，状态码 0x${st.toString(16)}（0x01 槽位号非法 / 0x04 长度不符）`
        );
        if (st === 0) {
          void invoke("cfg_send", { cmd: 0x13, payloadHex: "" }).catch(() => {});
        }
      }
      // MACRO_GET 应答（0x8A）：idx(1B) + macro_t(137B) = 138B
      // macro_t = name(8B) + step_count(1B) + steps(32 × [action,keycode,delay_ms(2B LE)])
      // 字节布局：bytes[0]=idx，bytes[1..8]=name，bytes[9]=步数，bytes[10] 起每 4 字节一步
      if (f.cmd === 0x8a && f.payload_hex) {
        const bytes = f.payload_hex.split(" ").map((h) => parseInt(h, 16));
        if (bytes.length === 138) {
          const idx = bytes[0];
          const stepCount = Math.min(bytes[9], MACRO_STEP_MAX);
          const steps: MacroStep[] = [];
          for (let s = 0; s < stepCount; s++) {
            const o = 10 + s * 4;
            steps.push({
              action: bytes[o],
              keycode: bytes[o + 1],
              delayMs: bytes[o + 2] | (bytes[o + 3] << 8),
            });
          }
          const name = new TextDecoder()
            .decode(new Uint8Array(bytes.slice(1, 9)))
            .replace(/\0[\s\S]*$/, "");
          setMacros((prev) => {
            const next = [...prev];
            next[idx] = { name: name || `宏 ${idx + 1}`, steps };
            return next;
          });
          appendLog(`[HOST] 宏 ${idx + 1} 读取成功（${stepCount} 步）`);
        }
      }
      // MACRO_SET / KNOB_SET 应答（0x8B/0x8D）：status（0x00 成功）
      if ((f.cmd === 0x8b || f.cmd === 0x8d) && f.payload_hex) {
        const st = parseInt(f.payload_hex.split(" ")[0], 16);
        const what = f.cmd === 0x8b ? "宏写入" : "旋钮配置写入";
        appendLog(
          st === 0
            ? `[HOST] 帧协议${what}成功（RAM 生效，「保存配置到键盘」后写 NVS）`
            : `[HOST] 帧协议${what}失败，状态码 0x${st.toString(16).padStart(2, "0")}`
        );
      }
      // KNOB_GET 应答（0x8C）：cw(1B) + ccw(1B) + press(1B)；兼容旧固件 2B 应答（press 视为默认切 Profile）
      if (f.cmd === 0x8c && f.payload_hex) {
        const bytes = f.payload_hex.split(" ").map((h) => parseInt(h, 16));
        if (bytes.length === 3 || bytes.length === 2) {
          setKnobCfg({ cw: bytes[0], ccw: bytes[1], press: bytes[2] ?? 0xfe });
          appendLog("[HOST] 旋钮配置读取成功");
        }
      }
    });

    return () => {
      cancelled = true;
      unlisteners.forEach((fn) => fn());
    };
  }, [appendLog, applyEvent]);

  // 日志自动滚动
  useEffect(() => {
    logEndRef.current?.scrollIntoView({ behavior: "instant" });
  }, [logLines]);

  // 刷新端口列表
  const refreshPorts = useCallback(async () => {
    try {
      const list = await invoke<PortInfo[]>("list_ports");
      setPorts(list);
      if (list.length > 0 && !list.some((p) => p.name === selectedPort)) {
        setSelectedPort(list[0].name);
      }
    } catch (err) {
      appendLog(`[HOST] 刷新端口失败: ${err}`);
    }
  }, [appendLog, selectedPort]);

  useEffect(() => {
    refreshPorts();
  }, [refreshPorts]);

  // 串口 / 蓝牙监控 / WiFi TCP 任一通路已连上（帧协议通路判断；cfg_send 后端自动选择优先级）
  const cfgConnected = connected || bleStatus === "connected" || wifiConnected;

  // 宏录制仅支持串口/蓝牙通路（WiFi 模式 [EVT] 经 TCP 直达后端注入器，不经界面收集）
  const recAvailable = connected || bleStatus === "connected";

  // ==================== 宏与旋钮（P6） ====================

  // 读取 4 个宏定义（MACRO_GET 0x0A，应答 0x8A 逐个回填）
  const fetchMacros = useCallback(async () => {
    if (!cfgConnected) return;
    try {
      for (let i = 0; i < MACRO_COUNT; i++)
        await invoke("cfg_send", { cmd: 0x0a, payloadHex: hexOf([i]) });
      appendLog("[HOST] MACRO_GET ×4 已发送（等 [FRAME] cmd=0x8A 应答）");
    } catch (err) {
      appendLog(`[HOST] 读取宏失败: ${err}`);
    }
  }, [appendLog, cfgConnected]);

  // 写指定宏（MACRO_SET 0x0B，payload=idx+macro_t 138B；RAM 生效，0x05 后持久化）
  const writeMacro = useCallback(
    async (i: number, m: MacroData) => {
      try {
        await invoke("cfg_send", { cmd: 0x0b, payloadHex: hexOf(macroToPayload(i, m)) });
        appendLog(`> [帧 0x0B] 宏 ${i + 1}「${m.name}」已下发（${m.steps.length} 步）`);
      } catch (err) {
        appendLog(`[HOST] 宏下发失败: ${err}`);
      }
    },
    [appendLog]
  );

  // 读取旋钮配置（KNOB_GET 0x0C，应答 0x8C）
  const fetchKnob = useCallback(async () => {
    if (!cfgConnected) return;
    try {
      await invoke("cfg_send", { cmd: 0x0c, payloadHex: "" });
      appendLog("[HOST] KNOB_GET 已发送（等 [FRAME] cmd=0x8C 应答）");
    } catch (err) {
      appendLog(`[HOST] 读取旋钮配置失败: ${err}`);
    }
  }, [appendLog, cfgConnected]);

  // 写旋钮配置（KNOB_SET 0x0D，payload=[cw,ccw,press]；RAM 生效，0x05 后持久化）
  const writeKnob = useCallback(
    async (cw: number, ccw: number, press: number) => {
      try {
        await invoke("cfg_send", { cmd: 0x0d, payloadHex: hexOf([cw, ccw, press]) });
        appendLog(
          `> [帧 0x0D] 旋钮配置已下发（CW=0x${cw.toString(16)} CCW=0x${ccw.toString(16)} 按下=0x${press.toString(16)}）`
        );
      } catch (err) {
        appendLog(`[HOST] 旋钮配置下发失败: ${err}`);
      }
    },
    [appendLog]
  );

  // ==================== 宏录制（v2.2：帧 0x0E） ====================

  // 停止录制：下发 0x0E 停止 → 事件序列生成步骤 → 填入编辑草稿（超 32 步截断）
  const stopRecording = useCallback(
    async (auto: boolean) => {
      const rb = recBufRef.current;
      if (!rb) return;
      if (recTimerRef.current) {
        clearTimeout(recTimerRef.current);
        recTimerRef.current = null;
      }
      if (recTickRef.current) {
        clearInterval(recTickRef.current);
        recTickRef.current = null;
      }
      recBufRef.current = null;
      setRecording(false);
      try {
        await invoke("cfg_send", { cmd: 0x0e, payloadHex: hexOf([rb.slot, 1]) });
      } catch (err) {
        appendLog(`[HOST] 停止录制命令发送失败: ${err}`);
      }
      let steps = buildRecSteps(rb.evts, rb.startedAt);
      let truncated = false;
      if (steps.length > MACRO_STEP_MAX) {
        steps = steps.slice(0, MACRO_STEP_MAX);
        truncated = true;
      }
      setMacroTab(rb.slot);
      setMacroDraft((d) => ({ ...d, steps }));
      appendLog(
        `[HOST] ${auto ? "录制超时自动停止" : "录制已停止"}：宏 ${rb.slot + 1} 共 ${steps.length} 步` +
          `${truncated ? `（超过 ${MACRO_STEP_MAX} 步已截断）` : ""}，已填入编辑区，确认后点「下发宏」保存`
      );
    },
    [appendLog]
  );

  // 开始录制：下发 0x0E 开始 → 启动超时定时器与倒计时；之后 [EVT] KEY 事件由 applyEvent 收集
  const startRecording = useCallback(async () => {
    if (!cfgConnected || recBufRef.current) return;
    recBufRef.current = { slot: macroTab, startedAt: Date.now(), evts: [], last: new Map() };
    setRecording(true);
    setRecRemain(Math.round(recMaxMs / 1000));
    try {
      await invoke("cfg_send", { cmd: 0x0e, payloadHex: hexOf([macroTab, 0]) });
      appendLog(
        `[HOST] 录制开始（宏 ${macroTab + 1}，最长 ${Math.round(recMaxMs / 1000)}s）：在键盘上真实按键（不会打进当前窗口），点「停止录制」结束`
      );
    } catch (err) {
      recBufRef.current = null;
      setRecording(false);
      appendLog(`[HOST] 开始录制失败: ${err}`);
      return;
    }
    recTimerRef.current = setTimeout(() => void stopRecording(true), recMaxMs);
    recTickRef.current = setInterval(() => {
      setRecRemain((s) => (s > 0 ? s - 1 : 0));
    }, 1000);
  }, [appendLog, cfgConnected, macroTab, recMaxMs, stopRecording]);

  // 卸载时清理录制定时器
  useEffect(
    () => () => {
      if (recTimerRef.current) clearTimeout(recTimerRef.current);
      if (recTickRef.current) clearInterval(recTickRef.current);
    },
    []
  );

  // ==================== Profile 管理与电量（P4） ====================

  // 读取 3 个 Profile（PROFILE_GET 0x06，应答 0x86 逐个回填）
  const fetchProfiles = useCallback(async () => {
    if (!cfgConnected) {
      appendLog("[HOST] 未连接设备（串口/蓝牙/WiFi）");
      return;
    }
    try {
      for (let i = 0; i < PROFILE_COUNT; i++) {
        profileGetIdx.current = i;
        await invoke("cfg_send", { cmd: 0x06, payloadHex: hexOf([i]) });
      }
      appendLog("[HOST] PROFILE_GET ×3 已发送（等 [FRAME] cmd=0x86 应答）");
    } catch (err) {
      appendLog(`[HOST] 读取 Profile 失败: ${err}`);
    }
  }, [appendLog, cfgConnected]);

  // 写指定 Profile（PROFILE_WRITE 0x08：名称 + 键位，固件立即 NVS 持久化）
  const writeProfile = useCallback(async (i: number, name: string, keymap: number[][]) => {
    const payload = [i, ...nameToBytes(name), ...keymap.flat()];
    await invoke("cfg_send", { cmd: 0x08, payloadHex: hexOf(payload) });
  }, []);

  // 切换 Profile（PROFILE_SET 0x07：固件重载键位 + LED 快闪 N 次；随后重读键位回显网格）
  const switchProfile = useCallback(
    async (i: number) => {
      if (!cfgConnected) return;
      setCurrentProfile(i + 1);
      try {
        await invoke("cfg_send", { cmd: 0x07, payloadHex: hexOf([i]) });
        appendLog(`> [帧 0x07] 切换到 Profile ${i + 1}`);
        await invoke("cfg_send", { cmd: 0x03, payloadHex: "" });
        // Profile 切换会整体重载键位/旋钮/宏，同步回读刷新 UI
        await fetchMacros();
        await fetchKnob();
      } catch (err) {
        appendLog(`[HOST] 切换 Profile 失败: ${err}`);
      }
    },
    [appendLog, cfgConnected, fetchMacros, fetchKnob]
  );

  // 重命名 Profile（写回原名键位 + 新名称）
  const renameProfile = useCallback(
    (i: number, name: string) => {
      const p = profiles[i];
      const trimmed = name.trim();
      if (!p || !trimmed || trimmed === p.name || !cfgConnected) return;
      appendLog(`> [帧 0x08] Profile ${i + 1} 重命名为「${trimmed}」`);
      writeProfile(i, trimmed, p.keymap).catch((err) =>
        appendLog(`[HOST] 重命名失败: ${err}`)
      );
      setProfiles((prev) => {
        const next = [...prev];
        next[i] = { ...p, name: trimmed };
        return next;
      });
    },
    [appendLog, cfgConnected, profiles, writeProfile]
  );

  // 导出 3 个 Profile 为 JSON 文件（需先点「读取」拿到数据）
  const exportProfiles = useCallback(async () => {
    if (profiles.length < PROFILE_COUNT || profiles.some((p) => !p)) {
      appendLog("[HOST] 请先点「读取」获取 3 个 Profile 再导出");
      return;
    }
    const json = JSON.stringify(
      {
        app: "SimpleKey",
        version: 1,
        exported_at: new Date().toISOString(),
        profiles: profiles.map((p, i) => ({ index: i, name: p.name, keymap: p.keymap })),
      },
      null,
      2
    );
    try {
      const path = await invoke<string>("export_profile_json", { content: json });
      appendLog(`[HOST] 已导出 ${PROFILE_COUNT} 个 Profile → ${path}`);
    } catch (err) {
      if (String(err) !== "已取消") appendLog(`[HOST] 导出失败: ${err}`);
    }
  }, [appendLog, profiles]);

  // 导入 JSON 并逐 Profile 写入键盘（PROFILE_WRITE）
  const importProfiles = useCallback(async () => {
    try {
      const [path, content] = await invoke<[string, string]>("import_profile_json");
      const data = JSON.parse(content) as {
        profiles?: { index?: number; name?: string; keymap?: number[][] }[];
      };
      const arr = data?.profiles;
      if (!Array.isArray(arr) || arr.length !== PROFILE_COUNT) {
        appendLog("[HOST] 导入失败：JSON 需包含 3 个元素的 profiles 数组");
        return;
      }
      let ok = 0;
      for (const item of arr) {
        const idx = Number(item.index);
        const km = item.keymap;
        if (
          !(idx >= 0 && idx < PROFILE_COUNT) ||
          !Array.isArray(km) ||
          km.length !== MATRIX_ROWS ||
          !km.every((r) => Array.isArray(r) && r.length === MATRIX_COLS)
        ) {
          continue;
        }
        const keymap = km.map((r) => r.map((v) => Number(v) & 0xff));
        const name = String(item.name ?? `Profile ${idx + 1}`);
        await writeProfile(idx, name, keymap);
        setProfiles((prev) => {
          const next = [...prev];
          next[idx] = { name, keymap, defaultMode: next[idx]?.defaultMode ?? 1 };
          return next;
        });
        ok++;
      }
      appendLog(`[HOST] 导入完成（${ok}/${PROFILE_COUNT} 个 Profile）: ${path}`);
      // 当前 Profile 被覆盖时重读键位回显网格
      if (ok > 0) await invoke("cfg_send", { cmd: 0x03, payloadHex: "" });
    } catch (err) {
      if (String(err) !== "已取消") appendLog(`[HOST] 导入失败: ${err}`);
    }
  }, [appendLog, writeProfile]);

  // 电量 5s 轮询（连接期间持续；断开清空历史）
  useEffect(() => {
    if (!cfgConnected) {
      setBattery(null);
      setBattHistory([]);
      return;
    }
    const poll = () =>
      invoke("cfg_send", { cmd: 0x09, payloadHex: "" }).catch(() => {});
    poll();
    const t = setInterval(poll, 5000);
    return () => clearInterval(t);
  }, [cfgConnected]);

  // ==================== 设备维护（P7） ====================

  // 维护参数读取：CAL_GET 0x0F + SLEEP_GET 0x11（应答回填草稿）
  const fetchMaint = useCallback(async () => {
    await invoke("cfg_send", { cmd: 0x0f, payloadHex: "" }).catch(() => {});
    await invoke("cfg_send", { cmd: 0x11, payloadHex: "" }).catch(() => {});
  }, []);

  // BLE 配对槽位读取（SLOT_GET 0x13）：连接建立时拉取一次，不轮询（自动化轮询不得计入睡眠活动源）
  const fetchBleSlots = useCallback(async () => {
    await invoke("cfg_send", { cmd: 0x13, payloadHex: "" }).catch(() => {});
  }, []);

  // 连接 / 断开
  const toggleConnect = useCallback(async () => {
    if (connected) {
      try {
        await invoke("close_port");
        setConnected(false);
        appendLog("[HOST] 已断开");
      } catch (err) {
        appendLog(`[HOST] 断开失败: ${err}`);
      }
    } else {
      setConnecting(true);
      try {
        await invoke("open_port", { name: selectedPort, baud: 115200 });
        setConnected(true);
        appendLog(`[HOST] 已连接 ${selectedPort} @ 115200`);
        // 连接后自动读取设备当前键位绑定，回显到键位网格
        try {
          await invoke("send_line", { line: "keymap" });
          appendLog("[HOST] 已请求设备键位（回显到网格）");
        } catch {
          /* 首次读取失败不影响连接 */
        }
        // AA 55 帧协议握手（P2）：PING 重同步 SEQ → 二进制读键位表
        try {
          await invoke("cfg_ping");
          await invoke("cfg_send", { cmd: 0x03, payloadHex: "" });
          appendLog("[HOST] 帧协议 PING + KEYMAP_GET 已发送");
        } catch (err) {
          appendLog(`[HOST] 帧协议握手失败: ${err}`);
        }
        // P4：握手完成后读取 3 个 Profile（名称/键位/默认模式）；P6：宏与旋钮配置；P7：校准与休眠参数；v2.5：BLE 槽位
        await fetchProfiles();
        await fetchMacros();
        await fetchKnob();
        await fetchMaint();
        await fetchBleSlots();
      } catch (err) {
        appendLog(`[HOST] 连接失败: ${err}`);
      } finally {
        setConnecting(false);
      }
    }
  }, [appendLog, connected, selectedPort, fetchProfiles, fetchMacros, fetchKnob, fetchMaint, fetchBleSlots]);

  // 发送命令
  const sendCmd = useCallback(
    async (cmd: string) => {
      if (!connected) {
        appendLog("[HOST] 未连接串口");
        return;
      }
      try {
        await invoke("send_line", { line: cmd });
        appendLog(`> ${cmd}`);
      } catch (err) {
        appendLog(`[HOST] 发送失败: ${err}`);
      }
    },
    [appendLog, connected]
  );

  const handleCustomCmd = useCallback(
    (e: React.KeyboardEvent<HTMLInputElement>) => {
      if (e.key === "Enter") {
        const cmd = customCmdRef.current?.value.trim();
        if (cmd) {
          sendCmd(cmd);
          if (customCmdRef.current) customCmdRef.current.value = "";
        }
      }
    },
    [sendCmd]
  );

  // 蓝牙监控连接 / 断开（监控用 GATT 订阅，与系统 HID 连接并存互不影响）
  const toggleBle = useCallback(async () => {
    if (bleStatus === "connected") {
      try {
        await invoke("ble_disconnect");
        setBleStatus("idle");
        appendLog("[HOST] 蓝牙监控已断开");
      } catch (err) {
        appendLog(`[HOST] 蓝牙断开失败: ${err}`);
      }
    } else {
      setBleConnecting(true);
      setBleStatus("scanning");
      try {
        const r = await invoke<string>("ble_scan_connect");
        setBleStatus("connected");
        appendLog(r === "already" ? "[HOST] 蓝牙监控：设备已被监控" : "[HOST] 蓝牙监控已连接");
        // 蓝牙配置通路握手（P2）：监控连接后自动 PING 重同步 SEQ + 读键位
        // （cfg_send 无串口连接时自动走 BLE Write，见 main.rs ble_write_frame）
        if (r !== "already") {
          try {
            await invoke("cfg_ping");
            await invoke("cfg_send", { cmd: 0x03, payloadHex: "" });
            appendLog("[HOST] 帧协议 PING + KEYMAP_GET 已发送（BLE 通路）");
          } catch (err) {
            appendLog(`[HOST] BLE 帧协议握手失败: ${err}`);
          }
          // P4：握手完成后读取 3 个 Profile；P6：宏与旋钮配置；v2.5：BLE 槽位
          await fetchProfiles();
          await fetchMacros();
          await fetchKnob();
          await fetchBleSlots();
        }
      } catch (err) {
        setBleStatus("idle");
        appendLog(`[HOST] 蓝牙连接失败: ${err}`);
      } finally {
        setBleConnecting(false);
      }
    }
  }, [appendLog, bleStatus, fetchProfiles, fetchMacros, fetchKnob, fetchBleSlots]);

  // WiFi 热点连接 / 断开（P5 方案 A：netsh 连 SimpleKey-* 热点 + TCP 8266 + SendInput 注入）
  const toggleWifi = useCallback(async () => {
    if (wifiConnected) {
      try {
        await invoke("wifi_disconnect");
        // 状态由 wifi-status(false) 事件同步
      } catch (err) {
        appendLog(`[HOST] WiFi 断开失败: ${err}`);
      }
    } else {
      setWifiConnecting(true);
      try {
        const ssid = await invoke<string>("wifi_connect");
        appendLog(`[HOST] WiFi 已连接热点 ${ssid}（TCP 通路建立中...）`);
        // WiFi 配置通路握手（P5）：TCP 建立后 PING 重同步 SEQ + 读键位 + Profile
        // （cfg_send 无串口连接且 NET 存活时自动走 WiFi TCP，见 main.rs cfg_send）
        try {
          await invoke("cfg_ping");
          await invoke("cfg_send", { cmd: 0x03, payloadHex: "" });
          appendLog("[HOST] 帧协议 PING + KEYMAP_GET 已发送（WiFi 通路）");
          await fetchProfiles();
          await fetchMacros();
          await fetchKnob();
          await fetchMaint();
          await fetchBleSlots();
        } catch (err) {
          appendLog(`[HOST] WiFi 帧协议握手失败: ${err}`);
        }
        // 连接成功状态由 wifi-status 事件同步
      } catch (err) {
        appendLog(`[HOST] WiFi 连接失败: ${err}`);
      } finally {
        setWifiConnecting(false);
      }
    }
  }, [appendLog, wifiConnected, fetchProfiles, fetchMacros, fetchKnob, fetchMaint, fetchBleSlots]);

  // 绑定面板选定键码：更新本地键位定义，串口已连接时立即下发固件（RAM 生效）；面板保持打开方便连续改键
  const pickKey = useCallback(
    (k: KeyDef) => {
      if (!selectedKey) return;
      const { row, col } = selectedKey;
      setKeymapDefs((prev) => {
        const next = prev.map((r) => [...r]);
        next[row][col] = k.code;
        return next;
      });
      setDirty(true); // 键位已修改待保存（KEYMAP_SAVE 写 NVS）
      const hex = `0x${k.code.toString(16).padStart(2, "0").toUpperCase()}`;
      if (connected) {
        invoke("send_line", { line: `keymap set ${row} ${col} ${hex}` }).catch((err) =>
          appendLog(`[HOST] 下发失败: ${err}`)
        );
        appendLog(`> keymap set ${row} ${col} ${hex}`);
      } else if (bleStatus === "connected" || wifiConnected) {
        // 蓝牙 / WiFi 配置通路：KEYMAP_SET 帧（0x04，payload=[row,col,keycode]，RAM 生效）
        // （cfg_send 后端自动路由：串口 → NET → BLE）
        const payload = [row, col, k.code]
          .map((n) => n.toString(16).padStart(2, "0"))
          .join("");
        invoke("cfg_send", { cmd: 0x04, payloadHex: payload }).catch((err) =>
          appendLog(`[HOST] 帧下发失败: ${err}`)
        );
        appendLog(`> [帧 0x04] keymap set ${row} ${col} ${hex}`);
      } else {
        appendLog("[HOST] 未连接设备（串口/蓝牙/WiFi），键位仅保存在本地");
      }
    },
    [appendLog, connected, bleStatus, wifiConnected, selectedKey]
  );

  // 清除绑定：下发 0x00（KC_NO），复用 pickKey 通路
  const clearKey = useCallback(() => {
    pickKey({ code: 0x00, label: "None", name: "KC_NO" });
  }, [pickKey]);

  // 网格键帽点击：选中键（再次点击同键取消选中，面板收起）
  const handleKeyClick = useCallback((row: number, col: number) => {
    setSelectedKey((prev) => (prev && prev.row === row && prev.col === col ? null : { row, col }));
  }, []);

  // 读取键位（帧协议 KEYMAP_GET 0x03：串口/蓝牙通路自动选择，0x83 应答回显网格）
  const readKeymap = useCallback(async () => {
    try {
      await invoke("cfg_send", { cmd: 0x03, payloadHex: "" });
      appendLog("[HOST] KEYMAP_GET 已发送（等 [FRAME] cmd=0x83 应答回显）");
    } catch (err) {
      appendLog(`[HOST] 读取键位失败: ${err}`);
    }
  }, [appendLog]);

  // 保存键位到键盘（帧协议 KEYMAP_SAVE 0x05：写当前 Profile NVS，0x85 应答确认）
  const saveKeymap = useCallback(async () => {
    try {
      await invoke("cfg_send", { cmd: 0x05, payloadHex: "" });
      appendLog("[HOST] KEYMAP_SAVE 已发送（等 [FRAME] cmd=0x85 应答确认）");
    } catch (err) {
      appendLog(`[HOST] 保存键位失败: ${err}`);
    }
  }, [appendLog]);

  // ==================== 设备维护（P7） ====================

  // 电池两点校准写入（CAL_SET 0x10：lo_meas/lo_true/hi_meas/hi_true 各 u16 LE）
  const saveCal = useCallback(async () => {
    const c = calDraft;
    // 实测值 500~4500（固件分压量程）；真实值 0~4500（0=该点未校准哨兵）
    const measOk = (v: number) => Number.isFinite(v) && v >= 500 && v <= 4500;
    const trueOk = (v: number) => Number.isFinite(v) && v >= 0 && v <= 4500;
    if (!measOk(c.loMeas) || !measOk(c.hiMeas) || !trueOk(c.loTrue) || !trueOk(c.hiTrue)) {
      appendLog("[HOST] 校准值非法（实测电压 500~4500 mV，真实电压 0~4500，0=未校准）");
      return;
    }
    if (c.hiMeas <= c.loMeas) {
      appendLog("[HOST] 高点实测电压必须大于低点实测电压");
      return;
    }
    // u16 小端打包：协议 v2.4 与固件均按 LE 逐字节解析
    const hex = [c.loMeas, c.loTrue, c.hiMeas, c.hiTrue]
      .flatMap((v) => [Math.round(v) & 0xff, (Math.round(v) >> 8) & 0xff])
      .map((b) => b.toString(16).padStart(2, "0"))
      .join("");
    try {
      await invoke("cfg_send", { cmd: 0x10, payloadHex: hex });
    } catch (err) {
      appendLog(`[HOST] 校准写入失败: ${err}`);
    }
  }, [appendLog, calDraft]);

  // 休眠参数写入（SLEEP_SET 0x12：light_min + deep_hr 各 u16 LE）
  const saveSleep = useCallback(async () => {
    const { lightMin, deepHr } = sleepDraft;
    if (lightMin < 1 || lightMin > 120 || deepHr < 1 || deepHr > 168) {
      appendLog("[HOST] 休眠参数非法（light 1~120 分钟 / deep 1~168 小时）");
      return;
    }
    // u16 小端打包：协议 v2.4 与固件均按 LE 逐字节解析
    const hex = [lightMin, deepHr]
      .flatMap((v) => [Math.round(v) & 0xff, (Math.round(v) >> 8) & 0xff])
      .map((b) => b.toString(16).padStart(2, "0"))
      .join("");
    try {
      await invoke("cfg_send", { cmd: 0x12, payloadHex: hex });
    } catch (err) {
      appendLog(`[HOST] 休眠参数写入失败: ${err}`);
    }
  }, [appendLog, sleepDraft]);

  // BLE 配对槽位清除（SLOT_CLEAR 0x14）：slot 0~2=单槽 / 0xFF=全部 3 槽；
  // 应答成功后由 0x94 事件分支自动重读槽位列表
  const clearBleSlot = useCallback(
    async (slot: number) => {
      const label = slot === 0xff ? "全部配对槽位" : `配对槽位 ${slot + 1}`;
      if (!window.confirm(`确认清除 BLE ${label} 的配对绑定？对应设备需重新配对才能连接键盘。`)) {
        return;
      }
      try {
        await invoke("cfg_send", { cmd: 0x14, payloadHex: hexOf([slot]) });
        appendLog(`[HOST] BLE ${label}清除指令已发送（等 [FRAME] cmd=0x94 应答）`);
      } catch (err) {
        appendLog(`[HOST] BLE ${label}清除失败: ${err}`);
      }
    },
    [appendLog]
  );

  // 固件刷写：先释放串口（esptool 需独占端口），bin=仅刷应用 / build 目录=完整刷写
  const doFlash = useCallback(async () => {
    let p: string;
    try {
      p = await invoke<string>("pick_firmware");
    } catch {
      return; // 用户取消
    }
    setFlashBusy(true);
    try {
      if (connected) {
        await invoke("close_port");
        setConnected(false);
        appendLog("[HOST] 已断开串口（刷写需要独占端口）");
      }
      appendLog(`[HOST] 开始刷写: ${p}`);
      await invoke("flash_firmware", { port: selectedPort, binPath: p });
      appendLog("[HOST] 刷写完成，请重新连接（PID 变更后 Windows 会重新枚举）");
    } catch (err) {
      if (String(err) !== "已取消") appendLog(`[HOST] 刷写失败: ${err}`);
    } finally {
      setFlashBusy(false);
    }
  }, [appendLog, connected, selectedPort]);

  // 读取崩溃日志：释放串口后 esptool read_flash 读出 coredump 分区（0x1F0000，64KB）存为 .bin
  const doReadCoredump = useCallback(async () => {
    setFlashBusy(true);
    try {
      if (connected) {
        await invoke("close_port");
        setConnected(false);
        appendLog("[HOST] 已断开串口（读取需要独占端口）");
      }
      const out = await invoke<string>("read_coredump", { port: selectedPort });
      appendLog(`[HOST] coredump 已保存: ${out}（可用 esp-coredump 解析崩溃现场）`);
    } catch (err) {
      if (String(err) !== "已取消") appendLog(`[HOST] 读取崩溃日志失败: ${err}`);
    } finally {
      setFlashBusy(false);
    }
  }, [appendLog, connected, selectedPort]);

  // 宏编辑草稿：切页签 / 读取回填时从 macros 同步（编辑中下发不回填，不打断编辑）
  useEffect(() => {
    const m = macros[macroTab];
    setMacroDraft(
      m ? { name: m.name, steps: m.steps.map((s) => ({ ...s })) } : { name: `宏 ${macroTab + 1}`, steps: [] }
    );
  }, [macroTab, macros]);

  // 草稿步骤批量操作辅助
  const draftSetSteps = useCallback(
    (fn: (steps: MacroStep[]) => MacroStep[]) =>
      setMacroDraft((d) => ({ ...d, steps: fn([...d.steps]) })),
    []
  );

  // ==================== 渲染 ====================

  return (
    <div className="flex h-full flex-col text-slate-800">
      <div className="relative flex min-h-0 flex-1 flex-col">
      <ConnectionBar
        pathTab={pathTab}
        setPathTab={setPathTab}
        connected={connected}
        connecting={connecting}
        ports={ports}
        selectedPort={selectedPort}
        setSelectedPort={setSelectedPort}
        onRefreshPorts={refreshPorts}
        onToggleConnect={toggleConnect}
        bleStatus={bleStatus}
        bleConnecting={bleConnecting}
        onToggleBle={toggleBle}
        wifiConnected={wifiConnected}
        wifiConnecting={wifiConnecting}
        onToggleWifi={toggleWifi}
        battery={battery}
        dirty={dirty}
        cfgConnected={cfgConnected}
        onReadKeymap={readKeymap}
        onSaveKeymap={saveKeymap}
      />

      <div className="flex min-h-0 flex-1">
        <SideNav workspace={workspace} setWorkspace={setWorkspace} />

        <div className="relative min-w-0 flex-1">
          {/* 背景图（黑白希·礼堂）限定在内容区：顶栏/左栏/底部日志台之外。
              100%×100% 拉伸铺满（完整显示不裁剪，长宽比随窗口轻微形变），
              10% 白雾轻微压暗保证文字可读 */}
          <div
            className="pointer-events-none absolute inset-0"
            style={{ backgroundImage: "url(/bg.jpg)", backgroundSize: "100% 100%" }}
          />
          <div className="pointer-events-none absolute inset-0 bg-white/10" />

          <main className="absolute inset-0 overflow-y-auto p-4">
          {workspace === "layout" && (
            <div className="mx-auto w-full max-w-4xl">
              {/* 布局页两栏：左列网格/电量曲线，右列键位绑定面板（窄窗口落到网格下方） */}
              <div className="flex flex-col gap-4 lg:flex-row lg:items-start">
                <div className="min-w-0 flex-1">
                  {/* 编码器状态条 */}
                  <div className="mb-4 flex flex-wrap items-center gap-x-6 gap-y-2 rounded-xl border border-slate-200 bg-white/70 p-3 backdrop-blur-sm">
                    <span className="text-sm font-semibold text-slate-500">编码器</span>
                    <div className="flex gap-6 text-sm">
                      <span>
                        顺时针 <b className="text-emerald-600">{encCounters.cw}</b>
                      </span>
                      <span>
                        逆时针 <b className="text-amber-600">{encCounters.ccw}</b>
                      </span>
                      <span
                        className={encCounters.pressed ? "font-bold text-blue-600" : "text-slate-400"}
                      >
                        按下{encCounters.long && " (长按)"}
                      </span>
                    </div>
                  </div>

                  <KeyGrid
                    keymapDefs={keymapDefs}
                    keyStates={keyStates}
                    selectedKey={selectedKey}
                    onKeyClick={handleKeyClick}
                  />

                  <BatteryCurve battHistory={battHistory} />
                </div>

                {selectedKey && (
                  <BindingPanel
                    row={selectedKey.row}
                    col={selectedKey.col}
                    code={keymapDefs[selectedKey.row]?.[selectedKey.col] ?? 0}
                    onPick={pickKey}
                    onClear={clearKey}
                    onClose={() => setSelectedKey(null)}
                  />
                )}
              </div>
            </div>
          )}

          {workspace === "profile" && (
            <div className="mx-auto w-full max-w-4xl">
              <ProfilePanel
                profiles={profiles}
                currentProfile={currentProfile}
                cfgConnected={cfgConnected}
                onSwitchProfile={switchProfile}
                onRenameProfile={renameProfile}
                onFetchProfiles={fetchProfiles}
                onExportProfiles={exportProfiles}
                onImportProfiles={importProfiles}
              />
            </div>
          )}

          {workspace === "knob" && (
            <div className="mx-auto w-full max-w-4xl">
              <KnobPanel
                knobCfg={knobCfg}
                setKnobCfg={setKnobCfg}
                onWriteKnob={writeKnob}
                onFetchKnob={fetchKnob}
                cfgConnected={cfgConnected}
              />
            </div>
          )}

          {workspace === "macro" && (
            <div className="mx-auto w-full max-w-4xl">
              <MacroPanel
                cfgConnected={cfgConnected}
                macros={macros}
                macroTab={macroTab}
                setMacroTab={setMacroTab}
                macroDraft={macroDraft}
                setMacroDraft={setMacroDraft}
                draftSetSteps={draftSetSteps}
                onWriteMacro={writeMacro}
                onFetchMacros={fetchMacros}
                recording={recording}
                recRemain={recRemain}
                recMaxMs={recMaxMs}
                setRecMaxMs={setRecMaxMs}
                recAvailable={recAvailable}
                onStartRecording={startRecording}
                onStopRecording={stopRecording}
              />
            </div>
          )}

          {workspace === "settings" && (
            <div className="mx-auto w-full max-w-4xl">
              {/* 命令控制台（调试）：与维护卡片同款图标章头 */}
              <div className="mb-4 rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm">
                <div className="mb-3 flex items-center gap-2">
                  <span className="grid h-6 w-6 place-items-center rounded-md bg-slate-100 text-xs font-bold text-slate-500">
                    调
                  </span>
                  <h2 className="text-sm font-semibold text-slate-700">命令控制台（调试）</h2>
                </div>
                <CommandPanel
                  connected={connected}
                  onSendCmd={sendCmd}
                  customCmdRef={customCmdRef}
                  onCustomCmd={handleCustomCmd}
                />
              </div>
              <MaintenancePanel
                cfgConnected={cfgConnected}
                connected={connected}
                battery={battery}
                calDraft={calDraft}
                setCalDraft={setCalDraft}
                onSaveCal={saveCal}
                onFetchMaint={fetchMaint}
                sleepDraft={sleepDraft}
                setSleepDraft={setSleepDraft}
                onSaveSleep={saveSleep}
                bleSlots={bleSlots}
                onClearBleSlot={clearBleSlot}
                onFetchBleSlots={fetchBleSlots}
                flashBusy={flashBusy}
                onFlash={doFlash}
                onReadCoredump={doReadCoredump}
              />
            </div>
          )}
        </main>
        </div>
      </div>

      <LogConsole logLines={logLines} onClear={() => setLogLines([])} logEndRef={logEndRef} />
      </div>
    </div>
  );
}
