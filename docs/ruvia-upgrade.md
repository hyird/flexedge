# Ruvia 固定提交升级与本地验证

本文前述源码清单、二进制摘要和真实 HTTP/SSE 结果归属于 Ruvia 升级验证批次；随后进行的残留清理另见文末，不将历史候选结果冒充清理后的新候选验收。用户随后授权提交与推送升级及清理代码；这不包含生产部署授权，下述本地验证也不代表推送后的 CI 已通过。

## 依赖与构建输入

- Ruvia 从 `d7fa8d6dd14be4de204172de9bca0b76f7a3643f` 升至此次查询时上游 `main` 的完整提交 `8a14af03c65f593b8ddaa7eebc1c97d05d02fe2b`，不跟随浮动分支。
- 构建读取 `build/_deps/ruvia-src` 的固定源码，不使用 `/home/cloudcli/Ruvia` 中用户尚未提交的修改。
- `vcpkg-overlay/openssl` 来自同一固定 Ruvia 提交，提供 OpenSSL `3.6.4#1`；`vcpkg-configuration.json` 注册 overlay。CI 缓存键包含配置、overlay 和 `CMakeLists.txt`。
- 独立配置验证实际 OpenSSL 为 `3.6.4`，QUIC runtime API 探测通过。主构建旧缓存中的 `3.6.3` 字符串不能作为实际安装版本证据。

本机验证使用 Linux x86_64、GCC `14.2.0`、CMake `3.31.6`、Ninja、Release、`BUILD_TESTING=ON`、`FLEXEDGE_BUILD_WEB=OFF`。本机 vcpkg 工具链提交为 `617ef1c0c422737d117eda00a471ea1be5bad088`，与 CI 固定的 `4bca8fd8654e5ba76f92661db7bfe954768ad8ef` 不同。浏览器探针使用 Bun `1.4.2`，CI 固定 Bun `1.4.0`。这些是本地验证，不代表 CI 矩阵已通过，也不是生产发布验收；本轮没有修改前端源码或重新验收完整前端。

## 迁移范围与保持的契约

- 模型使用当前公开 `RUVIA_MODEL` 和 `ModelOptions`，请求先经 `JsonBody<Model>` 绑定，再由领域 middleware 使用公开 `Validator` 校验；业务处理仍可读取已绑定的 `ValidatedJson`。
- 网站、节点、DNS 和证书的纯配置规则归所属 feature，管理请求校验留在领域边界；删除无实际职责的节点旧 schema，不保留旧宏、私有验证入口或兼容别名。
- 通过旧源码和实际请求验证缺省与显式 null 的差别：缺省保留原中文 required 信息，显式 null 保留旧 invalid_type 首错。邮箱规则也按旧实现对照，而非猜测新 validator 的默认行为。
- 迁移 Worker 注册、SDK handle、回调和取消接口；节点 HTTP/TLS 会话、健康探测及后台数据库操作的 shutdown 等待所拥有的工作结束。
- 日志和资源通知 reader 向本 Worker 的订阅投递，测试使用两个同类 Worker 的真实 Channel 验证目标归属和通知合并，不指定某个同类实例为特权 reader。
- 快照读取直接在原请求、原 Worker 执行，不再将数据库工作 post 到另一个上下文。读取预算、watchdog 收尾和 flight 取消按该组件的实际生命周期验证。

公开 API、DTO 字段、权限与租户范围、错误 envelope、If-Match/revision、节点控制协议和安装身份不因内部 API 迁移改名。`service/config/schema.h` 保持不变。本轮没有进行完整 ORM 重写：保留现有公开 SQL 调用及其锁、事务、参数语义，不宣称已经满足全面 ORM 迁移规范。

## 已完成的验证批次

### Release 与自动化测试

`build/ruvia-upgrade-build/final-release-build.log` 记录预算修复后的完整 Release 构建成功；`final-ctest.log` 记录 **44/44 CTest 通过，无跳过或未执行项，总耗时 40.04 秒**。随后只补强预算测试的两个文件，该项独立复核 **1/1 通过，33.10 秒**，其他 43 项源码未变，复用其验证证据。`final-release-after-test-strengthening.log` 确认所有目标已最新。数据库测试使用隔离 PostgreSQL `16.15`，不是缺少环境时返回 `77` 的跳过结果。

最终 376 个构建输入的清单为 `final-source-inputs.sha256`，清单 SHA-256：`9b098fca56e9efc9bb5f896639245b270d3520e9fba726bd2e00492f23574dca`。候选程序摘要位于 `final-candidate.sha256`；测试补强未改变 server、node 或 manifest。

