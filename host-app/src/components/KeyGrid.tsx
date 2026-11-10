import { CODE_TO_LABEL, SUB_LABEL } from "../keycodes";

interface KeyGridProps {
  keymapDefs: number[][];
  keyStates: boolean[][];
  selectedKey: { row: number; col: number } | null;
  onKeyClick: (row: number, col: number) => void;
}

// 4×5 键位网格（点击按键选中并在右侧绑定面板改键；实时高亮显示按下状态）
export function KeyGrid({ keymapDefs, keyStates, selectedKey, onKeyClick }: KeyGridProps) {
  // 单个键帽（原版样式：灰色键帽底座 + 白色立体键面；按下时键面变蓝下沉，选中时蓝色描边）
  const keyCap = (r: number, c: number, sizeCls: string, is2u = false) => {
    const code = keymapDefs[r]?.[c] ?? 0;
    // 2u 空格跨两个矩阵位，任一轴位触发都点亮
    const down = is2u
      ? (keyStates[r]?.[c] || keyStates[r]?.[c + 1]) ?? false
      : (keyStates[r]?.[c] ?? false);
    const selected = selectedKey?.row === r && selectedKey?.col === c;
    const label =
      CODE_TO_LABEL.get(code) ?? `0x${code.toString(16).padStart(2, "0").toUpperCase()}`;
    const sub = SUB_LABEL.get(code);
    return (
      <button
        key={`${r}-${c}`}
        onClick={() => onKeyClick(r, c)}
        title={`ROW ${r} COL ${c}${is2u ? "（2u 空格，含两列轴位）" : ""}`}
        className={`${sizeCls} rounded-lg bg-slate-400/90 p-[3px] shadow-sm ring-2 ring-offset-1 transition-all ${
          selected ? "ring-blue-500" : "ring-transparent hover:ring-1 hover:ring-blue-300"
        }`}
      >
        <div
          className={`flex h-14 w-full flex-col items-center justify-center rounded-lg border transition-all duration-75 ${
            down
              ? "scale-[0.97] border-blue-600 bg-blue-500 text-white"
              : "border-slate-200 bg-gradient-to-b from-white to-slate-50 text-slate-600 shadow-[0_2px_0_0_#cbd5e1,0_2px_6px_rgba(15,23,42,0.08)] hover:-translate-y-px"
          }`}
        >
          <span className="font-medium leading-tight">{label}</span>
          {sub && (
            <span
              className={`text-[10px] leading-tight ${down ? "text-blue-100" : "text-slate-400"}`}
            >
              {sub}
            </span>
          )}
        </div>
      </button>
    );
  };

  return (
    <div className="rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm">
      <h2 className="mb-3 text-sm font-semibold text-slate-500">
        键位矩阵（点击按键编辑键位；按下时高亮）
      </h2>
      {/* 键位网格（原版样式：浅灰底盘 + 灰色键帽底座 + 白色立体键面；末行 Space 为 2u 跨双列） */}
      <div className="rounded-2xl bg-slate-200 p-3 shadow-inner">
        <div className="flex flex-col gap-2">
          {[0, 1, 2, 3, 4].map((r) => (
            <div key={r} className="flex gap-2">
              {r < 4
                ? [0, 1, 2, 3].map((c) => keyCap(r, c, "flex-1"))
                : [
                    keyCap(4, 0, "flex-1"),
                    keyCap(4, 1, "flex-1"),
                    keyCap(4, 2, "flex-[2]", true),
                  ]}
            </div>
          ))}
        </div>
      </div>
      <p className="mt-2 text-xs text-slate-400">
        选择键位后通过串口立即下发（RAM 生效）；点顶栏「保存更改」持久化键位/旋钮/宏到当前 Profile
      </p>
    </div>
  );
}
