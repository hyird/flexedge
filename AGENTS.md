# FlexEdge 前端：Ant Design 与 Tailwind CSS 联合开发规范

本规范适用于 `web/` 下的 React 前端代码。关键词“必须”“禁止”“应”“可以”分别表示强制要求、禁止项、默认要求和允许选项。若与用户在当前任务中的明确要求冲突，以用户要求为准，并在交付说明中记录偏差与原因。

## 1. 技术基线

- 组件系统：Ant Design 6。复杂交互、可访问性、状态管理和业务组件行为优先使用 Antd。
- 布局与样式：Tailwind CSS 4。项目采用 CSS-first 配置和 `@tailwindcss/vite`，不得照搬 Tailwind 3 的 `tailwind.config.js` 配置方式。
- 适配层：`web/components/ui/` 是项目对 Antd 的统一封装层，不是第二套组件库。页面优先复用这里的组件；跨页面的视觉或行为变更应先评估是否应落到共享适配层。
- 类名合并：动态或可覆盖的 Tailwind 类必须通过 `cn()` 合并，避免互斥工具类产生不可预测结果。

## 2. 职责边界

### 2.1 Ant Design：组件与交互

以下内容优先由 Antd 或 `@/components/ui` 提供：

- `Table`、`Form`、`Select`、`Modal`、`Drawer`、`DatePicker`、`Dropdown`、`Tabs`、分页、通知等复杂组件。
- 焦点管理、键盘操作、弹层定位、加载/禁用/校验状态等交互行为。
- 组件自身的尺寸、状态、变体及主题能力。

不得为了复刻已有 Antd 能力而用 Tailwind 重写复杂交互组件。不得依赖 Antd 未公开、易变化的深层 DOM 结构。

### 2.2 Tailwind CSS：布局与轻量视觉

以下内容优先使用 Tailwind：

- 页面级 Grid/Flex 布局、响应式断点、宽高、间距和溢出控制。
- 文字排版、简单背景/边框/阴影，以及非交互或低交互的展示元素。
- 传给 Antd `className`、`classNames` 或 `styles` 的局部、非破坏性微调。

Tailwind 不负责强行改写 Antd 的内部结构。例如，禁止在页面中使用任意选择器批量修改 `.ant-table-thead`、`.ant-select-selector` 等内部节点。

默认不得新增自定义 CSS 类。能够由 Antd API、Design Token、Tailwind 工具类、现有 CSS 变量或共享组件表达的样式，必须使用这些机制完成，不得再创建 `.page-card`、`.custom-button`、`.xxx-wrapper` 等仅用于组合常规样式的类。

## 3. 组件选型与复用规则

按以下顺序选型：

1. 先查找 `web/components/ui/` 和 `web/components/` 中已有的共享组件。
2. 已有适配层能表达需求时，页面不得绕过它重复封装。
3. 需要 Antd 的复杂能力且共享层不适用时，可以直接使用 Antd；若同类用法会出现两次以上，应提炼共享封装。
4. 仅当 Antd 不适合且交互足够简单时，才使用原生元素配合 Tailwind。

全局一致性需求必须在共享层解决。例如“所有表格”应修改共享 `DataTable` 及统一的业务列约定，而不是只修一个页面。

## 4. CSS 加载顺序与层级

`web/styles/index.css` 的 Cascade Layer 顺序是项目契约：

```css
@layer theme, base, antd, components, utilities;
@import "antd/dist/reset.css" layer(antd);
@import "tailwindcss/theme" layer(theme);
@import "tailwindcss/utilities" layer(utilities);
```

规则如下：

- 不得随意调整 `theme, base, antd, components, utilities` 的声明顺序。
- Antd reset 必须进入 `antd` 层；Tailwind utilities 必须进入 `utilities` 层。
- Antd 的运行时 CSS-in-JS 样式必须通过 `<StyleProvider layer>` 注入 `antd` 层。只给 `reset.css` 指定 layer 并不能约束运行时组件样式；未分层样式的优先级高于所有正常的分层样式。
- 禁止默认引入 Tailwind Preflight。不得把上述导入简化为 `@import "tailwindcss"`，因为这会隐式启用 Preflight 并改变原生标签和 Antd 的基础样式。
- 如确需启用 Preflight，必须作为独立架构变更处理，至少回归按钮、表单、图片、表格、弹窗和 Drawer，不得在普通页面需求中顺手开启。
- 如未来启用 Antd `zeroRuntime`，预编译的 `antd.css` 也必须显式导入 `layer(antd)`，不得作为未分层样式加载。
- 全局样式只允许放置 layer 声明、必要重置、主题变量和真正跨页面的基础规则，不得收纳普通页面样式或 Tailwind 工具类的重复封装。