测试覆盖模型、missing/null、实际 handler 的请求绑定、资源校验、UUID 集合、实体关系、revision 周边持久化、事务与锁、后台关停、节点运行时与协议、健康探测、通知归属、日志以及安装脚本。GCC 14 在协程中比较 vector initializer-list 临时对象时发生内部编译器错误；测试改为命名结果与期望值，未降低断言。

`tests/snapshot_read_budget_test.cpp` 使用专用 loopback Ruvia App，Python stdlib 驱动真实 HTTP/数据库操作；CMake 注册 Python DRIVER，外层超时 `90` 秒，缺少隔离 PostgreSQL 配置返回 `77`。该测试在完整批次中实际通过，不是跳过：同一 DbHandle 连续两次 `pg_sleep(18)`，保留默认单次 30 秒超时，总读取在 `30.003` 秒取消；指标 `0,0 → 3,0`，flight 释放、follower 重新认领及健康查询恢复通过，Worker shutdown 断言正常退出 `0`。

独立 flight generation 测试验证旧 ticket 的复制、complete/cancel 不影响同版本的新 flight；迟到结果丢弃、短预算和原请求 deadline 取消也经过实际 HTTP/PG 运行。watchdog 失败、join 失败的异常清理和 `readOnce()` 的完整收尾主要由静态控制流审查证明，未声称完成 OOM/SDK join 故障注入。重试使用最初绝对 deadline 已由源码审查和补强测试确认：fixture 同步证明 follower 已加入 leader 的 flight，leader 至少再等待 1000ms；follower 总预算 1300ms，重新认领时剩余至多约 302ms，而查询需要 650ms。正确预算使它在 1.302 秒超时；若错误重置为 1300ms，查询会成功并使断言失败。最新 aggregate 实测 30.018 秒，日志 `build/ruvia-upgrade-build/follower-budget-regression.log`。没有运行产品代码变异测试。

### 迁移与 Redis

证据位于 `build/ruvia-migration-probes/`：

- 候选实际生成的 21 条迁移 SQL 完成新库安装；重复运行 `applied=0, skipped=21, changed=0`。
- 修改 `0001` 的 checksum 被拒绝；迁移记录与规范化结构前后不变。
- 故意失败的迁移回滚建表，没有写入该迁移 ID。
- 运行时生成的 SQL 清单 SHA-256 为 `d40893ba0b57de4975a6df8fa6cb9d1a0bdcda3b2de9a02ea0b87fe29b4e604d`。
- Redis 的 SDK PING、Streams 读取/XACK、Lua INCR 实际通过。

真实 HTTP 候选另使用独立库 `flexedge_http_auth_qa`，启动后具有 21 条迁移记录，不与探针的迁移 ledger 混用。最终候选三次启动均为 `applied=0, skipped=21`，ledger 仍为 21，证据 `build/ruvia-http-qa/final-upgrade-migration-startups.json`。凭据仅保留在 `build/` 私有文件，权限 `600`，不进入日志或本文。

### 真实 HTTP 与浏览器探针

最终冻结产品源码的 HTTP runner **55 项中 54 项通过、1 项失败**，逐项证据：`build/ruvia-http-qa/final-upgrade-http-report.json`。候选 server SHA-256 为 `cb14a7d64e4acfe1146f9d25a31abda7073cb8cf09e24aa5b23b60e18b9f0b13`，与主构建摘要一致；node 和 manifest 也逐项核对一致。

通过项包括匿名 401、登录与 Cookie、me/refresh/logout、登出失效、各领域实际无效写入的 400 envelope、provider 创建、428/412/正确 revision 更新，以及并发 SSE 初始快照、提交后的通知、无即时重复快照、关闭和手动恢复。没有把假 provider 写入描述为真实 provider verify、DNS 或 ACME 联调；未进行系统级 egress 抓包。

最终候选的原生 Chromium 探针 **26 项中 24 项通过、2 项失败**，证据：`final-upgrade-browser-report.json`、`final-upgrade-browser-server-redacted.log`。使用 Google Chrome `153.0.8010.52` 和 Pi 全局安装的 Playwright Core `1.64.0-alpha-1789764292000`，没有变更仓库依赖。两页各一条 EventSource，实际 Cookie 鉴权、ready 和初始快照通过；`EventSource.close()` 后浏览器约 `103ms` 中止请求，观察期内未自动重连，手动恢复只创建一条新连接。

这属于真实浏览器 API/SSE 探针，不是 HeroUI 前端页面验收。最终失败集合与预算修复前的同一 Ruvia 新版本候选一致，没有新增已观测验收失败，比较证据 `final-upgrade-baseline-comparison.json`；这不等于全部 SSE 验收通过，也不是与升级前旧 Ruvia 二进制的完整行为对照。

