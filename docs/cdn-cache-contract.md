# CDN 缓存实现契约

2026-09-09 实施契约。网站直接引用策略；集群不增加缓存字段。所有业务读取使用 SSE `snapshot`/`resource-error`，写操作使用 Cookie 会话及现有 If-Match revision。

## 策略 API

- GET SSE `/api/cache-policies/stream`：`page,page_size,keyword,status`；返回现有分页 envelope。
- GET SSE `/api/cache-policies/options/stream`：策略选项数组，包含 id/name/status/revision。
- GET SSE `/api/cache-policies/:id/stream`：完整单项。
- POST `/api/cache-policies`：创建；PUT `/api/cache-policies/:id`：完整更新，带 If-Match；DELETE 同路径带 If-Match，禁止删除仍被网站引用的策略。
- 复制通过 POST 提交新名称和原规则；启停通过 PUT 完整数据，确认影响网站。

策略字段：`id,name,description,status,revision,rules,website_count,websites,created_at,updated_at`。status=`enabled|disabled`；websites 为只读 `[{id,name,status}]`。写入只包含 name、description、status、rules。

规则数组即执行顺序，最多 64 条。每条字段：

| 字段 | 类型/取值 |
| --- | --- |
| id | UUID |
| name | 非空字符串，最多 100 字符 |
| status | enabled / disabled |
| match_type | all / prefix / exact / extension |
| patterns | string[]，all 时为空，其他类型 1–64 项 |
| action | cache / bypass |
| ttl_seconds | 整数，0–31536000；0 表示复用前验证 |
| query_mode | include / ignore；默认 include |
| status_codes | 整数数组，允许 200、301、302、404；默认 [200] |
| min_object_bytes | 非负安全整数 |
| max_object_bytes | 非负安全整数，0 表示节点对象上限 |
| stale_if_error_seconds | 整数，0–86400 |
| respect_origin_cache_control | boolean；默认 true，仅控制有效期来源，不能绕过 private/no-store/no-cache |
| range_enabled | boolean，默认 true；只提供基于完整缓存对象的单 Range，未缓存/不支持的 Range 直接回源 |

无规则命中则绕过缓存。缓存资格默认限制 GET/HEAD、无认证/Cookie、响应无 Set-Cookie，处理 Vary 和请求/响应 Cache-Control。节点实际缓存键隔离网站、网站 revision、策略 id/revision、协议/host/target 及表示。

## 网站与节点字段

网站 `config` 增加 `cache_enabled:boolean`、`cache_policy_id:string`（空字符串表示未选择）。开启必须引用同租户、未删除策略；停用策略保留引用并让缓存绕过。网站保存的是引用，发布对象嵌入不可变策略快照供节点独立执行。数据库用真实关系列与 JSON 配置保持事务一致，策略修改重新发布引用网站所在的集群。

节点 `config.cache`：`memory_bytes,disk_bytes,max_object_bytes,directory`。新配置默认分别为 67108864、1073741824、268435456、`cache`；这是惰性容量上限，不预分配磁盘。memory_bytes/disk_bytes 为 0 分别关闭对应层；directory 是节点 state 目录下的相对目录，拒绝绝对路径、父级跳转及符号链接。所有字段由 API 校验，旧持久配置通过一次数据库迁移补齐。

缓存任务（刷新/预热）、日志与统计的独立协议由主集成工作补充，不复用策略修改来伪装内容刷新。
