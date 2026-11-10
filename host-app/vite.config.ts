import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

// Tauri 前端构建配置
export default defineConfig({
  plugins: [react(), tailwindcss()],
  // Vite 开发服务器选项（Tauri 模式下由 env 控制）
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
    watch: {
      // 排除 Rust 构建产物目录（编译锁文件会导致 watcher EBUSY 崩溃）
      ignored: ["**/src-tauri/target/**"],
    },
  },
  // 环境变量以 TAURI_ 开头的注入到 process.env
  envPrefix: ["VITE_", "TAURI_"],
  build: {
    // Tauri 在 debug 构建下用系统默认 minLevel
    minify: !process.env.TAURI_DEBUG ? "esbuild" : false,
    sourcemap: !!process.env.TAURI_DEBUG,
  },
});