QA 期间输入清单由 `6cb865…` 更新为 `9b098f…`，仅两个预算测试文件补强、构建和重新测试；产品源码、CMake 与候选程序没有变更。完整最终输入匹配及 source freshness 证据为 `final-upgrade-input-validation.json`，没有把清单变化隐瞒为“完全未变”。

## 明确未通过或尚未证明的边界

1. **既有命名 heartbeat 不符合纯注释心跳规范。** 原提交已有该行为，HTTP 与浏览器探针均据实报告失败。本轮不擅自修改公开 SSE 协议。心跳路径的 session principal 检查是安全鉴权读取，不能与资源快照查询混称；没有服务端查询计数，因此也不能宣称空闲时数据库查询为零。
2. **服务端关闭延迟尚未满足即时资源回收验收。** 浏览器关闭后，服务端请求约在 `15–30` 秒后结束；关闭第二页后未收到 Playwright 网络终止事件。浏览器批次没有观察到 Python 强制 socket shutdown 批次出现的 Bad file descriptor。缺少服务端订阅计数，不能宣称残留订阅为零。
3. 两个真实 SSE 客户端在 `WORKER_THREADS=2` 下并发，但服务日志无 Worker ID，不能据此证明分别落在不同 Worker。独立两 Worker Channel 测试证明的是投递过滤，不是完整线上 Worker 隔离。
4. 现有进程内共享 Hub/快照缓存仍是既有架构限制；本轮不得描述为全面消除了全局可变业务对象。未执行完整前端验收、生产联调或已部署节点升级/回滚验收。

上述验证阶段没有部署、提交 Git 或更改生产配置。用户原有性能文档、结果和测试文件保留。隔离候选停止前 health 为 200，最终正常退出 `0`，关闭耗时 `7ms`，server/supervisor PID 已消失；证据 `final-upgrade-server-stop.json`。本轮三个 Docker 容器已停止，端口 `11022`、`55432`、`55433`、`16380` 均不可连接；保留测试数据及镜像，清理记录为 `build/ruvia-upgrade-build/qa-cleanup.json`。私有凭据保持 `600`，不得复制到发布制品。

## 后续残留、死代码与重复转发清理

本轮按全仓调用、模板/注册和构建入口确认后，仅修改六个代码/测试文件：

- `web/lib/format.ts` 删除没有生产调用的 `initials()`；同步删除其专属测试，其余格式化行为和测试不变。
- `service/features/certificate/config_mapper.h` 删除无人调用的 `complete()`。
- `service/domains/certificate/certificate.schema.h` 删除 `isValidCertificateConfig()` 纯转发，创建校验直接使用同一 `normalize(...).has_value()`，保留中文错误及缺省/null 规则。
- `service/features/live_resource/read_scope.h` 删除未用 `deadline()` getter，预算、组合取消和 watchdog join 不变。
- `service/features/dns/cloudflare.h` 删除无人调用的 `findZoneById()`；实际 `findZone()`、Zone payload、认证和错误分类保留。

没有因为单个调用者就改写 probe 退休回调，也没有机械合并 HTTP/TLS 的不同生命周期或 readOnce/snapshot 的不同职责。`publishForWorker` 确有 access-history 和测试调用，必须保留本 Worker 的投递语义。八项看似无直接 import 的前端依赖仍提供 HeroUI peers；其中可选 peer `recharts` 也因网站详情实际使用 Pro BarChart 而保留。没有新增兼容别名或替代包装层，没有改 SQL、迁移、公开 DTO、协议、依赖清单或锁文件。

清理后重新完成本地 Release 全目标构建，以及隔离 PostgreSQL 环境下 **44/44 CTest，通过且无跳过，总耗时 41.04 秒**。前端 `bun run lint`、`bun run typecheck`、`bun run test`（35 个文件、161 项）、`bun run build` 均通过。证据位于 `build/cleanup-audit/` 的 `release-build.log`、`ctest.log` 和 `frontend-*.log`。

另用当前构建页面做了桌面、窄屏与深色的 **mock 浏览器冒烟**：登录/未登录跳转、侧栏与供应商导航、账户头像、创建 Dialog/必填焦点/取消、主题菜单和页面搜索可操作。mock 初次登录响应遗漏 `data.user` 和未关闭主题菜单导致的点击等待已修正，未修改产品源码；有限 SSE fixture 会主动结束并触发预期重连状态，本次不声明真实 SSE 资源回收、空闲无查询或完整 DataGrid 后端联调通过。截图、快照、网络及 mock 边界记录在该证据目录；之前真实 SSE 的两项未通过边界仍保留。

用户性能文件的起始与结束 SHA-256 一致。清理验证阶段没有提交或部署；临时前端预览和本轮重新启动的 PostgreSQL 验证容器在验证后停止。
