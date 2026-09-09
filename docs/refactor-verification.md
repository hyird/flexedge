# 重构阶段验收（2026-09-09）

本轮从持续拆分转向统一验收。当前工作区已经发布，尚未提交或推送；最终产物与线上证据见 [阶段发布记录](release-20260909.md)。

## 已完成验证

| 检查 | 当前结果 | 证据范围 |
|---|---|---|
| 前端 lint | 通过 | `bun run lint` |
| 前端类型检查 | 通过 | `bun run typecheck` |
| 前端测试 | 147 通过，0 失败 | `bun test`，35 个文件、615 个断言 |
| 前端生产构建 | 通过 | Vite 8.2.1，输出 `build/web/index.html` |
| Windows Release 服务端 | 通过 | 当前源码重新构建 |
| Windows 架构回归 | 通过 | `flexedge_architecture` |
| Linux Release 全量构建 | 通过 | 包含最新 Agent 读取层变更 |
| Linux CTest | 27 通过，10 跳过 | 跳过项均需本机 PostgreSQL；未计入实际通过数 |
| Windows PostgreSQL 存储回归 | 10 项全部通过，无跳过 | 全部测试目标按当前源码重新构建；直接调用生产 C++ 存储函数 |
| 当前服务端 HTTP 认证 | 通过 | PostgreSQL、Redis 与实际 Release 程序，登录、Cookie、轮换、并发重放、退出、锁定 |

Agent 对象读取已迁入 `release_objects.store.h`，并验证对象字段、排序、身份与集群范围。
此前临时的 `tests/agent_release_sql_test.py` 已由直接 C++ 存储回归替代并删除。

统一验收发现并修复了 `desired_summary.store.h` 的依赖模板调用缺少 `template` 关键字的问题；
MSVC 接受原写法，GCC 编译失败，说明专项 Windows 构建不能代替跨平台验收。

## 真实节点控制链路

正常 API 创建网站和节点、真实节点认证、网站对象下载与持久化、回环源站 HTTP 转发、仅网站发布更新、节点端点更新、进程重启后恢复、转发和四次请求的访问日志落库查询均已通过。详见 [真实节点回归](agent-runtime-test.md)。未覆盖 TLS/证书对象和真实外部 DNS。

## 数据库整体验收补充

Windows Release 重新构建并串行执行以下 10 项测试，全部通过：
端点占用、网站关系、网站引用锁、网站聚合、部署来源、投递对象、集群发布存储、
集群发布锁、节点发布目标、Agent 状态读写。
其中锁测试使用两个数据库连接及 lock_timeout 验证阻塞行为；这不等于任意并发负载验证。
测试前后专用数据库的 public 表数量均为 0，测试 schema 无遗留，PostgreSQL 已停止。
Linux 下这 10 项仍是跳过状态；Windows 的实际执行结果不能冒充 Linux 数据库联调。

## 尚未证明的范围

- 实际浏览器已检查代表性页面和交互，仍未覆盖每个资源页面的全部错误状态。
- PostgreSQL 存储测试使用表结构投影，不是完整 schema 与全部 HTTP 业务链路验证。
- 两台实际节点升级、既有证书 TLS 转发与日志交付通过；新证书签发、日志断线补偿、网络断线和 ACK 丢失恢复仍未验证。
- 未做性能基准，不能给出性能提升比例或长期稳定性结论。

当前 Agent 回归脚本已加入断线与日志补交场景，但此前运行被自动审批拒绝（blocked by policy）；仅完成语法检查，不计入已通过项。本次实际发布和普通链路验收不替代此故障测试。
