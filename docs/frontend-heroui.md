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

重做阶段的浏览器数据来自 `qa/mock-server.mjs`，只用于本地验收，不进入生产构建。mock smoke 同时覆盖 DTO 路由、内存 CRUD、缺失/旧 revision、关联删除约束和 SSE 事件；它不代表真实控制面、数据库或节点联调。重做阶段未做真实后端联调、Linux 构建、远程 CI 或生产部署；后续上线验收见下文。

构建日志、源码输入摘要、产物 SHA256、网络记录和截图归档在 `build/hero-ui-qa/`。构建前后 162 个前端输入文件摘要一致。CI 安装私有 Pro 包使用 `HEROUI_AUTH_TOKEN` secret；推送前核对仓库 Secret 列表，尚未配置该 Secret。

## 本地预览

分别运行 mock 与生产预览：

```powershell
bun qa/mock-server.mjs
$env:PORT = '11987'
bun node_modules/vite/bin/vite.js preview --host 127.0.0.1 --port 5190
```

访问 `http://127.0.0.1:5190`。该 URL 展示本地构建与模拟数据。

## 2026-10-01 生产部署

- 14:26（Asia/Taipei）部署到 `https://edge.a-z.xin`，目标为 `103.236.69.112` 的
  `/opt/flexedge`、`flexedge.service`、`127.0.0.1:1102`，数据库为本机 `flexedge`。
- 来源为提交 `ccad2d6087e2dded9c98ddacd28e298b5bba5cfe` 的
  [CI 运行](https://github.com/hyird/flexedge/actions/runs/36817211887)，第 2 次尝试全部通过。
  仓库已配置 `HEROUI_AUTH_TOKEN`。制品压缩包摘要与 GitHub Artifact 元数据一致，
  页面摘要也与本地验收构建一致。
- 原子替换 `web/index.html` 和对应提交的 `licenses/frontend-notices.md`，
  随后重启控制面。服务 `active/running`、`NRestarts=0`，本机 HTTP 与公网 HTTPS
  健康检查通过，两处实际响应的页面 SHA-256 均为
  `ccdb9d75fba87d02e184b97303f1a2831d868103385400d772f881b6621889a0`。
- 运行中服务端程序 SHA-256 保持
  `4c15bf5974134bcea63a00e22d7778c136ad44e2328baff52f2c0dfbde845ecb`。
  迁移账本仍为 `baseline_20260912`，没有执行数据库迁移或发布节点程序；
  北京、成都节点均保持 `0.3.42` 并在重启后上报新的心跳。
- 真实线上浏览器使用已有管理员凭据完成登录，检查概览、集群、节点、网站、DNS、
  证书、缓存策略、供应商及任务列表，以及网站详情和流量统计。
  桌面 1440×960、窄屏 390×844、深色界面及移动菜单可用，最后恢复浅色主题。
  浏览器未记录运行异常。本轮只读检查未保存业务配置、重试任务或执行资源删除，
  不代表所有管理写入和完整端到端回归。
- 旧页面、许可、程序、配置和 PostgreSQL dump 保存在私有目录
  `/opt/flexedge/backups/heroui-20261001-ccad2d6`；`pg_restore --list` 通过，
  恢复页面的摘要校验通过。
  发布清单、CI 日志、前后状态、实际公网资源和浏览器截图归档到
  `build/deploy-heroui-20261001/`，不包含凭据或数据库备份。

回滚须先确认当前页面仍是本次候选，再在控制面执行：

```bash
python3 /opt/flexedge/.deploy-heroui-20261001-ccad2d6/release.py rollback
```

脚本恢复旧页面和许可、重启服务，并核对本机与公网健康及旧页面摘要。

## 日志抽屉修正

节点实时日志和网站访问日志恢复为 HeroUI Pro `Sheet` 右侧抽屉。
节点宽度上限 42rem，网站宽度上限 64rem，窄屏占满视口；抽屉占满高度，
日志与请求详情在内容区域滚动，标题和关闭按钮保持可见。
日志文本可选择复制，关闭按钮、Escape 和遮罩均支持关闭。

基于 `ae06f03` 的隔离工作目录通过 lint、typecheck、35 文件 / 161 项 Vitest
测试及生产构建。桌面 1440×960、窄屏 390×844 和深色界面经过浏览器 mock 检查。
节点抽屉为 672×960，网站抽屉为 1024×960，窄屏网站抽屉为 390×844。
打开日志只持有一条对应日志 SSE；关闭后释放日志并恢复列表 SSE；
切换历史日志取得一次快照后没有活跃 SSE。mock 证据和截图存放于
`build/log-drawer-qa/`，不代表真实日志联调。

日志修正在 2026-10-01 21:13（Asia/Taipei）以 `d264d82` 上线。
实际公网入口 SHA-256 为
`b38499ec420cb70197dedda75850baac87dc8eb9ef7a03d28486aa0a128e257a`。
线上节点日志抽屉取得真实记录，运行程序摘要、迁移账本及节点版本保持一致；
两个节点均上报切换后的心跳。发布证据位于 `build/deploy-log-drawers-20261001/`。

## 全部业务抽屉与表格滚动

新建、编辑、详情、复制、日志和危险操作确认统一使用共享 Pro `Sheet` 抽屉。
公共 `Dialog` / `Modal` 封装已移除；页面搜索保留 Pro `Command` 的输入、过滤和
键盘导航，置于右侧抽屉中，Escape 先关闭抽屉，Ctrl/Cmd+K 和 Enter 导航保留。
提交期间禁用关闭按钮、Escape 与遮罩关闭。未保存的网站配置确认仍保留草稿并
允许取消关闭，删除确认显示具体对象。

所有 Pro DataGrid 使用自身滚动容器约束高度，数据区在内部滚动，列标题固定在顶部，
分页位于容器外；横向滚动时标题与单元格共用列宽。继续使用可变行高的原生表格，
没有将 tbody 改成独立布局或引入另一套表格实现。

- lint、typecheck、Vitest 及 Bun 两个入口的 35 文件 / 161 项测试、生产构建通过。
- 浏览器 mock 验证网站表单错误与未保存确认、节点表单选择器、集群、DNS、证书、
  两类供应商、缓存策略、任务详情、删除取消和页面搜索键盘行为，覆盖桌面与窄屏、
  明暗主题。
- 31 个 mock 节点与 30 条访问日志用于长表格验证。桌面列表滚动前后表头 y=213，
  页面 scrollY=0，内部 scrollTop 从 0 增至 752.67。窄屏日志横向滚动 390px 时，
  缓存列标题与单元格的 x 坐标一致。
- mock 截图与坐标记录位于 `build/log-drawer-qa/`。这些验证不包含生产业务写入。

`d264d82` 的 [CI 运行](https://github.com/hyird/flexedge/actions/runs/36867032933)
前端 lint 和测试已通过，完整 CI 在后端编译失败：网站统计存在未使用参数，
GCC 14 还报告 DNS model optional 的 `maybe-uninitialized`。本次发布仅使用经过
本地验证的前端制品，没有发布该 CI 的后端程序，也未把完整 CI 描述为通过。
