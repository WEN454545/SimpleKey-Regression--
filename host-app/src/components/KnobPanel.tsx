import type { Dispatch, SetStateAction } from "react";

// 旋钮可配置输出（Consumer 功能，与固件 KC_CONSUMER 0xC0~0xC7 值域一致）
const KNOB_OPTIONS: { code: number; label: string }[] = [
  { code: 0xc0, label: "音量 +" },
  { code: 0xc1, label: "音量 -" },
  { code: 0xc2, label: "静音" },
  { code: 0xc3, label: "播放/暂停" },
  { code: 0xc4, label: "下一曲" },
  { code: 0xc5, label: "上一曲" },
  { code: 0xc6, label: "亮度 +" },
  { code: 0xc7, label: "亮度 -" },
];

// 旋钮短按可选功能（v1.9：0xFE=切 Profile 默认 / Consumer 媒体 / 宏 M1~M4，与固件 knob_press 校验一致）
const KNOB_FUNC_PROFILE = 0xfe;
const KNOB_PRESS_OPTIONS: { code: number; label: string }[] = [
  { code: KNOB_FUNC_PROFILE, label: "切换 Profile（默认）" },
  ...KNOB_OPTIONS,
  { code: 0xf0, label: "触发宏 M1" },
  { code: 0xf1, label: "触发宏 M2" },
  { code: 0xf2, label: "触发宏 M3" },
  { code: 0xf3, label: "触发宏 M4" },
];

interface KnobPanelProps {
  knobCfg: { cw: number; ccw: number; press: number } | null;
  setKnobCfg: Dispatch<SetStateAction<{ cw: number; ccw: number; press: number } | null>>;
  onWriteKnob: (cw: number, ccw: number, press: number) => void;
  onFetchKnob: () => void;
  cfgConnected: boolean;
}

// 旋钮配置（P6：顺/逆时针输出可改为任意媒体功能；RAM 生效，0x05 持久化）
export function KnobPanel({
  knobCfg,
  setKnobCfg,
  onWriteKnob,
  onFetchKnob,
  cfgConnected,
}: KnobPanelProps) {
  return (
    <div className="mb-4 rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm">
      <div className="mb-3 flex items-center gap-2">
        <h2 className="text-sm font-semibold text-slate-500">旋钮配置</h2>
        <button
          onClick={onFetchKnob}
          disabled={!cfgConnected}
          title="帧协议读取当前旋钮输出配置"
          className="ml-auto rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1 text-xs font-medium text-slate-600 hover:bg-slate-100 disabled:opacity-40"
        >
          读取
        </button>
      </div>
      <div className="grid grid-cols-3 gap-2">
        <label className="text-xs text-slate-500">
          顺时针输出
          <select
            value={knobCfg?.cw ?? 0xc0}
            onChange={(e) => {
              const cw = Number(e.target.value);
              setKnobCfg((prev) => ({ cw, ccw: prev?.ccw ?? 0xc1, press: prev?.press ?? 0xfe }));
              onWriteKnob(cw, knobCfg?.ccw ?? 0xc1, knobCfg?.press ?? 0xfe);
            }}
            disabled={!cfgConnected}
            className="mt-1 w-full rounded-md border border-slate-300 px-2 py-1.5 text-sm disabled:opacity-40"
          >
            {KNOB_OPTIONS.map((o) => (
              <option key={o.code} value={o.code}>{o.label}</option>
            ))}
          </select>
        </label>
        <label className="text-xs text-slate-500">
          逆时针输出
          <select
            value={knobCfg?.ccw ?? 0xc1}
            onChange={(e) => {
              const ccw = Number(e.target.value);
              setKnobCfg((prev) => ({ cw: prev?.cw ?? 0xc0, ccw, press: prev?.press ?? 0xfe }));
              onWriteKnob(knobCfg?.cw ?? 0xc0, ccw, knobCfg?.press ?? 0xfe);
            }}
            disabled={!cfgConnected}
            className="mt-1 w-full rounded-md border border-slate-300 px-2 py-1.5 text-sm disabled:opacity-40"
          >
            {KNOB_OPTIONS.map((o) => (
              <option key={o.code} value={o.code}>{o.label}</option>
            ))}
          </select>
        </label>
        <label className="text-xs text-slate-500">
          按下输出
          <select
            value={knobCfg?.press ?? 0xfe}
            onChange={(e) => {
              const press = Number(e.target.value);
              setKnobCfg((prev) => ({ cw: prev?.cw ?? 0xc0, ccw: prev?.ccw ?? 0xc1, press }));
              onWriteKnob(knobCfg?.cw ?? 0xc0, knobCfg?.ccw ?? 0xc1, press);
            }}
            disabled={!cfgConnected}
            className="mt-1 w-full rounded-md border border-slate-300 px-2 py-1.5 text-sm disabled:opacity-40"
          >
            {KNOB_PRESS_OPTIONS.map((o) => (
              <option key={o.code} value={o.code}>{o.label}</option>
            ))}
          </select>
        </label>
      </div>
      <p className="mt-2 text-xs text-slate-400">
        默认旋转=音量加减、按下=切换 Profile；改为其它功能后 WiFi 模式经 [EVT] CC / MACRO 行由上位机注入。点「保存配置到键盘」持久化
      </p>
    </div>
  );
}
