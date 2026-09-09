# 节点存储层验证

`service/domains/node/endpoint_claim.store.h` 负责端点占用的清除和批量替换，
事务由节点命令服务持有。`service/features/node_dispatch/target.store.h` 负责
排除待处理发布目标，节点命令服务决定何时调用及是否限定原集群。

创建和更新路由都注册 `NodeConfigValidator`，端点数量限定为 1 至 8 个；
存储层每个端点绑定四个参数，因此这两条 HTTP 路径最多产生 32 个 INSERT 参数。
数量规则属于请求校验层。存储层仍接受空列表，语义为清除该租户下指定节点的占用；
直接调用存储函数的内部代码不自动获得请求校验保证。

请求层按规范化 UUID 和解析后的 IP 地址判重，避免 UUID 大小写、IPv6 缩写或
大小写差异绕过校验。IPv4 和 IPv4-mapped IPv6 仍是不同地址。
端点不接受 `%` 接口作用域后缀，例如 `fe80::1%1`，因为持久化的 `inet` 地址
不包含本机接口作用域；不带后缀的 IPv6 地址仍按通常格式规则校验。
`flexedge_node_endpoint_validation` 覆盖这些规则，无需数据库连接。

## 运行

准备一个专用、空的 PostgreSQL 数据库，名称以 `_auth_qa` 结尾。
这里沿用认证测试的数据库命名约束，不能使用生产数据库。
端点测试会创建并提交 `sys_node_endpoint_claim` 测试表，结束时删除；
已有同名表会导致创建失败，不会被测试清除。测试进程被强制终止时，
应检查专用数据库是否遗留该表，再决定重建测试数据库。

Windows PowerShell 示例（PostgreSQL 需预先启动）：

```powershell
$env:PGHOST = '127.0.0.1'
$env:PGPORT = '55439'
$env:PGUSER = 'auth_qa'
$env:PGDATABASE = 'flexedge_auth_qa'
# 如数据库要求密码，通过 PGPASSWORD 提供。
cmake --build build --config Debug --target flexedge_endpoint_claim_test
ctest --test-dir build -C Debug -R flexedge_endpoint_claim --output-on-failure

cmake --build build --config Debug --target flexedge_node_target_store_test
ctest --test-dir build -C Debug -R flexedge_node_target_store --output-on-failure
```

Linux 使用对应构建目录，单配置构建无需 `--config Debug` 和 `-C Debug`。
运行端点测试的进程必须能通过本机 `127.0.0.1` 访问 PostgreSQL；
WSL 与 Windows 的回环地址可达性取决于网络配置。

## 证据边界

- `flexedge_endpoint_claim` 通过真实 `DbClient` 和存储函数执行动态 SQL，验证
  多行参数绑定、替换、空列表清除、租户隔离及失败事务回滚。
- IPv6 等价写法冲突必须返回 SQLSTATE `23505` 和
  `uq_node_endpoint_claim`，其他异常不能算作预期冲突。
- 测试先提交原占用记录，再触发替换失败，通过新事务确认旧记录仍然存在。
  Ruvia 会终止失败的事务，因此测试不尝试在该事务中恢复 savepoint。
- 缺少 `PGHOST` 或 `PGDATABASE` 时，CTest 将该项标为 **Skipped**；
  这不是数据库联调通过。该项设置了 30 秒超时。
- `flexedge_node_target_store` 直接调用 C++ 存储函数，在临时表上检查租户、节点、
  集群及状态范围，以及标记已应用后清除失败信息；通过完整行比较检查无关记录保持不变。
- 两组测试使用表结构投影。端点表覆盖主键和 IP 唯一约束，但不覆盖节点外键、
  完整 schema 迁移、节点命令服务、HTTP 鉴权、revision 冲突或并发请求。

Windows 上两组测试已使用隔离 PostgreSQL 实际通过。Linux 编译和未配置数据库时
的跳过检查，不能替代 Linux 数据库联调。

## 网站关系投影

`flexedge_website_relations` 使用相同的隔离数据库运行条件和
`tests/support/postgres_test.h` 连接生命周期入口。构建目标为
`flexedge_website_relations_test`。

测试直接调用网站关系存储函数，在事务临时表上验证域名参数绑定、可空 DNS 区域、
证书顺序替换、空集合清除，以及同租户其他网站和其他租户的数据保留。
域名占用查询还覆盖创建时判重、更新时排除自身、其他网站冲突、租户隔离和空输入。
引用存储查询覆盖集群不存在、禁用、删除及跨租户，证书未签发、到期、无到期时间、
删除及跨租户；两张证书中任意一张不可用都必须拒绝整个选择。查询保留共享行锁，
但此测试没有验证并发锁等待。
DNS 区域引用返回具名字段，测试验证更长域名优先、同长度按 sort 排序，
以及删除记录和其他租户记录的排除；不覆盖命令层的域名归属规则。
独立的 `flexedge_website_domain_claim` 无需数据库，直接验证纯业务函数的
嵌套区域选择、大小写、通配符、区域顶级域名限制、缺少区域和外部解析。
区域选择由业务函数按规范化域名的最长匹配决定，反向输入测试确保不依赖数据库排序。
HTTP 错误映射与完整保存事务仍不在该测试范围内。

