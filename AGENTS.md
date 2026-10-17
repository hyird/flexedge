# FlexEdge 协作规范

默认中文回复。本文件只记录关键设计与约束，覆盖控制面 `service/`、边缘节点 `node/`、共享契约 `common/` 与前端 `web/`。

## 工程原则

- 保留用户已有改动，不回滚或夹带无关修改。
- 正确性优先，其次长期可维护性；只为现有调用方和明确需求设计，不预设扩展点，不加仅转发的包装层。
- 允许在重构中采用合理的设计模式，以明确职责、降低耦合或消除重复；模式应服务于当前需求，不为套用模式增加无实际收益的抽象，不绕过分层、所有权与并发约束。
- 重构一次迁移全部调用方，不留旧实现、别名、兼容旁路或占位；外部契约（API、协议、DTO、数据库对象、安装身份）不因内部重构改变。
- C++20，命名与格式遵循 Ruvia 风格（标识符小写下划线、数据成员尾随 `_`、宏 `FLEXEDGE_` 前缀），clang-format 21；聚合类型允许省略的字段在声明处给出默认值，不靠关闭警告掩盖初始化问题。
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
├── components/           # 跨页面通用视觉交互；layout/ 为应用布局，不依赖 features
├── lib/                  # API 客户端、SSE、Query 与无业务归属的工具
└── routes/               # 路由装配与鉴权；把领域组件注入应用布局
tools/ tests/             # 离线工具、自动化测试
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
- `features/node_presence/` 拥有节点在线窗口、presence deadline 与重建映射，以及将心跳、资源通知、缓存失效原子发布的 outbox participant；`features/live_resource/` 只拥有通用订阅、缓存与持久事件投递，不引用节点领域或 presence 实现。
- `features/auth_session/` 拥有认证主体、会话凭据/cookie 与会话有效性读取；`middleware/request_identity.h` 只读取已绑定的请求身份，登录与用户管理仍归 `domains/auth/`。
- 前端：页面通过所属 `web/features/<domain>/` 的数据操作访问接口，UI 不直接调用 HTTP 客户端；共享层不依赖领域页面或路由。

## Worker 模型

有连接的工作跟连接走；无连接的工作谁认领谁做完。控制面所有 Web Worker、节点所有数据 Worker 分别是同一套自动机的副本，各自完全相同。

