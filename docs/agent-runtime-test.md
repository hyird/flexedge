# 真实节点控制链路回归

`tests/agent_runtime_http_test.py` 使用真实 Windows 节点程序、服务端、PostgreSQL 和 Redis。
运行时创建临时目录并复制节点程序，凭据仅写入该目录，测试结束终止子进程、删除节点资源并退出登录。

## 前置条件

- 专用完整 schema 的本地测试服务端监听 `127.0.0.1`，使用测试账号；不得使用生产环境。
- 一个专用启用集群，关联的 DNS 区域 runtime 必须含 `records_imported`、`lines`、`record_states`、`conflicts`。
- 区域须有启用的 `default` 线路。本次使用 `.invalid` 域名和 invalid provider 夹具，不验证外部 DNS。
- 运行前该集群没有网站或证书对象；测试会创建网站，端点先用 `127.0.0.42`，再改为 `127.0.0.43`，本机 80/443 端口不能有冲突监听。
- `cmake --build build --config Release --target flexedge_node flexedge_service` 已通过。
- 服务端目录中的 node.exe 和 node-release.manifest 必须与本次节点构建匹配。

DNS runtime 夹具示例：

```json
{"records_imported":true,"lines":[{"code":"default","name":"Default","display_name":"Default","status":"enabled"}],"record_states":[],"conflicts":[]}
```

PowerShell 运行示例：

```powershell
$env:NODE_QA_URL = 'http://127.0.0.1:51102'
$env:NODE_QA_CLUSTER = '<专用测试集群 UUID>'
$env:NODE_QA_BINARY = (Resolve-Path build/node.exe).Path
bun -e 'import { defaultWebsiteConfig } from "./web/features/websites/website-form.ts"; console.log(JSON.stringify(defaultWebsiteConfig()));' | Set-Content build/agent-qa-website-config.json
$env:NODE_QA_WEBSITE_CONFIG = (Resolve-Path build/agent-qa-website-config.json).Path
$env:AUTH_QA_PASSWORD = '<测试管理员密码>'
python tests/agent_runtime_http_test.py
```

## 本次实际证据

1. 通过正常 HTTP API 创建 pending 节点并取得凭据。
2. 启动真实节点，服务端状态变为 registered，applied_node_spec_revision 为 1，且有活动发布和摘要。
3. 节点目录实际生成加密的 `state/active/state.pb`。
4. 按带引号的 If-Match revision 更新节点规格，节点应用版本变为 2。
5. 终止并重启同一测试节点，沿用磁盘状态，保持活动发布并产生新的心跳。
6. 清理后数据库内本次 Agent-E2E 节点的未删除数量为 0，服务端、Redis 和 PostgreSQL 均已停止。

最初失败来自测试夹具缺少 DNS runtime 字段，以及脚本使用 CRLF 凭据和无引号 ETag；
修正夹具和脚本后以上真实流程通过，没有为测试放宽生产校验。

## 非空发布与转发验收补充

同一脚本已扩展并实际通过以下场景：

- 根据当前前端默认配置，通过正常 API 创建 external DNS 模式测试网站。
- 启动随机端口的回环 HTTP 源站，返回本次随机标记、Host 与路径。
- 节点取得网站对象，磁盘 objects 目录非空；通过节点监听地址发出的请求收到源站的完整预期响应。
- 只修改网站回源 Host，确认活动发布改变而节点规格仍为 1；新 Host 实际到达源站。
- 将节点端点迁移到另一个回环地址，确认规格版本 2 并从新地址完成转发。
- 重启节点，确认恢复原发布、产生新心跳且再次完成转发。
- 清理后未删除的 Agent-E2E 节点、网站数量均为 0，源站、节点和服务端测试进程均已停止。

## 访问日志交付验收补充

四次转发使用不同的请求路径标记。每次等待服务端访问日志历史 API 返回对应记录，
核对节点 ID、原始 Host、GET 方法、HTTP 协议、200 状态、非零响应字节数和完整查询参数。
本次观察每个请求只有一个匹配记录，日志 ID 互不重复。真实节点的独立日志连接、
服务端日志处理与数据库历史查询链路实际通过；不据此宣称任意网络故障下 exactly-once。

该证据不覆盖 TLS/证书对象、日志断线补偿、网络中断重连、升级或生产部署。
进程重启恢复不能代替网络中断与 ACK 丢失恢复测试。
## 断线恢复扩展（尚未执行）

当前脚本新增 loopback TCP 转发器，在不中止节点进程的情况下关闭现有连接并拒绝新连接。
计划验证离线时继续使用旧配置转发、恢复后应用新发布，以及离线请求的访问日志补交。
该扩展目前仅通过 Python 语法检查；启动隔离服务并执行测试的命令被自动审批以
“blocked by policy”拒绝，未获得更具体原因。上文已通过的网络证据来自加入此扩展之前的运行，
不能作为断线恢复通过的证据。本次未启动测试监听服务。
