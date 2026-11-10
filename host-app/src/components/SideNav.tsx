export type Workspace = "layout" | "profile" | "knob" | "macro" | "settings";

const NAV_ITEMS: { key: Workspace; icon: string; label: string }[] = [
  { key: "layout", icon: "键", label: "键位布局" },
  { key: "profile", icon: "档", label: "Profile" },
  { key: "knob", icon: "旋", label: "旋钮" },
  { key: "macro", icon: "宏", label: "宏" },
  { key: "settings", icon: "置", label: "设置" },
];

interface SideNavProps {
  workspace: Workspace;
  setWorkspace: (w: Workspace) => void;
}

// 左侧一级分类导航（VIA 式竖栏）：文字图标 + 9px 小字标签
export function SideNav({ workspace, setWorkspace }: SideNavProps) {
  return (
    <nav className="flex w-16 shrink-0 flex-col items-center gap-1 border-r border-slate-200 bg-white/70 py-3 backdrop-blur-sm">
      {NAV_ITEMS.map((item) => (
        <button
          key={item.key}
          onClick={() => setWorkspace(item.key)}
          title={item.label}
          className={`flex w-14 flex-col items-center gap-0.5 rounded-lg py-2 transition-colors ${
            workspace === item.key
              ? "bg-blue-600 text-white"
              : "text-slate-500 hover:bg-slate-100 hover:text-blue-600"
          }`}
        >
          <span className="text-base font-semibold leading-none">{item.icon}</span>
          <span className="text-[9px] leading-tight">{item.label}</span>
        </button>
      ))}
    </nav>
  );
}