禁止无作用域的全局 Antd 覆盖，例如：

```css
/* 禁止 */
.ant-input {
    padding: 20px;
}
```

## 5. Design Token 与主题

### 5.1 令牌分层与单一事实来源

每个语义值只能有一个事实来源，但不要求所有令牌都归入 Antd：

- **Antd 组件令牌**：主色、成功/警告/错误色、控件圆角、字体、组件尺寸和状态样式由 Antd Design Token 管理。
- **项目语义令牌**：页面画布、表面层级、侧边栏、图表、地图和业务状态等非 Antd 专属语义由项目 CSS 变量管理；可以引用 Antd CSS 变量，但不得复制同一颜色值形成两份配置。
- **局部业务值**：只在单一场景成立的尺寸、坐标和外部品牌/协议色可以保留在组件或业务模块中。

不得在 JSX、CSS 和 Tailwind 主题中分别维护互不关联的同义值。

应用根节点必须同时使用 `<StyleProvider layer>` 和 Antd 6 CSS 变量模式。`StyleProvider` 必须包住 `ConfigProvider`，以保证组件及图标样式进入正确 layer。Antd 6 的 `cssVar` 是对象，不使用旧示例中的 `cssVar: true`：

```tsx
import { StyleProvider } from '@ant-design/cssinjs';
import { App, ConfigProvider } from 'antd';

<StyleProvider layer>
    <ConfigProvider
        locale={zhCN}
        theme={{
            cssVar: { prefix: 'ant', key: 'flexedge' },
            token: {
                colorPrimary: '#1677ff',
                borderRadius: 6,
                fontFamily:
                    'Inter Variable, -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif',
            },
        }}
    >
        <App>{/* application */}</App>
    </ConfigProvider>
</StyleProvider>
```

Antd 主题种子值只允许在根主题配置中定义。业务组件不得再次写入 `#1677ff` 等系统色。

### 5.2 Tailwind 4 映射

在 `web/styles/index.css` 的 `@theme inline` 中把 Tailwind 语义类映射到 CSS 变量，不新建 `tailwind.config.js`：

```css
@theme inline {
    --color-primary: var(--ant-color-primary);
    --color-success: var(--ant-color-success);
    --color-warning: var(--ant-color-warning);
    --color-error: var(--ant-color-error);
    --radius-antd: var(--ant-border-radius);

    /* 项目语义令牌 */
    --color-background: var(--app-background);
    --color-card: var(--app-card);
    --color-sidebar: var(--app-sidebar);
}
```

映射后使用 `bg-primary`、`text-primary`、`text-error`、`rounded-antd`、`bg-background` 等语义类。新增令牌时，先判断它属于 Antd 组件令牌还是项目语义令牌，再扩展 `@theme inline`；不得仅为缩短一个任意值而创建全局令牌。

以下情况可以使用字面值，但必须有明确语义，且不应冒充系统主题色：

- 数据可视化的分类色阶、地图图层颜色和协议/品牌规定色。
- 精确对齐所需的尺寸、坐标或计算值，例如 `w-[72px]`。
- 一次性调试值；提交前必须移除或令牌化。

主题切换时，Antd 算法、Antd CSS 变量和项目语义变量必须同步。不得只切换 `.dark` 而让 Antd 仍停留在亮色主题。

## 6. Antd 样式微调的降级顺序

需要微调 Antd 时，必须按以下顺序选择方案：

1. 使用组件公开的 props、Design Token、component token、`styles` 或 `classNames`。
2. 使用组件自身的 `className`，或在外层容器上处理布局和间距。
3. 修改 `web/components/ui/` 中的共享适配层，并验证所有调用方。
4. 最后才使用有明确业务根类作用域的 CSS 覆盖，并写注释说明原因及依赖的 Antd 结构。

新增自定义 CSS 类仅允许用于以下情况：

- Antd 公开 API、Design Token、`styles`、`classNames` 和 Tailwind 工具类均无法表达的必要样式。
- 需要稳定根作用域来隔离第三方组件覆盖，且无法通过组件公开接口注入样式。
- 复杂动画、伪元素、打印样式或跨元素状态关系，使用工具类会明显降低可读性或无法实现。

自定义类必须保持最小作用域，名称表达业务或组件语义，并在代码旁说明不能使用标准机制的原因。不得仅为了缩短一串 Tailwind 类而创建自定义 CSS 类；重复组合应优先提炼 React 共享组件。

示例：

```tsx
<div className="w-full max-w-md p-4">
    <Select className="w-full" />
</div>
```

禁止：

