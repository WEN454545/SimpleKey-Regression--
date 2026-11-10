import type { BatteryStatus } from "../types";

// 电量显示（P4：POWER_GET 5s 轮询；电池未焊时 mv/pct 为悬空值，以 flags 徽标为准）
export function BatteryIndicator({ battery }: { battery: BatteryStatus | null }) {
  return (
    <>
      {battery && (
        <div className="flex items-center gap-1.5 border-l border-slate-200 pl-6 text-sm">
          <span className="text-slate-500">电池</span>
          <b
            className={
              battery.charging || battery.full
                ? "text-emerald-600"
                : battery.pct >= 50
                  ? "text-emerald-600"
                  : battery.pct >= 20
                    ? "text-amber-600"
                    : "text-red-600"
            }
          >
            {battery.pct}%
          </b>
          <span className="text-xs text-slate-400">{battery.mv} mV</span>
          {battery.charging && (
            <span className="rounded bg-emerald-50 px-1.5 py-0.5 text-xs text-emerald-600">充电中</span>
          )}
          {battery.full && (
            <span className="rounded bg-emerald-50 px-1.5 py-0.5 text-xs text-emerald-600">已充满</span>
          )}
          {battery.usb && (
            <span className="rounded bg-slate-100 px-1.5 py-0.5 text-xs text-slate-500">USB</span>
          )}
        </div>
      )}
    </>
  );
}

// 电量曲线（P4：POWER_GET 5s 一点，最多 120 点 = 10 分钟窗口）
export function BatteryCurve({ battHistory }: { battHistory: BatteryStatus[] }) {
  return (
    <>
      {battHistory.length >= 2 && (
        <div className="mt-4 rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm">
          <h2 className="mb-2 text-sm font-semibold text-slate-500">
            电池电压曲线（3300~4200 mV，最近 {battHistory.length} 点 × 5s）
          </h2>
          <svg viewBox="0 0 600 120" className="h-28 w-full" preserveAspectRatio="none">
            {[0.25, 0.5, 0.75].map((f) => (
              <line key={f} x1={0} x2={600} y1={120 * f} y2={120 * f} stroke="#e2e8f0" strokeWidth={1} />
            ))}
            <polyline
              fill="none"
              stroke="#10b981"
              strokeWidth={2}
              points={battHistory
                .map((b, i) => {
                  const x = (i / (battHistory.length - 1)) * 600;
                  const v = Math.min(4200, Math.max(3300, b.mv));
                  const y = 120 - ((v - 3300) / 900) * 120;
                  return `${x.toFixed(1)},${y.toFixed(1)}`;
                })
                .join(" ")}
            />
          </svg>
          <p className="mt-1 text-xs text-slate-400">
            电池未焊时电压为悬空值（曲线触顶属正常）；接上电池后可观察充放电趋势与低电告警阈值（3.55V）
          </p>
        </div>
      )}
    </>
  );
}
