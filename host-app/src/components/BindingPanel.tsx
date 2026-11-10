import { useState } from "react";
import { KEY_GROUPS, CODE_TO_LABEL, SUB_LABEL } from "../keycodes";
import type { KeyDef } from "../keycodes";

interface BindingPanelProps {
  row: number;
  col: number;
  code: number; // 当前绑定键码
  onPick: (k: KeyDef) => void; // 选择键码（下发后保持面板打开，方便连续改键）
  onClear: () => void; // 清除绑定（下发 0x00）
  onClose: () => void;
}

// 键位绑定面板（VIA 式同屏右栏：点击键帽后出现，替代原 Bottom Sheet 键码选择器）
export function BindingPanel({ row, col, code, onPick, onClear, onClose }: BindingPanelProps) {
  // 键码目录筛选：搜索词非空时跨全部分组过滤（不区分大小写），否则按分组页签展示
  const [query, setQuery] = useState("");
  const [groupIdx, setGroupIdx] = useState(0);
  const q = query.trim().toLowerCase();
  const label =
    CODE_TO_LABEL.get(code) ?? `0x${code.toString(16).padStart(2, "0").toUpperCase()}`;
  const sub = SUB_LABEL.get(code);
  const match = (k: KeyDef) =>
    !q || k.label.toLowerCase().includes(q) || k.name.toLowerCase().includes(q);
  const hits = q ? KEY_GROUPS.flatMap((g) => g.keys).filter(match) : [];

  // 键码按钮（当前绑定键码高亮蓝底，与原键码选择器一致）
  const keyButton = (k: KeyDef) => (
    <button
      key={k.code}
      onClick={() => onPick(k)}
      title={k.name}
      className={`rounded-md border px-1 py-1.5 font-mono text-xs transition-colors ${
        code === k.code
          ? "border-blue-500 bg-blue-500 text-white"
          : "border-slate-200 bg-slate-50 text-slate-600 hover:border-blue-300 hover:bg-blue-50"
      }`}
    >
      {k.label}
    </button>
  );

  return (
    <aside className="w-full shrink-0 self-start rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm lg:sticky lg:top-0 lg:w-80">
      {/* 头部：图标章 + 标题 + 关闭 */}
      <div className="mb-3 flex items-center gap-2">
        <span className="grid h-6 w-6 place-items-center rounded-md bg-blue-500 text-xs font-bold text-white">
          键
        </span>
        <h2 className="text-sm font-semibold text-slate-700">键位绑定</h2>
        <button
          onClick={onClose}
          title="关闭绑定面板"
          className="ml-auto rounded px-2 py-0.5 text-xs text-slate-400 hover:bg-slate-100"
        >
          ×
        </button>
      </div>

      {/* 选中键信息：位置 + 当前绑定 */}
      <div className="mb-3 rounded-lg border border-slate-200 bg-white/80 p-3">
        <div className="text-xs text-slate-400">
          位置 R{row}C{col}
        </div>
        <div className="mt-0.5 flex items-baseline gap-2">
          <span className="text-2xl font-semibold text-slate-800">
            {code === 0x00 ? "无" : label}
          </span>
          {sub && <span className="text-xs text-slate-400">{sub}</span>}
        </div>
      </div>

      {/* 操作行：清除绑定 + 提示文案 */}
      <div className="mb-3 flex items-center justify-between gap-2">
        <button
          onClick={onClear}
          className="rounded-md border border-slate-200 bg-white px-2.5 py-1 text-xs text-slate-600 transition-colors hover:border-red-300 hover:bg-red-50 hover:text-red-600"
        >
          清除绑定
        </button>
        <span className="text-xs text-slate-400">点击键码立即下发并生效</span>
      </div>

      {/* 键码目录：顶部搜索框实时过滤；无搜索词时分组页签 + 当前组键码网格 */}
      <input
        value={query}
        onChange={(e) => setQuery(e.target.value)}
        placeholder="搜索键码（如 esc / vol / 1）"
        className="mb-2 w-full rounded-md border border-slate-200 bg-white px-2 py-1.5 text-xs text-slate-700 placeholder:text-slate-300 focus:border-blue-400 focus:outline-none"
      />
      {q ? (
        hits.length > 0 ? (
          <div className="grid grid-cols-6 gap-1.5">{hits.map(keyButton)}</div>
        ) : (
          <p className="text-xs text-slate-400">无匹配键码</p>
        )
      ) : (
        <>
          <div className="mb-2 flex flex-wrap gap-1">
            {KEY_GROUPS.map((g, i) => (
              <button
                key={g.title}
                onClick={() => setGroupIdx(i)}
                className={`rounded-md px-2 py-1 text-xs transition-colors ${
                  i === groupIdx
                    ? "bg-blue-500 font-semibold text-white"
                    : "bg-slate-100 text-slate-500 hover:bg-slate-200"
                }`}
              >
                {g.title}
              </button>
            ))}
          </div>
          <h4 className="mb-1.5 text-xs font-semibold uppercase tracking-wide text-slate-400">
            {KEY_GROUPS[groupIdx].title}
          </h4>
          <div className="grid grid-cols-6 gap-1.5">{KEY_GROUPS[groupIdx].keys.map(keyButton)}</div>
        </>
      )}
    </aside>
  );
}