- 大量使用 `!` 工具类或 CSS `!important` 压制 Antd。
- 使用自定义 CSS 类重复封装 Tailwind 已有的布局、间距、排版、颜色、圆角或阴影工具。
- 使用 `[&_.ant-xxx]` 穿透多个内部层级；除非不存在公开 API，且选择器被业务根类严格限定。
- 通过 DOM 顺序、自动生成类名或 `:nth-child()` 绑定 Antd 内部实现。
- 在多个页面复制相同覆盖规则。

确需 `!important` 时，代码旁必须说明：为何公开 API 不足、影响范围、回归点以及未来移除条件。

## 7. 布局、响应式与可访问性

- 页面布局不得依赖 Antd 内部 DOM 的偶然尺寸；用外层容器明确控制 `min-w-0`、`overflow-*`、Grid/Flex 和断点。
- 表格必须在列不足时填满可用宽度，只有真实横向溢出时才固定操作列；不得仅根据列数猜测溢出。
- 弹窗、Drawer、下拉菜单和表单在窄屏下必须可操作，不得使用无上限的固定宽度。
- 不得用颜色作为唯一状态提示；交互元素必须保留可见焦点态、键盘路径和正确的禁用/加载语义。
- 图标按钮必须提供可访问名称；优先使用 Antd 的 Tooltip/按钮 API，而不是只靠视觉图标表达含义。

## 8. 代码组织约束

- 页面只负责业务编排；通用视觉行为放在共享组件，通用主题值放在 token/CSS 变量层。
- 不得同时创建作用相同的 Antd、原生 Tailwind 和共享 UI 三套组件。
- 样式优先直接使用 Tailwind 工具类，不创建页面级 CSS 类或与组件同名的独立样式文件；确需自定义 CSS 时必须满足第 6 节的例外条件。
- Tailwind 类较长时按“布局 → 尺寸 → 间距 → 排版 → 颜色 → 状态/响应式”的顺序组织；条件类使用 `cn()`。
- 不得为纯样式需求引入新的运行时状态。只有需要测量真实布局（例如横向溢出）时才使用 `ResizeObserver` 等运行时机制。
- 删除或替换共享样式前，必须搜索全部调用方，并保留与当前任务无关的既有行为。

## 9. 审查与验证清单

提交前至少确认：

- [ ] 复杂组件来自 Antd 或现有共享适配层，没有重复造轮子。
- [ ] 没有无作用域的 `.ant-*` 全局覆盖，也没有依赖自动生成类名。
- [ ] 没有可由 Antd、Tailwind 或现有共享组件替代的自定义 CSS 类。
- [ ] 没有无解释的 `!important`、`!bg-*`、`!p-*` 等强制覆盖。
- [ ] Antd 组件令牌与项目语义令牌职责清晰，同一语义值没有多份配置。
- [ ] 没有为 Tailwind 4 新建无效的 Tailwind 3 风格配置。
- [ ] `web/styles/index.css` 的 layer 与 import 顺序未被破坏，Preflight 未被意外启用，Antd 运行时样式已进入 `antd` layer。
- [ ] 共享组件变更已检查全部调用方；响应式、溢出、hover、focus、disabled 和 loading 状态已回归。
- [ ] 已执行与改动相称的验证：`bun run lint`、`bun run typecheck`、`bun test`、`bun run build`。
- [ ] 样式或主题变更已进行真实浏览器视觉回归；至少覆盖相关页面，并按影响范围检查 Table、Form、Modal、Drawer、Dropdown、窄屏和暗色模式。
- [ ] 视觉验收检查了实际计算样式与交互结果，而不只是 DOM 中是否出现预期 class。
- [ ] 交付说明明确区分“本地构建/测试通过”和“已在真实部署环境验证”。

## 10. 规范执行要求

- 改动范围必须与任务目标一致，不得借局部样式需求无边界扩展重构范围。
- 涉及根主题、全局样式或组件基础设施时，必须同步完成影响范围内的适配和视觉回归，不得留下两套并行规则或半迁移状态。
- 共享规则应尽可能通过 lint、架构测试或自动化检查固化；检查规则必须验证真实架构约束，不得只匹配易规避的字符串形式。
- 规范、自动化检查和实际运行行为必须保持一致；出现冲突时，应以用户要求和可验证行为为依据统一修正，不得长期保留相互矛盾的规则。

## 11. 允许偏离规范的条件

只有在第三方组件缺少公开 API、存在已确认的兼容性缺陷，或用户明确要求特殊视觉效果时才允许偏离。偏离必须保持最小作用域，并在代码注释和交付说明中记录原因、影响范围、验证结果与回滚方式。
