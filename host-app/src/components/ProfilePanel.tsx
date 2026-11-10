import { CODE_TO_LABEL } from "../keycodes";
import type { ProfileInfo } from "../types";

interface ProfilePanelProps {
  profiles: ProfileInfo[];
  currentProfile: number;
  cfgConnected: boolean;
  onSwitchProfile: (i: number) => void;
  onRenameProfile: (i: number, name: string) => void;
  onFetchProfiles: () => void;
  onExportProfiles: () => void;
  onImportProfiles: () => void;
}

// Profile 管理（P4：点击切换 + 名称编辑重命名 + 导入导出 JSON）
export function ProfilePanel({
  profiles,
  currentProfile,
  cfgConnected,
  onSwitchProfile,
  onRenameProfile,
  onFetchProfiles,
  onExportProfiles,
  onImportProfiles,
}: ProfilePanelProps) {
  return (
    <div className="mb-4 rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm">
      <div className="mb-3 flex items-center gap-2">
        <h2 className="text-sm font-semibold text-slate-500">
          Profile 管理（点击卡片切换）
        </h2>
        <button
          onClick={onFetchProfiles}
          disabled={!cfgConnected}
          title="帧协议重新读取 3 个 Profile"
          className="rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1 text-xs font-medium text-slate-600 hover:bg-slate-100 disabled:opacity-40"
        >
          读取
        </button>
        <button
          onClick={onExportProfiles}
          title="导出 3 个 Profile 为 JSON 文件（需先读取）"
          className="rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1 text-xs font-medium text-slate-600 hover:bg-slate-100"
        >
          导出
        </button>
        <button
          onClick={onImportProfiles}
          disabled={!cfgConnected}
          title="从 JSON 导入并写入键盘（NVS 持久化）"
          className="ml-auto rounded-md border border-indigo-200 bg-indigo-50 px-2.5 py-1 text-xs font-medium text-indigo-600 hover:bg-indigo-100 disabled:opacity-40"
        >
          导入
        </button>
      </div>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-3 lg:grid-cols-1">
        {[0, 1, 2].map((i) => {
          const p = profiles[i];
          const isCur = currentProfile === i + 1;
          // 19 键缩略布局：前 4 行 4 列 + 末行 Ctrl/Shift/Space（2u 跨双列），与主网格一致
          const km = p?.keymap;
          const thumbCells: Array<{ r: number; c: number; span?: boolean }> = [];
          for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) thumbCells.push({ r, c });
          thumbCells.push({ r: 4, c: 0 }, { r: 4, c: 1 }, { r: 4, c: 2, span: true });
          return (
            <div
              key={i}
              onClick={() => onSwitchProfile(i)}
              title={`切换到 Profile ${i + 1}`}
              className={`cursor-pointer rounded-xl border-2 p-3 transition-all ${
                isCur
                  ? "border-blue-500 bg-blue-50 ring-2 ring-blue-500 ring-offset-1"
                  : "border-slate-200 bg-slate-50 hover:border-blue-300 hover:ring-1 hover:ring-blue-300"
              }`}
            >
              <div className="mb-2 flex items-center justify-between">
                <div className="flex items-center gap-1.5">
                  <span className="grid h-6 w-6 place-items-center rounded-md bg-blue-100 text-xs font-bold text-blue-600">
                    {i + 1}
                  </span>
                  <span className={`text-xs font-bold ${isCur ? "text-blue-600" : "text-slate-400"}`}>
                    Profile {i + 1}
                  </span>
                  {isCur && (
                    <span className="rounded-full bg-blue-500 px-1.5 py-0.5 text-[10px] font-medium text-white">
                      使用中
                    </span>
                  )}
                </div>
                {p && (
                  <span className="text-[10px] text-slate-400">
                    默认 {p.defaultMode === 1 ? "USB" : p.defaultMode === 2 ? "BLE" : "无"}
                  </span>
                )}
              </div>
              <input
                key={`${i}-${p?.name ?? "empty"}`}
                defaultValue={p?.name ?? ""}
                placeholder={p ? p.name : "未读取"}
                onClick={(e) => e.stopPropagation()}
                onBlur={(e) => onRenameProfile(i, e.target.value)}
                onKeyDown={(e) => e.key === "Enter" && (e.target as HTMLInputElement).blur()}
                className="w-full rounded-md border border-slate-200 bg-white px-2 py-1 text-sm focus:border-blue-400 focus:outline-none"
              />
              {/* 19 键位缩略（与主网格一致：末行 Space 2u 跨双列） */}
              <div className="mt-2 grid grid-cols-4 gap-1">
                {thumbCells.map(({ r, c, span }) => {
                  const code = km?.[r]?.[c] ?? 0;
                  return (
                    <span
                      key={`${r}-${c}`}
                      className={`rounded bg-white text-center font-mono text-[10px] leading-5 text-slate-500 ring-1 ring-slate-200 ${span ? "col-span-2" : ""}`}
                    >
                      {code
                        ? (CODE_TO_LABEL.get(code) ?? code.toString(16).toUpperCase())
                        : "-"}
                    </span>
                  );
                })}
              </div>
            </div>
          );
        })}
      </div>
      <p className="mt-2 text-xs text-slate-400">
        名称编辑后失焦/回车保存（写入键盘 NVS）；导出 JSON 可备份键位方案，导入自动写回键盘
      </p>
    </div>
  );
}
