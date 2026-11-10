import type { Dispatch, SetStateAction } from "react";
import { fmtBleAddr } from "../protocol";
import type { BatteryStatus, BleSlotsData } from "../types";

interface MaintenancePanelProps {
  cfgConnected: boolean;
  connected: boolean;
  battery: BatteryStatus | null;
  calDraft: { loMeas: number; loTrue: number; hiMeas: number; hiTrue: number };
  setCalDraft: Dispatch<
    SetStateAction<{ loMeas: number; loTrue: number; hiMeas: number; hiTrue: number }>
  >;
  onSaveCal: () => void;
  onFetchMaint: () => void;
  sleepDraft: { lightMin: number; deepHr: number };
  setSleepDraft: Dispatch<SetStateAction<{ lightMin: number; deepHr: number }>>;
  onSaveSleep: () => void;
  bleSlots: BleSlotsData | null;
  onClearBleSlot: (slot: number) => void;
  onFetchBleSlots: () => void;
  flashBusy: boolean;
  onFlash: () => void;
  onReadCoredump: () => void;
}

// 设备维护（P7）：电池两点校准 / 休眠参数 / 蓝牙配对槽位 / 固件刷写与崩溃日志
export function MaintenancePanel({
  cfgConnected,
  connected,
  battery,
  calDraft,
  setCalDraft,
  onSaveCal,
  onFetchMaint,
  sleepDraft,
  setSleepDraft,
  onSaveSleep,
  bleSlots,
  onClearBleSlot,
  onFetchBleSlots,
  flashBusy,
  onFlash,
  onReadCoredump,
}: MaintenancePanelProps) {
  return (
    <div className="grid grid-cols-1 items-start gap-4 lg:grid-cols-2">
      {/* 设备维护（P7）：电池两点校准 */}
      <div className="rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm">
        <div className="mb-3 flex items-center gap-2">
          <span className="grid h-6 w-6 place-items-center rounded-md bg-indigo-100 text-xs font-bold text-indigo-600">
            校
          </span>
          <h2 className="text-sm font-semibold text-slate-700">电池两点校准</h2>
          <button
            onClick={onFetchMaint}
            disabled={!cfgConnected}
            title="帧协议 CAL_GET 0x0F 读取当前校准点"
            className="ml-auto rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1 text-xs font-medium text-slate-600 hover:bg-slate-100 disabled:opacity-40"
          >
            读取
          </button>
        </div>
        {battery && (
          <p className="mb-2 text-xs text-slate-500">
            当前实测电压：<b>{battery.mv} mV</b>
            {battery.usb && <span className="text-amber-600">（USB 在位，校准请拔线用电池供电）</span>}
          </p>
        )}
        <div className="grid grid-cols-2 gap-2">
          {(
            [
              ["loMeas", "低点实测电压 (mV)"],
              ["loTrue", "低点真实电压 (mV)"],
              ["hiMeas", "高点实测电压 (mV)"],
              ["hiTrue", "高点真实电压 (mV)"],
            ] as const
          ).map(([key, label]) => (
            <label key={key} className="text-xs text-slate-500">
              {label}
              <input
                type="number"
                min={0}
                max={4500}
                value={calDraft[key]}
                onChange={(e) =>
                  setCalDraft((d) => ({ ...d, [key]: Number(e.target.value) || 0 }))
                }
                className="mt-0.5 w-full rounded-md border border-slate-300 px-2 py-1 font-mono text-sm focus:border-blue-400 focus:outline-none"
              />
            </label>
          ))}
        </div>
        <button
          onClick={onSaveCal}
          disabled={!cfgConnected}
          title="CAL_SET 0x10：写入 NVS，读数按两点线性映射修正"
          className="mt-2 w-full rounded-md border border-indigo-200 bg-indigo-50 px-2.5 py-1.5 text-xs font-medium text-indigo-600 hover:bg-indigo-100 disabled:opacity-40"
        >
          写入校准点
        </button>
        <p className="mt-2 text-xs text-slate-400">
          用万用表测电池真实电压填「真实电压」，键盘上报值为「实测电压」；两点做全程线性修正（区间内插值、区间外外推）。
          真实电压填 0 表示该点未校准。建议低点取 ~3.5V、高点取 ~4.1V。
        </p>
      </div>

      {/* 设备维护（P7）：休眠参数 */}
      <div className="rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm">
        <div className="mb-3 flex items-center gap-2">
          <span className="grid h-6 w-6 place-items-center rounded-md bg-indigo-100 text-xs font-bold text-indigo-600">
            眠
          </span>
          <h2 className="text-sm font-semibold text-slate-700">休眠参数</h2>
          <button
            onClick={onFetchMaint}
            disabled={!cfgConnected}
            title="帧协议 SLEEP_GET 0x11 读取当前参数"
            className="ml-auto rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1 text-xs font-medium text-slate-600 hover:bg-slate-100 disabled:opacity-40"
          >
            读取
          </button>
        </div>
        <div className="grid grid-cols-2 gap-2">
          <label className="text-xs text-slate-500">
            light sleep 闲置 (分钟)
            <input
              type="number"
              min={1}
              max={120}
              value={sleepDraft.lightMin}
              onChange={(e) =>
                setSleepDraft((d) => ({ ...d, lightMin: Number(e.target.value) || 0 }))
              }
              className="mt-0.5 w-full rounded-md border border-slate-300 px-2 py-1 font-mono text-sm focus:border-blue-400 focus:outline-none"
            />
          </label>
          <label className="text-xs text-slate-500">
            deep sleep 闲置 (小时)
            <input
              type="number"
              min={1}
              max={168}
              value={sleepDraft.deepHr}
              onChange={(e) =>
                setSleepDraft((d) => ({ ...d, deepHr: Number(e.target.value) || 0 }))
              }
              className="mt-0.5 w-full rounded-md border border-slate-300 px-2 py-1 font-mono text-sm focus:border-blue-400 focus:outline-none"
            />
          </label>
        </div>
        <button
          onClick={onSaveSleep}
          disabled={!cfgConnected}
          title="SLEEP_SET 0x12：立即生效并写 NVS"
          className="mt-2 w-full rounded-md border border-indigo-200 bg-indigo-50 px-2.5 py-1.5 text-xs font-medium text-indigo-600 hover:bg-indigo-100 disabled:opacity-40"
        >
          写入休眠参数
        </button>
        <p className="mt-2 text-xs text-slate-400">
          电池供电闲置进入 light sleep（任意键唤醒）；持续闲置到深睡阈值则深睡关机（RTC 任意键唤醒，等效重启）。
          USB 在位永不睡眠；WiFi 热点模式不进 light sleep；BLE 连接态不睡眠。
        </p>
      </div>

      {/* 设备维护（v2.5）：蓝牙配对槽位（宽卡：三槽位横排） */}
      <div className="rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm lg:col-span-2">
        <div className="mb-3 flex items-center gap-2">
          <span className="grid h-6 w-6 place-items-center rounded-md bg-sky-100 text-xs font-bold text-sky-600">
            牙
          </span>
          <h2 className="text-sm font-semibold text-slate-700">蓝牙配对槽位</h2>
          <button
            onClick={onFetchBleSlots}
            disabled={!cfgConnected}
            title="帧协议 BLE_SLOT_GET 0x13 读取槽位列表"
            className="ml-auto rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1 text-xs font-medium text-slate-600 hover:bg-slate-100 disabled:opacity-40"
          >
            读取
          </button>
        </div>
        <div className="grid grid-cols-1 gap-2 sm:grid-cols-3">
          {[0, 1, 2].map((i) => {
            const s = bleSlots?.slots[i];
            const paired = !!s?.paired;
            const isCur = bleSlots?.currentSlot === i && paired;
            return (
              <div
                key={i}
                className={`flex flex-col rounded-lg border p-2.5 ${
                  isCur ? "border-blue-300 bg-blue-50/50" : "border-slate-200"
                }`}
              >
                <div className="flex flex-wrap items-center gap-1.5">
                  <span className="text-xs font-semibold text-slate-600">槽位 {i + 1}</span>
                  {isCur && (
                    <span className="rounded bg-blue-100 px-1.5 py-0.5 text-[10px] font-medium text-blue-600">
                      当前
                    </span>
                  )}
                  <span
                    className={`rounded px-1.5 py-0.5 text-[10px] font-medium ${
                      paired ? "bg-emerald-100 text-emerald-600" : "bg-slate-100 text-slate-400"
                    }`}
                  >
                    {paired ? "已配对" : "空槽位"}
                  </span>
                </div>
                {paired && s ? (
                  <div className="mt-1.5 flex min-h-9 flex-col justify-center gap-0.5">
                    {/* 固件在配对加密成功后读取对端 GATT 设备名回填；旧记录/读取失败显示「未知设备」 */}
                    <span className="truncate text-xs text-slate-600">{s.name || "未知设备"}</span>
                    <span className="truncate font-mono text-xs text-slate-400">
                      {fmtBleAddr(s.peerAddr)}
                    </span>
                  </div>
                ) : (
                  <div className="mt-1.5 min-h-9" />
                )}
                <button
                  onClick={() => onClearBleSlot(i)}
                  disabled={!paired || !cfgConnected}
                  title={paired ? "删除该槽位的配对绑定（BLE_SLOT_CLEAR 0x14）" : "空槽位无需清除"}
                  className="mt-2 rounded-md border border-red-200 bg-red-50 px-2 py-0.5 text-xs font-medium text-red-500 hover:bg-red-100 disabled:opacity-30"
                >
                  删除配对
                </button>
              </div>
            );
          })}
        </div>
        <button
          onClick={() => onClearBleSlot(0xff)}
          disabled={!cfgConnected || !bleSlots?.slots.some((x) => x.paired)}
          title="清除全部 3 个槽位的配对绑定（BLE_SLOT_CLEAR 0x14 slot=0xFF）"
          className="mt-2 w-full rounded-md border border-red-200 bg-red-50 px-2.5 py-1.5 text-xs font-medium text-red-500 hover:bg-red-100 disabled:opacity-40"
        >
          全部清除
        </button>
        <p className="mt-2 text-xs text-slate-400">
          键盘有 3 个独立配对槽位；删除配对会同步删除键盘侧密钥，对应设备需在系统蓝牙设置中重新配对。
          全部清除等效于串口命令 ble clear。连接建立时自动读取一次，删除成功后自动刷新。
        </p>
      </div>

      {/* 设备维护（P7）：固件刷写 / 崩溃日志（esptool 子进程，宽卡横排按钮） */}
      <div className="rounded-xl border border-slate-200 bg-white/70 p-4 backdrop-blur-sm lg:col-span-2">
        <div className="mb-3 flex items-center gap-2">
          <span className="grid h-6 w-6 place-items-center rounded-md bg-amber-100 text-xs font-bold text-amber-600">
            刷
          </span>
          <h2 className="text-sm font-semibold text-slate-700">固件刷写 / 崩溃日志</h2>
        </div>
        <div className="flex gap-2">
          <button
            onClick={onFlash}
            disabled={flashBusy || !connected}
            title={
              connected
                ? "选择 .bin（仅刷应用）或 build 目录（完整刷写，含分区表）"
                : "需先连接串口（刷写经当前串口进行）"
            }
            className="flex-1 rounded-md border border-blue-200 bg-blue-50 px-2.5 py-1.5 text-xs font-medium text-blue-600 hover:bg-blue-100 disabled:opacity-40"
          >
            {flashBusy ? "esptool 执行中…" : "选择固件并刷写"}
          </button>
          <button
            onClick={onReadCoredump}
            disabled={flashBusy || !connected}
            title="读出 coredump 分区（0x1F0000，64KB）保存为 .bin"
            className="flex-1 rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1.5 text-xs font-medium text-slate-600 hover:bg-slate-100 disabled:opacity-40"
          >
            读取崩溃日志
          </button>
        </div>
        <p className="mt-2 text-xs text-slate-400">
          刷写/读取会自动断开串口并在完成后要求重连；进度见底部日志台 [ESPTOOL] 行。
          分区表或 bootloader 变更后请选 build 目录完整刷写；崩溃日志为 ELF 格式 coredump，
          可用 esp-coredump 工具解析崩溃现场。
        </p>
      </div>
    </div>
  );
}
