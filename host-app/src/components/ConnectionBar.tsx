import type { PortInfo, BatteryStatus } from "../types";
import { BatteryIndicator } from "./BatteryPanel";

export type PathTab = "usb" | "ble" | "wifi";

const PATH_TABS: { key: PathTab; label: string }[] = [
  { key: "usb", label: "USB" },
  { key: "ble", label: "蓝牙" },
  { key: "wifi", label: "WiFi" },
];

interface ConnectionBarProps {
  pathTab: PathTab;
  setPathTab: (t: PathTab) => void;
  connected: boolean;
  connecting: boolean;
  ports: PortInfo[];
  selectedPort: string;
  setSelectedPort: React.Dispatch<React.SetStateAction<string>>;
  onRefreshPorts: () => void;
  onToggleConnect: () => void;
  bleStatus: "idle" | "scanning" | "connected";
  bleConnecting: boolean;
  onToggleBle: () => void;
  wifiConnected: boolean;
  wifiConnecting: boolean;
  onToggleWifi: () => void;
  battery: BatteryStatus | null;
  dirty: boolean;
  cfgConnected: boolean;
  onReadKeymap: () => void;
  onSaveKeymap: () => void;
}

// 顶栏：应用名 + 三通路分段选择器 + 通路连接控件 + 电量 + 键位读取/保存 + 连接状态
export function ConnectionBar({
  pathTab,
  setPathTab,
  connected,
  connecting,
  ports,
  selectedPort,
  setSelectedPort,
  onRefreshPorts,
  onToggleConnect,
  bleStatus,
  bleConnecting,
  onToggleBle,
  wifiConnected,
  wifiConnecting,
  onToggleWifi,
  battery,
  dirty,
  cfgConnected,
  onReadKeymap,
  onSaveKeymap,
}: ConnectionBarProps) {
  const bleOnline = bleStatus === "connected";
  const onlinePath = connected ? "USB" : bleOnline ? "蓝牙" : wifiConnected ? "WiFi" : null;
  return (
    <header className="flex h-16 shrink-0 items-center gap-3 border-b border-slate-200 bg-white/70 px-4 shadow-sm backdrop-blur-md">
      <h1 className="shrink-0 text-sm font-bold text-slate-700">SimpleKey-Regression</h1>

      {/* 三通路分段选择器（选中白底阴影，tab 上带通路状态点） */}
      <div className="flex shrink-0 rounded-lg bg-slate-100 p-0.5">
        {PATH_TABS.map((t) => {
          const online = t.key === "usb" ? connected : t.key === "ble" ? bleOnline : wifiConnected;
          return (
            <button
              key={t.key}
              onClick={() => setPathTab(t.key)}
              className={`flex items-center gap-1.5 rounded-md px-3 py-1 text-sm font-medium transition-colors ${
                pathTab === t.key
                  ? "bg-white text-slate-800 shadow-sm"
                  : "text-slate-500 hover:text-slate-700"
              }`}
            >
              <span
                className={`inline-block h-1.5 w-1.5 rounded-full ${
                  online ? "bg-emerald-500" : "bg-slate-300"
                }`}
              />
              {t.label}
            </button>
          );
        })}
      </div>

      {/* 当前选中通路的连接控件 */}
      {pathTab === "usb" && (
        <>
          <select
            value={selectedPort}
            onChange={(e) => setSelectedPort(e.target.value)}
            className="w-44 min-w-0 rounded-md border border-slate-300 bg-white px-2 py-1 text-sm"
          >
            {ports.length === 0 && <option value="">无可用串口</option>}
            {ports.map((p) => (
              <option key={p.name} value={p.name}>
                {p.name} {p.desc && `- ${p.desc}`}
              </option>
            ))}
          </select>
          <button
            onClick={onRefreshPorts}
            className="shrink-0 whitespace-nowrap rounded-md border border-slate-300 bg-white px-2 py-1 text-sm hover:bg-slate-50 active:bg-slate-100"
            title="刷新串口列表"
          >
            刷新
          </button>
          <button
            onClick={onToggleConnect}
            disabled={!selectedPort || connecting}
            className={`shrink-0 whitespace-nowrap rounded-md px-3 py-1.5 text-sm font-medium text-white disabled:opacity-40 ${
              connected ? "bg-red-500 hover:bg-red-600" : "bg-emerald-600 hover:bg-emerald-700"
            }`}
          >
            {connected ? "断开" : connecting ? "连接中..." : "连接"}
          </button>
        </>
      )}
      {pathTab === "ble" && (
        <button
          onClick={onToggleBle}
          disabled={bleConnecting}
          className={`shrink-0 whitespace-nowrap rounded-md px-3 py-1.5 text-sm font-medium text-white disabled:opacity-40 ${
            bleOnline ? "bg-red-500 hover:bg-red-600" : "bg-blue-600 hover:bg-blue-700"
          }`}
        >
          {bleOnline ? "断开监控" : bleConnecting || bleStatus === "scanning" ? "扫描中..." : "连接监控"}
        </button>
      )}
      {pathTab === "wifi" && (
        <button
          onClick={onToggleWifi}
          disabled={wifiConnecting}
          className={`shrink-0 whitespace-nowrap rounded-md px-3 py-1.5 text-sm font-medium text-white disabled:opacity-40 ${
            wifiConnected ? "bg-red-500 hover:bg-red-600" : "bg-emerald-600 hover:bg-emerald-700"
          }`}
          title={
            wifiConnected
              ? "断开 TCP 通路（保持 WLAN 连接）"
              : "连接键盘热点 SimpleKey_绝希（TCP 192.168.4.1:8266）"
          }
        >
          {wifiConnected ? "断开 WiFi" : wifiConnecting ? "连接中..." : "连接 WiFi"}
        </button>
      )}

      <div className="ml-auto flex shrink-0 items-center gap-3">
        {/* 电量：窄窗口隐藏（信息在键位页也有） */}
        <div className="hidden lg:block">
          <BatteryIndicator battery={battery} />
        </div>

        {/* 键位读取 / 保存更改（dirty 时橙点提示未保存修改） */}
        <button
          onClick={onReadKeymap}
          disabled={!cfgConnected}
          title="帧协议读取设备当前键位绑定并回显到网格（串口/蓝牙/WiFi 通路自动选择）"
          className="shrink-0 whitespace-nowrap rounded-md border border-slate-300 bg-white px-2.5 py-1.5 text-sm font-medium text-slate-600 hover:bg-slate-50 disabled:opacity-40"
        >
          读取
        </button>
        <button
          onClick={onSaveKeymap}
          disabled={!cfgConnected}
          title={
            dirty
              ? "有未保存的键位修改（保存写入键盘 NVS）"
              : "把当前键位/旋钮/宏写入键盘当前 Profile（NVS 持久化）"
          }
          className={`flex shrink-0 items-center gap-1.5 whitespace-nowrap rounded-md bg-blue-600 px-3 py-1.5 text-sm font-medium text-white hover:bg-blue-700 disabled:opacity-40 ${
            dirty ? "ring-2 ring-orange-400" : ""
          }`}
        >
          {dirty && <span className="h-2 w-2 rounded-full bg-orange-400" />}
          保存更改
        </button>

        {/* 连接状态综合提示（窄窗口隐藏避免挤压） */}
        <div className="hidden items-center gap-1.5 text-sm xl:flex">
          <span
            className={`inline-block h-2 w-2 rounded-full ${
              onlinePath ? "bg-emerald-500" : "bg-slate-300"
            }`}
          />
          <span className={onlinePath ? "text-emerald-600" : "text-slate-400"}>
            {onlinePath ? `已连接（${onlinePath}）` : "未连接"}
          </span>
        </div>
      </div>
    </header>
  );
}
