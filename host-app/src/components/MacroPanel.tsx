import type { Dispatch, SetStateAction } from "react";
import { MACRO_ACT, MACRO_STEP_MAX } from "../protocol";
import { KEY_GROUPS, CODE_TO_LABEL } from "../keycodes";
import type { KeyDef } from "../keycodes";
import type { MacroData, MacroStep } from "../types";

// 步骤动作 → 显示文本
const STEP_ACTION_LABEL = ["按下", "松开", "延迟", "轻点"];

// 步骤动作 → 类型色（DOWN=蓝 / UP=红 / DELAY=琥珀 / TAP=绿）
const STEP_ACTION_COLOR = ["bg-blue-500", "bg-red-500", "bg-amber-500", "bg-emerald-500"];

// 宏步骤可选键码目录（全部分组扁平化；不含 None 与宏触发键，避免嵌套触发）
const STEP_KEY_OPTIONS: KeyDef[] = KEY_GROUPS.flatMap((g) => g.keys).filter(
  (k) => k.code !== 0x00 && !(k.code >= 0xf0 && k.code <= 0xf3)
);

interface MacroPanelProps {
  cfgConnected: boolean;
  macros: (MacroData | null)[];
  macroTab: number;
  setMacroTab: Dispatch<SetStateAction<number>>;
  macroDraft: MacroData;
  setMacroDraft: Dispatch<SetStateAction<MacroData>>;
  draftSetSteps: (fn: (steps: MacroStep[]) => MacroStep[]) => void;
  onWriteMacro: (i: number, m: MacroData) => void;
  onFetchMacros: () => void;
  recording: boolean;
  recRemain: number;
  recMaxMs: number;
  setRecMaxMs: Dispatch<SetStateAction<number>>;
  recAvailable: boolean;
  onStartRecording: () => void;
  onStopRecording: (auto: boolean) => void;
}