`flexedge_website_reference_lock` 使用两个独立 DbClient，在专用
`flexedge_website_lock_qa` schema 中验证共享行锁。读事务持有集群、证书和区域引用后，
写事务修改相应记录必须因 100ms `lock_timeout` 返回 SQLSTATE `55P03`；释放读事务后，
相同修改必须成功。Windows 隔离 PostgreSQL 已实际通过。
该测试不验证完整 HTTP 保存流程、死锁重试或并发吞吐。

测试成功创建并提交专用 schema 后才会清理它；已有同名 schema 时创建失败且不清理。
若测试被强制终止，应检查专用测试数据库中的该 schema；CTest 设置 30 秒超时。

`flexedge_website_aggregate` 直接调用主表存储函数，在事务临时表上验证创建默认值、
快照字段、revision 更新、错误 revision 拒绝、租户范围、软删除及删除后禁止更新。
主表存储层对 schema 要求非空的字段直接解包；异常数据不会被替换成 revision 1、空配置
或空集群 ID。测试使用允许 NULL 的临时结构，验证空 revision 和空配置会抛出异常。
Windows 隔离 PostgreSQL 已通过；临时结构不包含完整外键，测试也不调用命令服务的
发布、标记和 DNS 协调流程。与其他数据库测试一样，缺少连接环境时跳过，超时为 30 秒。
Windows 上已通过真实 PostgreSQL 执行；它不覆盖完整表约束、命令服务或 HTTP 路径。
临时表在事务回滚或连接关闭时移除，不创建持久表。

锁定的网站快照包含同一行的 revision。更新命令先拒绝过期 revision，再检查集群、域名和证书，
避免为过期请求获取关联资源锁；不存在的网站仍返回未找到。CAS 更新条件继续保留。
聚合存储测试覆盖快照 revision 的 1、2 和软删除后的 3。
`tests/website_revision_http_test.py` 已在 Windows 隔离服务、真实 PostgreSQL 和 Redis 上通过：
revision 不匹配返回 412/16614；当前 revision 配合不存在集群返回 422/16604；
网站不存在返回 404/16601。失败后配置、revision、状态、集群及更新时间保持不变。
运行需设置 WEBSITE_QA_URL、WEBSITE_QA_ID 和 AUTH_QA_PASSWORD，并准备可往返保存的有效网站配置。
同一 HTTP 测试还覆盖成功修改和恢复名称、revision 连续递增、响应 ETag，以及使用旧 revision 再次写入被拒绝。
真实数据库观察到两代发布记录（superseded、active），目标节点数均为 0；未验证真实节点接收与应用。
该脚本需要可丢弃的网站测试数据。清理时先解除发布及关系投影引用，再删除同步标记、网站及关联资源；
仅删除本轮发布引用且已无其他发布引用的 delivery objects。

`flexedge_deployment_source` 使用事务临时表验证部署快照的租户和集群过滤、删除过滤、
排序、启用状态、证书有效性过滤及双域名读取。部署读取层对必填字段不提供默认值，
测试验证 NULL revision 会报错。Windows 真实 PostgreSQL 已通过；证书材料使用测试占位字符串，
不覆盖证书解析、密钥解密或节点接收。缺少数据库环境时跳过，超时 30 秒。

`flexedge_delivery_object` 在真实 PostgreSQL 临时表上验证内容对象加密往返、相同摘要重复写入不改写密文、
租户隔离，以及错误摘要、错误类型、损坏密文和可解密但内容不匹配时拒绝使用。
测试使用固定的专用测试密钥；对象存储中的序列化明文使用 SensitiveString 管理生命周期。
Windows 数据库测试已通过；未测量进程内存清除，也不覆盖节点传输。缺少数据库环境时跳过，超时 30 秒。

`flexedge_cluster_release_store` 在 PostgreSQL 临时表上验证两代发布分配、当前指针、
schema version、active/superseded 状态及目标计数。仅已注册、启用、凭证齐全且未删除的
同租户同集群节点进入目标表；已注册但禁用的节点仍更新 desired_release_id，保留重新启用后的目标。
Windows 数据库测试已通过；临时结构不包含完整外键和发布加密，不代表真实节点下发验证。

`flexedge_cluster_release_lock` 使用两个独立 DbClient 验证同一集群的分配互斥：
首事务持有分配锁时，另一个分配收到 55P03；首事务提交后，下一次分配得到代数 2。
重复回滚代数 2 后仍可重新分配 2，验证计数和 building 行共同回滚。
Windows PostgreSQL 已通过。测试自行创建并清理 flexedge_release_lock_qa schema；已有同名 schema 时失败且不清理。
测试被强制终止后需检查隔离数据库中的遗留 schema。此项不覆盖完整发布竞争或节点应用。

`flexedge_heartbeat_store` 直接调用心跳存储函数，验证 revision 回退与超过目标版本、
错误租户或 agent、错误摘要、跨集群发布、删除节点和未注册节点都被拒绝且整行保持不变。
合法相同版本与递增版本更新运行数据和心跳时间。Windows PostgreSQL 已通过；
不覆盖网络身份认证、上报解析及 fanout 通知。缺少数据库环境时跳过，超时 30 秒。

该上报存储测试也覆盖应用结果：成功结果清除旧错误，已删除节点不能记录成功或失败；
成功后迟到的失败返回 alreadyApplied 且整行不变，未完成部署的失败返回 recorded 并保存错误字段。
失败存储使用 ApplyFailureOutcome 区分 rejected、alreadyApplied、recorded，命令层负责业务错误和通知。
