import { useState } from "react";

interface LogConsoleProps {
  logLines: string[];
  onClear: () => void;
  logEndRef: React.RefObject<HTMLDivElement | null>;
}

// 底部：日志台（可整体收起为细条，主工作区随之变大）
export function LogConsole({ logLines, onClear, logEndRef }: LogConsoleProps) {
  // 底部日志台整体收起（收起后留细条，点击展开；默认收起）
  const [logOpen, setLogOpen] = useState(false);
  const [evtOnly, setEvtOnly] = useState(false);
  const visibleLines = evtOnly ? logLines.filter((l) => l.includes("[EVT]")) : logLines;
  return (
    <>
      {logOpen ? (
        <footer className="relative h-56 shrink-0 border-t border-slate-200 bg-slate-900">
          <div className="flex items-center gap-3 border-b border-slate-700 px-3 py-1.5">
            <span className="text-xs font-semibold text-slate-300">串口日志</span>
            <label className="flex cursor-pointer items-center gap-1.5 text-xs text-slate-400">
              <input
                type="checkbox"
                checked={evtOnly}
                onChange={(e) => setEvtOnly(e.target.checked)}
                className="accent-blue-500"
              />
              只看事件
            </label>
            <button
              onClick={onClear}
              className="rounded px-2 py-0.5 text-xs text-slate-400 hover:bg-slate-700"
            >
              清空
            </button>
            <button
              onClick={() => setLogOpen(false)}
              title="收起日志台"
              className="rounded px-2 py-0.5 text-xs text-slate-400 hover:bg-slate-700 hover:text-slate-200"
            >
              ▼ 收起
            </button>
          </div>
          <div className="log-area h-[calc(100%-2.25rem)] overflow-y-auto px-3 py-1.5 font-mono text-xs leading-5">
            {visibleLines.map((line, i) => (
              <div
                key={i}
                className={
                  line.startsWith("[EVT]")
                    ? "text-emerald-400"
                    : line.startsWith("[BLE]")
                      ? "text-cyan-400"
                      : line.startsWith("[ERR]") || line.startsWith("E (")
                        ? "text-red-400"
                        : line.startsWith("[OK]") || line.startsWith("[INFO]")
                          ? "text-slate-300"
                          : line.startsWith("W (")
                            ? "text-amber-400"
                            : "text-slate-500"
                }
              >
                {line}
              </div>
            ))}
            <div ref={logEndRef} />
          </div>
        </footer>
      ) : (
        <footer className="relative shrink-0 border-t border-slate-200 bg-slate-900">
          <button
            onClick={() => setLogOpen(true)}
            title="展开日志台"
            className="flex w-full items-center gap-2 px-3 py-1 text-left text-xs text-slate-400 hover:text-slate-200"
          >
            ▲ 串口日志（{visibleLines.length} 条，点击展开）
          </button>
        </footer>
      )}
    </>
  );
}
