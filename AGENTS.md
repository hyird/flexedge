# FlexEdge 前端：shadcn-admin 重构规范

本规范适用于 `web/` 下的 React 前端。前端的唯一视觉与工程母版是
[`satnaing/shadcn-admin`](https://github.com/satnaing/shadcn-admin)。旧 FlexEdge 前端代码、
旧组件封装和旧页面样式不得作为新实现的设计依据。

## 1. 技术基线

- React 19、TypeScript、Vite 8。
- Tailwind CSS 4（CSS-first）与 `@tailwindcss/vite`。
- shadcn/ui `new-york` 风格，底层交互使用 Radix UI。
- TanStack Router、TanStack Query、TanStack Table。
- Lucide 图标、Sonner 通知、React Hook Form 与 Zod 表单校验、Zustand 轻量状态。
- 禁止引入或继续使用 Ant Design、Material UI、Element 等第二套组件系统。

## 2. 母版一致性

- 应保留 shadcn-admin 的整体结构：可折叠侧栏、移动端 Sheet、顶部 Header、全局搜索、
  主题切换、用户菜单、内容区宽度、固定布局和响应式行为。
- 页面标题、卡片、Tabs、筛选工具栏、数据表、分页、Dialog、Drawer、Dropdown、Badge、
  Skeleton 和空状态应优先复用母版已有实现与组合方式。
- `web/components/ui/` 是 shadcn/ui 基础组件层。业务页面不得重复实现已有组件。
- `web/components/layout/` 是统一布局层。页面不得自建另一套侧栏或 Header。
- 允许把母版示例业务替换成 FlexEdge 业务，但不得任意改变其视觉语言、密度、圆角、
  色彩体系、交互反馈和响应式策略。
- 复制或实质改编上游代码时必须保留 MIT 许可证和第三方署名。

## 3. 目录与职责

- `web/components/ui/`：shadcn/ui 原子组件，仅处理通用视觉与交互。
- `web/components/layout/`：侧栏、Header、Main、用户菜单、全局搜索。
- `web/components/data-table/`：通用表格、列头、过滤、分页、批量操作。
- `web/features/<domain>/`：FlexEdge 业务页面、表单、列定义、对话框和领域类型。
- `web/routes/`：TanStack Router 文件路由，只负责路由装配与鉴权边界。
- `web/lib/`：API 客户端、错误处理、格式化与无业务状态的工具。
- `web/context/`、`web/stores/`：跨页面上下文与最小必要状态。

页面只负责编排。出现两次以上的交互或视觉组合应提炼到共享组件，不得复制粘贴。

## 4. 样式与主题

- 使用母版的 `web/styles/index.css` 与 `web/styles/theme.css` 作为主题基础。
- 不得新建 Tailwind 3 风格的 `tailwind.config.js`。
- 动态或可覆盖类名必须通过 `cn()` 合并。
- 优先使用 Tailwind 工具类和 CSS 变量；不得为普通布局、间距、颜色、圆角或阴影新建
  页面级 CSS 类。
- 不得使用无解释的 `!important`，不得依赖自动生成类名或脆弱的 DOM 层级选择器。
- 明暗主题必须同时覆盖页面、侧栏、弹层、表格、表单和通知。

## 5. 交互与可访问性

- Dialog、AlertDialog、Sheet、Dropdown、Select、Tooltip、Command、Checkbox、Switch 等复杂
  交互必须使用 `web/components/ui/` 中的 Radix/shadcn 组件。
- 图标按钮必须有可访问名称；表单控件必须有 Label、错误信息和键盘路径。
- 危险操作必须二次确认并显示影响对象；提交时必须有 loading/disabled 状态。
- 所有资源页必须包含加载、空数据、错误、筛选、分页和刷新状态。
- 窄屏下表格可横向滚动，表单 Dialog/Sheet 必须可完整操作，侧栏必须切换为移动端 Sheet。
- 不能只用颜色表达状态，Badge 文案与图标/形状需同时提供语义。

## 6. API 与数据约束

- 后端 `service/domains/**` 与 `service/features/**` 是 API 契约的事实来源。
- API 客户端统一处理响应 envelope、Cookie 会话、401 刷新、业务错误和字段错误。
- 更新和删除必须按后端契约发送 `If-Match` revision，冲突时提示用户刷新后重试。
- TanStack Query key 必须稳定；mutation 成功后只失效受影响的查询。
- 不得使用伪数据冒充真实接口结果。仅用于开发视觉回归的 mock 必须与生产构建隔离。

## 7. 质量要求

提交前至少完成：

- `bun run lint`
- `bun run typecheck`
- `bun test`
- `bun run build`
- 真实浏览器视觉回归：桌面与窄屏、明暗主题、侧栏折叠、搜索、表格、Dialog/Sheet、
  Dropdown、错误态和空状态。

交付说明必须区分静态检查、本地 mock 视觉回归与真实后端联调，不得把其中一种描述成另一种。

## 8. 变更边界

- 本次是完整替换，旧 `web/` 文件不保留兼容层。
- 后续改动仍需保持 shadcn-admin 母版一致性；如确需偏离，必须在代码注释和交付说明中
  写明原因、影响范围、验证结果和回滚方式。
- 业务重构不得顺带修改后端行为，除非用户明确要求。