// 宏编辑（P6：4 个槽位，步骤化编辑按下/松开/延迟/轻点；随 Profile 存 NVS）
export function MacroPanel({
  cfgConnected,
  macros,
  macroTab,
  setMacroTab,
  macroDraft,
  setMacroDraft,
  draftSetSteps,
  onWriteMacro,
  onFetchMacros,
  recording,
  recRemain,
  recMaxMs,
  setRecMaxMs,
  recAvailable,
  onStartRecording,
  onStopRecording,
}: MacroPanelProps) {
  const moveStep = (si: number, dir: -1 | 1) =>
    draftSetSteps((steps) => {
      const tj = si + dir;
      if (tj < 0 || tj >= steps.length) return steps;
      [steps[si], steps[tj]] = [steps[tj], steps[si]];
      return steps;
    });
  const removeStep = (si: number) =>
    draftSetSteps((steps) => steps.filter((_, j) => j !== si));
  const updateStep = (si: number, patch: Partial<MacroStep>) =>
    draftSetSteps((steps) => steps.map((s, j) => (j === si ? { ...s, ...patch } : s)));

  return (
    <div className="rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm">
      <div className="mb-3 flex items-center gap-2">
        <h2 className="text-sm font-semibold text-slate-500">宏编辑</h2>
        <button
          onClick={onFetchMacros}
          disabled={!cfgConnected}
          title="帧协议重新读取 4 个宏定义"
          className="ml-auto rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1 text-xs font-medium text-slate-600 hover:bg-slate-100 disabled:opacity-40"
        >
          读取
        </button>
      </div>
      {/* 槽位页签 */}
      <div className="mb-3 flex gap-1.5">
        {[0, 1, 2, 3].map((i) => (
          <button
            key={i}
            onClick={() => setMacroTab(i)}
            className={`flex-1 rounded-md border px-1 py-1 text-xs font-medium transition-colors ${
              macroTab === i
                ? "border-blue-500 bg-blue-500 text-white"
                : "border-slate-200 bg-slate-50 text-slate-600 hover:border-blue-300"
            }`}
          >
            {macros[i]?.name || `宏 ${i + 1}`}
          </button>
        ))}
      </div>
      {/* 宏名称 */}
      <input
        value={macroDraft.name}
        onChange={(e) => setMacroDraft((d) => ({ ...d, name: e.target.value.slice(0, 7) }))}
        placeholder="宏名称（7 字节内）"
        className="mb-2 w-full rounded-md border border-slate-300 px-2 py-1 text-sm focus:border-blue-400 focus:outline-none"
      />
      {/* 步骤列表 */}
      <div className="mb-2 max-h-56 space-y-1 overflow-y-auto">
        {macroDraft.steps.length === 0 && (
          <p className="rounded-md bg-slate-50 px-2 py-3 text-center text-xs text-slate-400">
            暂无步骤，点击下方按钮添加
          </p>
        )}
        {macroDraft.steps.map((s, si) => (
          <div key={si} className="flex items-center gap-1 rounded-md border border-slate-200 bg-slate-50 px-1.5 py-1">
            <span className={`w-1 shrink-0 self-stretch rounded-full ${STEP_ACTION_COLOR[s.action] ?? "bg-slate-300"}`} />
            <span className="w-5 pr-0.5 text-right font-mono text-[10px] tabular-nums text-slate-400">{si + 1}</span>
            <select
              value={s.action}
              onChange={(e) => updateStep(si, { action: Number(e.target.value) })}
              className="rounded border border-slate-300 bg-white px-1 py-0.5 text-xs"
            >
              {STEP_ACTION_LABEL.map((label, a) => (
                <option key={a} value={a}>{label}</option>
              ))}
            </select>
            {s.action === MACRO_ACT.DELAY ? (
              <input
                type="number"
                min={1}
                max={65535}
                value={s.delayMs}
                onChange={(e) =>
                  updateStep(si, { delayMs: Math.max(1, Math.min(65535, Number(e.target.value) || 1)) })
                }
                className="w-20 rounded border border-slate-300 bg-white px-1 py-0.5 font-mono text-xs"
              />
            ) : (
              <select
                value={s.keycode}
                onChange={(e) => updateStep(si, { keycode: Number(e.target.value) })}
                className="min-w-0 flex-1 rounded border border-slate-300 bg-white px-1 py-0.5 text-xs"
              >
                {STEP_KEY_OPTIONS.map((k) => (
                  <option key={k.code} value={k.code}>
                    {CODE_TO_LABEL.get(k.code) ?? k.label}
                  </option>
                ))}
              </select>
            )}
            {s.action === MACRO_ACT.DELAY && <span className="text-xs text-slate-400">ms</span>}
            <div className="ml-auto flex shrink-0 gap-0.5">
              <button onClick={() => moveStep(si, -1)} title="上移" className="rounded px-1 text-xs text-slate-400 hover:bg-slate-200 hover:text-slate-600">↑</button>
              <button onClick={() => moveStep(si, 1)} title="下移" className="rounded px-1 text-xs text-slate-400 hover:bg-slate-200 hover:text-slate-600">↓</button>
              <button onClick={() => removeStep(si)} title="删除" className="rounded px-1 text-xs text-red-400 hover:bg-red-50 hover:text-red-600">×</button>
            </div>
          </div>
        ))}
      </div>
      {/* 添加步骤 + 下发 */}
      <div className="flex flex-wrap items-center gap-1.5">
        <button
          onClick={() =>
            draftSetSteps((steps) =>
              steps.length < MACRO_STEP_MAX
                ? [...steps, { action: MACRO_ACT.TAP, keycode: 0x04, delayMs: 0 }]
                : steps
            )
          }
          disabled={macroDraft.steps.length >= MACRO_STEP_MAX}
          className="rounded-md border border-slate-200 bg-slate-50 px-2 py-1 text-xs text-slate-600 hover:bg-slate-100 disabled:opacity-40"
        >
          + 按键
        </button>
        <button
          onClick={() =>
            draftSetSteps((steps) =>
              steps.length < MACRO_STEP_MAX
                ? [...steps, { action: MACRO_ACT.DELAY, keycode: 0, delayMs: 100 }]
                : steps
            )
          }
          disabled={macroDraft.steps.length >= MACRO_STEP_MAX}
          className="rounded-md border border-slate-200 bg-slate-50 px-2 py-1 text-xs text-slate-600 hover:bg-slate-100 disabled:opacity-40"
        >
          + 延迟
        </button>
        <button
          onClick={() => onWriteMacro(macroTab, macroDraft)}
          disabled={!cfgConnected || macroDraft.steps.length === 0}
          className="ml-auto rounded-md border border-indigo-200 bg-indigo-50 px-2.5 py-1 text-xs font-medium text-indigo-600 hover:bg-indigo-100 disabled:opacity-40"
          title="把当前宏下发到键盘（RAM 生效；点「保存配置到键盘」持久化）"
        >
          下发宏 {macroTab + 1}
        </button>
      </div>
      {/* 宏录制（v2.2：帧 0x0E 开始/停止；键盘端抑制输出，[EVT] KEY 收集后回填草稿） */}
      <div className="mt-2 flex flex-wrap items-center gap-1.5 rounded-md border border-slate-200 bg-slate-50 px-2 py-1.5">
        {recording ? (
          <button
            onClick={() => void onStopRecording(false)}
            className="rounded-md bg-red-500 px-2.5 py-1 text-xs font-medium text-white hover:bg-red-600"
          >
            停止录制
          </button>
        ) : (
          <button
            onClick={() => void onStartRecording()}
            disabled={!cfgConnected || !recAvailable}
            title={
              recAvailable
                ? "点开始在键盘上录制按键（录制期间字符不会输出）"
                : "录制需串口或蓝牙通路（WiFi 模式 [EVT] 不经过界面）"
            }
            className="rounded-md border border-red-200 bg-red-50 px-2.5 py-1 text-xs font-medium text-red-600 hover:bg-red-100 disabled:opacity-40"
          >
            ● 录制
          </button>
        )}
        <select
          value={recMaxMs}
          onChange={(e) => setRecMaxMs(Number(e.target.value))}
          disabled={recording}
          className="rounded border border-slate-300 bg-white px-1 py-0.5 text-xs disabled:opacity-50"
          title="录制最长时长，到时自动停止"
        >
          <option value={5000}>最长 5s</option>
          <option value={10000}>最长 10s</option>
          <option value={30000}>最长 30s</option>
          <option value={60000}>最长 60s</option>
        </select>
        {recording && (
          <span className="animate-pulse text-xs font-medium text-red-500">
            录制中… 剩余 {recRemain}s，在键盘上按键即被录制
          </span>
        )}
      </div>
      <p className="mt-2 text-xs text-slate-400">
        轻点=按下 30ms 后松开；「按下」未配对「松开」时播放结束自动释放防卡键。USB/BLE 模式键盘本地播放，
        WiFi 模式步骤经 [EVT] MACRO 行推给上位机注入。最多 {MACRO_STEP_MAX} 步
      </p>
    </div>
  );
}