- 控制面只有统一的 Web Worker 池，每个实例装配相同的 HTTP、SSE、节点 WebSocket 与任务认领循环；禁止专用 Worker、协调器或主实例。
- 节点使用控制回路与统一的数据 Worker 池。所有数据 Worker 装配相同的监听、协议、路由、回源、日志与统计流程，完整处理自己接入的连接；不设接入、TLS、回源、日志或统计专用数据 Worker。
- 同类 Worker 的组件、装配与处理流程完全相同，仅实例身份、实际持有的连接、在途状态和已认领任务不同；不按编号、下标、哈希、分片或资源 ID 分配职责，`workers().front()`、`workers[0]`、`index % n` 不得表示特权。
- 各 Worker 独占可变运行组件、连接及连接池、缓存、会话、订阅、定时器与任务状态；节点数据 Worker 独占本实例的健康状态、统计计数与日志生产状态。
- 控制面 Web Worker 之间不共享可变对象、不互调、不转交连接，只经 Redis 或数据库协调；不得通过投递、回调或内存转发代办已接入连接的工作与已认领任务。
- 节点连接从认证到断开归接入它的 Web Worker；节点业务走一条 WebSocket + Protobuf 通道，通道内只访问 Redis，需持久化的上报经 Redis 可靠记录由各 Web Worker 竞争认领。
- 无连接任务由认领者做完，事务与幂等防重，持久化完成后才确认；消息与任务记录不指定执行实例。
- 发起者优先：写入任务的 Web Worker 提交后只唤醒自己的认领循环，不广播唤醒；发起者繁忙或退出时，其他 Web Worker 在有界扫描或租约过期后接管。
- 节点数据 Worker 之间只允许共享不可变状态；控制回路与数据面只互投拥有所有权的不可变快照（配置、证书、路由、健康状态、统计、日志与结果），不共享可变运行时。
- 不可变约束覆盖快照及其全部可达业务数据；发布后不通过可写别名、`mutable`、缓存或惰性初始化修改。仅加 `const`、锁或 `atomic` 不构成豁免；接收方持有快照所有权，快照不内嵌另一 Worker 的连接、handle 或回调。
- 节点配置激活由控制回路的 `config_activation` 唯一决定提交与补偿，事务 lease 覆盖 Worker 回滚确认及磁盘恢复；Worker participant 只管理本回路 prepared/tentative 资源，不能自行决定全局回滚，异步补偿不放进析构函数。
- 节点请求固定单个 `worker_execution_view`，同时保活源配置与本 Worker 的值类型路由程序；规则在加载时编译动作、匹配、重写、查询策略与条件，请求执行不解释原始 Protobuf 规则字符串。RE2 及其缓存不跨 Worker，共享源配置不含执行对象。
- 路由结果借用已固定执行视图的元数据和 header；未改写 target 借用请求缓冲，改写与重定向字符串由结果持有，移动后不保存指向自身字符串的 view。原请求与执行视图保活至响应和重试结束。
- 每个 Worker 在加载时按源站选择条件复用不可变 `origin_plan`，编译角色、累计权重和健康查询键的 hash；请求只持有独立 `origin_cursor` 与一次健康快照，不构造或排序候选列表。健康优先是加权 primary/backup 序列的全局稳定分组，未知健康和全不健康仍可回源。
- 回源 header 只借用至同步序列化结束，先应用替换再检查 Ruvia 字段上限；普通字段描述符与 16KiB 首部 scratch 使用请求栈，超大首部按 writer 返回的所需大小分配。异步发送只持有拥有所有权的 wire bytes，一次预留完整头体容量，不保留 scratch 引用供后续读取。
- HTTP/1 解析结果借用首部文本：首次输入转移给 reader 并保活，不把首部存储归还给随后会扩容的 body 缓冲。空闲连接不预留固定 8KiB 请求字符串；不能依据对端声明的完整 Content-Length 预分配体缓冲。
- 缓存文件发送的 64KiB scratch 只在实际发送非空磁盘正文时分配，不内嵌到每个 HTTP/1 或 HTTP/2 子会话；在提交响应首部前取得缓冲，异步写及取消回调全部结束前保持会话对缓冲的所有权，不在 close 时提前释放。
- HTTP 转发解析全部 `Connection` 选项，移除其指定的连接级字段；Upgrade 单独生成本跳握手选项，不原样转发对端的连接控制列表。HTTP/2 响应桥接保留重复字段的语义及顺序，`Set-Cookie` 不折叠成逗号列表。
- 回源交换只借用会话已序列化的不可变请求 wire，不按源站尝试复制完整正文；发送、重连及取消完成回调持续保活所属会话，prepared 请求不在 close 或切换源站时重置。
- HTTP/1 与 HTTP/2 的 `Expect` 决策复用 Ruvia 解析事实，不手工比较单个字段；HTTP/1 的 100 响应与后续读取串行，正文完整或无正文时不额外发送，HTTP/1.0 忽略 100-continue，未知扩展维持忽略策略。
- 响应首部按 Ruvia 的无内容计划转发；HEAD/304 保留合法表示长度，未知时省略可选 `Content-Length`，不把传输正文为空写成表示长度 0。1xx/204 不重新发出源站上被接收端忽略的 `Content-Length` 或 `Transfer-Encoding`；不为此放宽 Upgrade 的协议校验。普通零长度正文仍按正常消息定界处理。
- HTTP/2 回源的合成 write 完成表示 adapter 已拥有请求队列，不等待上传窗口排空才读取响应；完整响应后的 NO_ERROR reset 不丢弃响应。只有上传已结束、响应已消费、读写与协议输出均无残留且无 GOAWAY 时才能复用连接。
- HTTP/2 回源只解析已规范化完整内部 wire 的首部；字段值借用到同步 HPACK 提交结束，普通小写字段名使用栈 scratch，超大字段名单块分配。正文只复制到 adapter 拥有的异步上传队列，上传排空或终止后释放容量，不让空闲连接保留旧的大正文。
- HTTP/2 虚拟流以可移动完成器持有异步操作，不额外共享包装 handler；关闭待写流必须异步完成一次 operation_aborted，并让完成回调持续保活输出缓冲，不只设置 closed 标志。
- 信息响应在最终首部前独立串行转发，保留重复字段语义并过滤全部连接选项；HTTP/1.0 与预热请求不下发。`need_more` 可携带完整 1xx 输出，写完后先消费已缓冲的后续首部；用偏移保留合并输入，不搬移正文。1xx 不提交最终状态、缓存或最终路由首部，H2 同批后续 reset 不抹去已接收的信息响应，缺少最终响应仍须失败。
- Redis Stream 消费者排空后、进入等待前清理自身空记录；各 Web Worker 启动及运行时定期原子回收长期空闲且无 pending 的历史消费者，不删除仍有待确认消息的消费者，不依赖已关闭的连接清理。
- 停止请求可由进程装配发起，实际取消与资源清理在所属回路执行；等待所拥有的操作、协程与完成回调结束后再释放依赖资源。恢复与清理覆盖重启、缩容、异常退出与过期。

