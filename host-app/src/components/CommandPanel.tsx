// 快捷命令按钮组
const COMMAND_GROUPS: { title: string; cmds: { label: string; cmd: string }[] }[] = [
  {
    title: "模式",
    cmds: [
      { label: "USB", cmd: "mode usb" },
      { label: "BLE", cmd: "mode ble" },
      { label: "下一个", cmd: "mode next" },
    ],
  },
  {
    title: "Profile",
    cmds: [
      { label: "1", cmd: "profile set 1" },
      { label: "2", cmd: "profile set 2" },
      { label: "3", cmd: "profile set 3" },
    ],
  },
  {
    title: "状态查询",
    cmds: [
      { label: "status", cmd: "status" },
      { label: "version", cmd: "version" },
      { label: "power", cmd: "power" },
      { label: "enc", cmd: "enc" },
      { label: "keymap", cmd: "keymap" },
    ],
  },
  {
    title: "LED",
    cmds: [
      { label: "USB 开", cmd: "led usb on" },
      { label: "USB 关", cmd: "led usb off" },
      { label: "BLE 开", cmd: "led ble on" },
      { label: "BLE 关", cmd: "led ble off" },
      { label: "闪 3 次", cmd: "led blink 3" },
    ],
  },
];

interface CommandPanelProps {
  connected: boolean;
  onSendCmd: (cmd: string) => void;
  customCmdRef: React.RefObject<HTMLInputElement | null>;
  onCustomCmd: (e: React.KeyboardEvent<HTMLInputElement>) => void;
}

// 命令控制台内容（设置页卡片内平铺：分组行 = 标题 + 按钮组，填满卡片宽度）
export function CommandPanel({ connected, onSendCmd, customCmdRef, onCustomCmd }: CommandPanelProps) {
  return (
    <div className="flex flex-col gap-2.5">
      {COMMAND_GROUPS.map((group) => (
        <div key={group.title} className="flex items-center gap-2">
          <span className="w-16 shrink-0 text-right text-xs font-semibold tracking-wide text-slate-400">
            {group.title}
          </span>
          <div className="flex flex-wrap gap-1.5">
            {group.cmds.map((c) => (
              <button
                key={c.cmd}
                onClick={() => onSendCmd(c.cmd)}
                disabled={!connected}
                className="rounded-md border border-slate-200 bg-slate-50 px-2.5 py-1 text-xs text-slate-600 hover:border-blue-300 hover:bg-blue-50 active:bg-blue-100 disabled:opacity-40"
              >
                {c.label}
              </button>
            ))}
          </div>
        </div>
      ))}
      <div className="flex items-center gap-2">
        <span className="w-16 shrink-0 text-right text-xs font-semibold tracking-wide text-slate-400">
          自定义
        </span>
        <input
          ref={customCmdRef}
          onKeyDown={onCustomCmd}
          placeholder="输入命令回车发送（串口命令，见 docs/串口命令协议.md）"
          className="min-w-0 flex-1 rounded-md border border-slate-300 px-2 py-1 font-mono text-xs focus:border-blue-400 focus:outline-none"
        />
      </div>
    </div>
  );
}
