# FlexEdge 协作规范

默认中文回复。本文件只记录关键设计与约束，覆盖控制面 `service/`、边缘节点 `node/`、共享契约 `common/` 与前端 `web/`。

## 工程原则

- 保留用户已有改动，不回滚或夹带无关修改。
- 正确性优先，其次长期可维护性；只为现有调用方和明确需求设计，不预设扩展点，不加仅转发的包装层。
- 重构一次迁移全部调用方，不留旧实现、别名、兼容旁路或占位；外部契约（API、协议、DTO、数据库对象、安装身份）不因内部重构改变。
- C++20，命名与格式遵循 Ruvia 风格（标识符小写下划线、数据成员尾随 `_`、宏 `FLEXEDGE_` 前缀），clang-format 21。
- 不创建 `docs/`；计划、排查与验证记录放 `build/`。

## 分层与目录

```text
service/
├── domains/<domain>/     # 管理 API 与所属领域业务
├── features/<feature>/   # 后台任务、发布、协议和运行组件
├── middleware/           # 认证与请求边界
├── config/               # 进程配置与结构化迁移
├── common/               # 公共响应、错误、无业务归属的数据库表达式
├── utils/                # 无业务归属的无状态工具
└── server.cpp            # 进程装配与控制器注册
node/
├── control/              # 控制回路：认证、控制通道、日志投递
├── data/                 # 数据面：监听、协议、回源、路由规则
├── runtime/              # 配置激活、运行状态、安装与指标
└── proto/                # 节点协议定义
common/                   # 控制面与节点共享的纯契约
web/
├── features/<domain>/    # 页面、表单、DTO、校验与数据操作
├── components/           # 跨页面通用视觉交互；layout/ 为应用布局
├── lib/                  # API 客户端、SSE、Query 与无业务归属的工具
└── routes/               # 路由装配与鉴权
tools/ qa/ tests/         # 离线工具、本地验收环境、自动化测试
build/                    # 构建、生成文件、临时探针与过程记录
```

- 同一业务的页面和接口归同一领域；不建空镜像目录，不为拆文件增加分层。私有实现留在所属领域或组件，确有跨领域复用才提取。
- 文件名小写下划线并描述单元本身（`website_controller.h`、`website_read_service.h`），同一职责集中在一个文件，不建 repository、queries、helper 或同职责变体。
- include 使用 `service/...`、`node/...`、`common/...` 仓库路径；源码、依赖与生成产物分离，前端生成文件不手工修改。

## 文件职责

| 职责 | 示例 | 约束 |
| --- | --- | --- |
| controller | `website_controller.h` | 只管路由、中间件、参数与响应 |
| service | `website_read_service.h`、`website_command_service.h` | 业务校验、revision、查询、写入与事务；不依赖 controller 或 runtime |
| store / entity | `reference_store.h`、`projection_entity.h` | 持久化映射与明确的数据读写，不承载另一套业务流程 |
| types / schema / error | `website_types.h`、`website_schema.h`、`website_error.h` | DTO、请求模型、校验与稳定错误；不执行 I/O |
| mapper | `website_runtime_mapper.h`、`mapper.h` | 实体、DTO、配置与协议之间的转换；不执行 I/O |
| worker | `dns_sync/worker.h` | 后台任务的认领、调度与生命周期 |
| model / validation | `website_config/model.h`、`validation.h` | 发布配置契约与规则，不依赖管理接口 DTO，校验与序列化只有一份 |
| transport | `dns_sync/transport.h` | 网络与 SDK 适配；不做业务数据库操作 |

- 依赖无环；实体、DTO、配置与协议各表达一种契约，不因字段相似混用。
- 协议编解码不做数据库或网络 I/O；配置类型和默认值不读业务数据库。
- 跨领域只用公开入口与纯契约，不访问内部状态；共享层不存放业务运行对象，不反向依赖业务，不借回调、全局对象或装配入口绕过边界。
- 前端：页面通过所属 `web/features/<domain>/` 的数据操作访问接口，UI 不直接调用 HTTP 客户端；共享层不依赖领域页面或路由。

