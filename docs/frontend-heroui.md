# HeroUI Pro 前端替换

2026-10-01，按用户要求直接删除旧前端视觉层并重写，不创建旧前端备份。移除协作规范中的 `astra-orchestrator` 技能要求。视觉参考本机 `F:/Workspace/UltraTerminal/web/src`，采用其侧栏工作台、紧凑表单、主题和布局方式；业务仍归 FlexEdge。

## 当前实现

- 固定 `@heroui-pro/react` 为 `1.0.0-beta.10`，`@heroui/react` / `@heroui/styles` 为 `3.2.6`，与参考项目一致。React 19、Vite 8、Tailwind CSS 4、TanStack Router / Query、React Hook Form / Zod 保留。
- 新布局使用 Pro `AppLayout`、`Sidebar`、`Navbar`、`Command`；资源页使用 `DataGrid`，流量统计使用 Pro `KPI` 和 `BarChart`。旧 shadcn/Radix 基础组件、主题、页面封装和相关直接依赖已删除。
- 登录、概览、集群、节点、网站、DNS、证书、缓存策略、服务商和任务页面均采用新组件。移动端导航、表格横向滚动、加载/错误/空状态、操作确认和明暗主题使用同一套实现。
- 保持现有 API、Cookie 会话、revision / `If-Match`、配置序列化与权限契约。本次未修改控制面、节点或数据库行为。
- 控制面当前只为多数资源读取提供 `/stream`，因此沿用实际 SSE 契约。配置选项与历史记录取得首帧后关闭，实时列表与日志按当前视图持有；模态实时视图打开时暂停背后列表。没有定时 GET、定时失效或空闲计时器重建资源订阅。
- TLS 关闭时清除证书和强制 HTTPS/HSTS，不可用绑定仍可移除。缓存取消/重试入口遵守实际任务状态；文件缓存编辑范围保留，旧策略状态码与错误陈旧窗口值往返保存。

## 验证

| 验证 | 本次结果 |
| --- | --- |
| 锁文件安装 | `bun install --frozen-lockfile` 通过 |
| 静态检查 | `bun run lint`、`bun run typecheck`、`bun run format:check` 通过 |
| 测试 | `bun test` 与 `bun run test` 均为 35 文件、162 项通过，0 失败 |
| 生产构建 | `bun run build` 通过；单文件 `build/web/index.html`，延续现有静态嵌入方式 |
| 架构检查 | Windows Release 重建 `flexedge_architecture_test`，`ctest -C Release -R '^flexedge_architecture$'` 通过；只更新前端文件归属检查 |
| 浏览器 mock | 桌面 1440×960 / 窄屏 390×844、明暗主题、折叠侧栏、移动导航、搜索键盘路径、各领域列表、表单校验/创建、菜单、确认取消、空/错误态、日志与统计已操作检查；最终复查使用生产构建预览 |
| 网络 mock | 空闲及重复通知请求数保持不变；节点/网站日志只持有一条活跃流；历史日志首帧后为 0 条；离开资源视图进入登录页为 0 条；日志断线恢复使用游标且无并行旧连接 |

浏览器数据来自 `qa/mock-server.mjs`，只用于本地验收，不进入生产构建。mock smoke 同时覆盖 DTO 路由、内存 CRUD、缺失/旧 revision、关联删除约束和 SSE 事件；它不代表真实控制面、数据库或节点联调。本次未做真实后端联调、Linux 构建、远程 CI 或生产部署。

构建日志、源码输入摘要、产物 SHA256、网络记录和截图归档在 `build/hero-ui-qa/`。构建前后 162 个前端输入文件摘要一致。CI 安装私有 Pro 包使用 `HEROUI_AUTH_TOKEN` secret；推送前核对仓库 Secret 列表，尚未配置该 Secret。

## 本地预览

分别运行 mock 与生产预览：

```powershell
bun qa/mock-server.mjs
$env:PORT = '11987'
bun node_modules/vite/bin/vite.js preview --host 127.0.0.1 --port 5190
```

访问 `http://127.0.0.1:5190`。该 URL 展示本地构建与模拟数据。