## 数据与协议

- Ruvia 固定完整提交 SHA，只用公开 API；运行时 SQL 用 `db_query`/ORM 参数化绑定，不拼 SQL 字符串。
- 持久化映射归所属实体文件并实际参与查询；DTO、实体、校验与数据库约束一致，区分缺省、null、默认与清空。
- 本次重新建立全新安装基线：`service::config::schema_migrations` 仅从 `0001_initial` 创建当前最终业务结构，不支持旧库原地升级，不携带历史迁移、数据回填或旧结构兼容。不得清空或重建现有实际数据库。
- 从此基线开始迁移只追加，不改已执行迁移；发布与测试使用候选二进制实际生成的迁移 SQL。
- 时间统一 UTC `TIMESTAMPTZ`；保持租户隔离、软删除、revision、锁顺序与幂等。
- Redis 业务记录按业务身份组织键，不按 Worker 分区；Streams 保留消费组、原子性与确认顺序，缓存不掩盖数据库失败。
- 节点协议保持与已部署节点兼容；节点发布先于需要新协议的控制面。
- 任务中心采用 GoEdge 模型：每个「对象 + 任务类型」只有一条任务，重新发起重置同一任务；当前状态与重试计划归任务快照，历史只记录执行结果与累计统计。证书执行记录包含 ACME 步骤及关联 DNS 任务；不建立用户操作或子任务层。
- 任务结果只通过绑定领域事务与同一 lease 的 `running_result_writer` 写入；状态转换、历史及依赖恢复不可由调用方分别拼装，提交不接收另一组事务、lease 或结果。quiet 不记历史、不发通知；trace 在业务提交后独立保存，失败不撤销已提交结果。
- 证书恢复进度使用 `validation_progress` / `finalize_progress` 阶段值，未发布订单不可序列化；只有挑战发布事务返回的真实 DNS revision 才能进入 validation 进度。失败处置统一为继续订单、重建订单或终态失败，内部阶段类型不改变持久化 JSON 契约。

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
- PostgreSQL 集成测试的 `PG*` 只指向隔离基库，测试角色需 `CREATEDB`；每个测试进程独占临时库并负责清理，普通测试不强制运行完整迁移。
- 节点请求路径基准使用显式构建目标 `flexedge_node_request_benchmark`；Release、相同 CPU/输入/迭代数交错运行前后版本并核对结果。普通 C++ 分配计数不覆盖 aligned/C 分配；微基准耗时不等于端到端 HTTP 吞吐。
- 前端：`bun run lint`、`typecheck`、`test`、`build`，并用真实浏览器验证页面与 SSE 行为。
- 如实区分已验证、未验证与 mock 结果，不以单平台冒充 CI 通过。
- 切换前备份，失败立即回滚，凭据不入仓库。