## Worker 模型

有连接的工作跟连接走；无连接的工作谁认领谁做完。控制面所有 Worker 是同一套自动机的副本。

- 控制面只有统一的 Web Worker 池，每个实例装配相同的 HTTP、SSE、节点 WebSocket 与任务认领循环；禁止专用 Worker、协调器或主实例。
- 不按编号、下标、哈希、分片或资源 ID 分配职责；`workers().front()`、`workers[0]`、`index % n` 不得表示特权。
- 各 Worker 独占自己的连接、缓存、订阅、定时器与任务状态；Worker 之间不共享可变对象、不互调、不转交连接，只经 Redis 或数据库协调。
- 节点连接从认证到断开归接入它的 Worker；节点业务走一条 WebSocket + Protobuf 通道，通道内只访问 Redis，需持久化的上报经 Redis 可靠记录由各 Worker 竞争认领。
- 无连接任务由各实例独立认领、认领者做完，事务与幂等防重，持久化完成后才确认；消息与任务记录不指定执行实例。
- 节点控制回路与数据面只互投不可变快照，不共享可变运行时。
- 取消与停机等待所拥有的操作结束；恢复与清理按实际归属覆盖重启、缩容、异常退出与过期。

## 数据与协议

- Ruvia 固定完整提交 SHA，只用公开 API；运行时 SQL 用 `db_query`/ORM 参数化绑定，不拼 SQL 字符串。
- 持久化映射归所属实体文件并实际参与查询；DTO、实体、校验与数据库约束一致，区分缺省、null、默认与清空。
- 迁移只追加，不改已执行迁移；发布与测试使用候选二进制实际生成的迁移 SQL。
- 时间统一 UTC `TIMESTAMPTZ`；保持租户隔离、软删除、revision、锁顺序与幂等。
- Redis 业务记录按业务身份组织键，不按 Worker 分区；Streams 保留消费组、原子性与确认顺序，缓存不掩盖数据库失败。
- 节点协议保持与已部署节点兼容；节点发布先于需要新协议的控制面。

## HTTP 与实时推送

- 查询与写入走 HTTP，用户操作直接返回结果，不等 SSE 回执；SSE 只用于有实时展示需求的视图。
- 推送只由真实变化触发，不发重复或空事件；首次订阅一次快照，恢复时补发或一次恢复快照。
- 每页最多一条活跃 SSE，视图关闭即取消，最后一个实时视图关闭即释放；断线有界退避重连，保活只发注释心跳。
- 禁止业务轮询：不用 `refetchInterval`、定时请求、定时失效或重订阅；SSE 失败不降级轮询，服务端不为 SSE 定时查询。
- 服务端每次请求与订阅独立校验身份、权限与租户。

## 前端

- 只用 HeroUI v3 + HeroUI Pro（React 19、Tailwind 4、TanStack Router/Query），不引入第二套组件系统；组件 API 以 HeroUI Pro MCP 文档为准。
- 资源详情是独立路由，页内 Tabs 不改 URL；列表页的筛选、分页与 Sheet 写入 URL。
- UI 不直接调用 HTTP 客户端；更新与删除按契约发送 If-Match。
- 界面不展示配置版本号；状态固定单行，状态切换不改变布局。

## 验证与部署

- 后端：Release 构建与相关 CTest；数据库、Redis、迁移在隔离实例验证执行、回滚与幂等。
- 前端：`bun run lint`、`typecheck`、`test`、`build`，并用真实浏览器验证页面与 SSE 行为。
- 如实区分已验证、未验证与 mock 结果，不以单平台冒充 CI 通过。
- 控制面 `edge.a-z.xin` / `flexedge.service` / `/opt/flexedge` / 端口 `1102`，节点 `flexedge-node.service`；切换前备份，失败立即回滚，凭据不入仓库。
