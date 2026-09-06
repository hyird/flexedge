# FlexEdge 部署环境与前端发布

## 主机分工

以下地址由项目负责人于 2026-09-05 确认：

| 角色 | 地址 | 用途 |
| --- | --- | --- |
| Server | `103.236.69.112` | 控制台前端与服务端 |
| Node | `140.143.162.240` | 边缘节点 |
| Node | `47.109.18.81` | 边缘节点 |

纯前端发布只更新 Server，两个 Node 无需更新。Node 的安装目录、服务名和运行状态需要在发布节点程序前另行核实。

## Server 运行信息

2026-09-05 通过 SSH 实际核实：

- SSH：`root@103.236.69.112`，使用已有 SSH 认证，不在文档中保存凭据。
- 运行目录：`/opt/flexedge`。
- 服务程序：`/opt/flexedge/server`。
- systemd 单元：`flexedge.service`，配置文件 `/etc/systemd/system/flexedge.service`。
- 前端文件：`/opt/flexedge/web/index.html`。
- 后端监听：`127.0.0.1:1102`。
- 控制台入口：`https://edge.a-z.xin`，Nginx 反向代理到上述后端。
- 健康检查：`http://127.0.0.1:1102/api/health`。

## 前端发布流程

1. 在仓库根目录运行 `bun run lint`、`bun run typecheck`、`bun test`、`bun run build`。按 AGENTS.md 做浏览器回归，并区分 mock 验证与真实后端联调。
2. 产物是 `build/web/index.html`。计算 SHA-256，上传为 `/opt/flexedge/web/index.html.new`，在 Server 再次核对哈希。
3. 将现有 `index.html` 备份为带时间戳的 `index.html.bak.YYYYMMDD-HHMMSS`。让候选文件继承原文件的属主和权限。
4. 在同一目录用 `mv` 将候选文件原子替换为 `index.html`，随后立即执行 `systemctl restart flexedge.service`。安排短暂重启窗口；重启期间控制台/API 和节点连接可能短暂中断。
5. 确认 `systemctl is-active flexedge.service` 返回 `active`，健康接口返回成功；下载实际 HTTP 页面并核对 SHA-256，最后检查公网 HTTPS 入口。
6. 若验证失败，使用本次备份恢复页面并重启服务，再重复健康和页面校验。

注意：2026-09-05 实测，仅替换磁盘文件后，运行中的服务仍引用旧静态文件，HTTP 下载出现响应长度不匹配。重启后恢复。不能仅以磁盘哈希一致判定发布成功。

Windows 上传示例（PowerShell）：

```powershell
Get-FileHash build/web/index.html -Algorithm SHA256
scp -o BatchMode=yes build/web/index.html root@103.236.69.112:/opt/flexedge/web/index.html.new
```

远程多行 Bash 脚本应先保存为 UTF-8、LF 换行的文件，再用 SCP 上传并执行。不要直接使用 PowerShell 文本管道传给 `bash -s`：即使替换了字符串中的 CR，管道仍可能在末尾加入 CRLF，导致单元名带入回车或 `if` 语句解析失败。各校验命令必须检查退出码；下载先写入文件再计算哈希，避免管道掩盖 curl 失败。

## 2026-09-05 前端体验优化发布

- 范围：共用搜索工具栏、分页、表格加载提示、删除确认提交状态。
- 产物 SHA-256：`014b35bbc2e298f0eb34a9bca11dbd8c4a42db27b35d844497a55cfcde739828`。
- 旧页面备份：`/opt/flexedge/web/index.html.bak.20260905-092547`。
- 已验证：重启后服务 active；Server 本机 HTTP 和公网 `https://edge.a-z.xin/` 首页哈希均与构建一致；本机与公网健康检查均返回 `code: 0`、`status: ok`。
- 未执行：两个 Node 的升级；登录后的真实业务操作端到端验证。

本次回滚命令（在 Server 上运行）：

```bash
set -eu
cd /opt/flexedge/web
cp -p index.html.bak.20260905-092547 index.html.rollback
mv -f index.html.rollback index.html
systemctl restart flexedge.service
systemctl is-active flexedge.service
curl -fsS http://127.0.0.1:1102/api/health
```

## 统一查询交互与后续发布（2026-09-05）

- 所有资源列表页（集群、节点、网站、DNS 托管、证书、DNS 服务商、证书供应商、后台任务）统一使用筛选区右侧的“重置、查询”按钮。
- 输入文字、修改下拉条件和按回车不提交查询；点击“查询”才应用当前条件并回到第一页。相同条件再次查询可以刷新结果。
- “重置”仅恢复控件默认值，点击“查询”后才更新列表。翻页沿用已经提交的条件。
- 创建、添加操作放在筛选区上一行；集群、节点和服务商页面与页签对齐。概览无筛选条件，保留刷新操作。
- 证书供应商接口返回完整列表，其关键词筛选和分页在前端执行，不改变后端契约。
- 本次产物 SHA-256：`f72c566071a17baaf0483fdc5161c5baf9f662d151196f9a2f37fe24b126322e`。
- 本次备份：`/opt/flexedge/web/index.html.bak.20260905-093842`。回滚时使用这一备份，替换上文旧发布示例中的备份文件名。
- lint、类型检查、9 项测试和生产构建通过；本地 mock 浏览器验证查询触发、重置、重复查询、创建面板及桌面/窄屏、明暗布局。
- 已部署到 Server；服务 active，本机 HTTP 与公网 HTTPS 首页哈希一致，健康检查正常。未升级 Node，未执行真实业务写入端到端验证。

## DNS线路滚动与下拉选择（2026-09-05）

- DNS线路详情改为固定表头表格，最大高度为视口高度的 45% 或 24rem 中的较小值，表格内部可纵向与横向滚动。
- DNS记录的线路选择改用标准 Select 下拉，展示完整线路路径，保留服务商列表中不存在的当前配置值。
- 使用 120 条本地 mock 线路验证桌面/窄屏、明暗主题、表格滚到末尾时表头固定，以及键盘选择最后一项；静态检查、9 项测试和构建通过。
- 发布 SHA-256：`0294d085b79fbffba5138a45c6aeb7928a96c76d45364c401da57b61b4356686`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-095517`。
- Server 重启后 active，公网首页哈希与构建一致，健康检查正常。未执行真实 DNS 记录写入测试。

## 层级下拉、单行表格与满高抽屉（2026-09-05）

- 线路下拉按分组层级缩进，父线路和子线路都可选择；选中后展示完整路径。
- 所有资源列表中的主信息和辅助信息统一单行排列，过宽内容通过表格横向滚动查看。
- DNS线路抽屉改为满高弹性布局，线路表格占满顶部信息与底部冲突区之间的剩余空间；不再受上一版 45svh/24rem 的高度限制。表头固定，表内滚动，冲突/错误区限制高度。
- 本地 mock 验证三级下拉选择、单行内容及 120 条线路的桌面/手机抽屉布局；lint、类型检查、9 项测试和构建通过。
- 发布 SHA-256：`520b891d056cbef384107591c01ce1c2fefe08530b7296a5f3aea8873fd19725`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-100136`。
- Server active，公网内容哈希与本地一致，健康检查通过；未执行真实业务写入测试。

## 上一版集群树与节点表格布局（2026-09-05）

- 集群管理改为左侧集群树、右侧节点列表。选择集群立即切换节点范围，URL 保留 cluster_id；节点筛选仍需点击查询生效。
- 左侧支持创建、编辑、删除及分批加载集群；不显示集群搜索/筛选，“全部集群”显示全部节点。
- 右侧移除重复的集群筛选，添加节点默认使用所选集群。手机端上下排列，表格保持单行、支持横向滚动。
- 本地 mock 浏览器验证集群切换、添加节点默认值、桌面及手机布局；lint、类型检查、9 项测试、构建通过。
- 发布 SHA-256：`710e51b0e6568042942a8ef16d429101067399980c954d220b7256f12e9c0aff`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-121028`。
- Server active，公网首页哈希与构建一致，健康检查通过。未执行真实节点或集群写入验证。

## 集群列表与紧凑域名卡片（2026-09-05）

- 集群管理左侧改为平铺的集群列表，移除树形展开/收起和“全部集群”根节点；集群选择继续通过 URL 的 `cluster_id` 切换右侧节点范围。
- 右侧节点表格上方将托管域名、服务商、主机前缀、接入域名和节点在线状态拆成四个等高紧凑信息卡片，每张卡片只保留标签和一行值。
- 节点列表自己的名称/状态筛选仍需点击“查询”后生效，保持与其他资源页一致。
- 发布 SHA-256：`ad7e0536e67bf1bfb500df607ee6f78069df5a26df10ec11d120f313e8539689`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-123054`。
- 已验证：生产页面展示左侧平铺集群列表和四个等高紧凑域名卡片，节点查询控件仍在表格上方；Server active，本机 HTTP 与公网 HTTPS 首页哈希一致，健康检查返回 `code: 0`、`status: ok`。
- 未执行：真实集群/节点写入操作及两个 Node 的升级。

## DNS线路选择器（2026-09-05）

- DNS记录中的线路字段改为独立选择器：点击触发弹层，按线路层级逐级展开，叶子线路可直接选择并显示选中标记。
- 选择器显示完整线路路径；当服务商返回的线路列表不包含当前记录值时，仍保留“当前配置”选项，避免编辑时丢失原值。
- lint、类型检查、9 项测试和生产构建通过；已部署到 Server。
- 发布 SHA-256：`087f227015a29b754952cf4428af52c4759031ee3411b0b06de449136ae920f0`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-152257`。
- Server active，本机 HTTP 与公网 HTTPS 首页哈希一致，健康检查返回 `code: 0`、`status: ok`。

## 线路选择器滚动修复（2026-09-05）

- 线路层级列表改用共享的 Radix `ScrollArea`，固定可视高度并由独立 viewport 接收鼠标滚轮、触摸和键盘滚动。
- 保留层级展开、完整线路路径、当前配置保留和选中状态；未执行真实节点写入。
- 发布 SHA-256：`0bac4596ae0664c3c0b2b27375e3c180ecedaf15d11597afc68a91b4211b5af9`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-153716`。
- 已验证：生产服务 active，公网首页哈希与构建一致，健康检查返回 `code: 0`、`status: ok`；浏览器中线路选择器可通过鼠标滚轮和滚动条浏览列表。

## test.a-z.xin 自定义 Host 诊断（2026-09-05）

- 故障表现：`test.a-z.xin` 的 CNAME 指向 `cn.a-z.xin`（47.109.18.81），节点访问返回 `502 Bad Gateway`；HTTP 仅能正常跳转到 HTTPS。
- 原因：站点源站配置为 `https://edge.a-z.xin:443`，当前 `回源 Host` 为 `$host`，请求带 `Host: test.a-z.xin`，而源站虚拟主机只匹配 `edge.a-z.xin`。
- 处理方向：使用网站编辑页的“回源 Host”配置自定义值（当前站点应填 `edge.a-z.xin`），由站点发布流程生成新集群版本；不在 Nginx 中写死测试域名。
- 已撤回临时 Nginx 别名，恢复备份 `/etc/nginx/http.d/edge.conf.bak.20260905-154820` 并通过 `nginx -t` 后 reload。待“回源 Host”更新发布后再验证测试域名。

## 菜单顺序与自定义 Host 提示（2026-09-05）

- 左侧“边缘资源”菜单顺序调整为“集群管理、网站管理”。
- 分组标题“域名与安全”改为“域名与证书”。
- 网站编辑页的“回源 Host”字段补充自定义说明：`$host` 透传访问域名，也可填写固定域名（例如 `edge.a-z.xin`）。
- 发布 SHA-256：`06b358b111c1bb4eba683246b50390f979cf54203d4f6e01ab094fb2cb9c9b08`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-195327`。
- lint、类型检查、9 项测试和生产构建通过；Server active，本机健康检查返回 `code: 0`、`status: ok`。
- `test.a-z.xin` 仍需在网站配置中将“回源 Host”设为 `edge.a-z.xin` 并发布后再做公网验证。

## DNS 记录代理开关说明（2026-09-05）

- DNS 记录编辑器中的“代理”（`proxied`）开关仅对 Cloudflare 展示；阿里云不展示该控件。
- Cloudflare 开启表示通过服务商代理流量（可使用其 CDN/WAF 能力）；关闭表示仅 DNS 解析，客户端直接连接源站。
- 阿里云记录保存时强制提交 `proxied=false`，避免无效代理字段进入同步流程。
- 发布 SHA-256：`97219aebe3f02cf3f04875f8ccb486c9091fc14b9ef7212c631835a8a5d638df`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-195910`。
- Server active，本机健康检查返回 `code: 0`、`status: ok`。

## 共享视觉优化（2026-09-05）

- 页面标题、说明文字和操作区重新调整层级与间距，内容区在桌面和窄屏下留白更均衡。
- 查询工具栏统一为卡片表面，资源表格增加轻量背景和阴影，表头对比度提高；分页与表格间距同步收口。
- 通过生产浏览器检查 DNS 托管页和 DNS 详情抽屉，详情 Tab、记录表头和侧栏菜单显示正常。
- 发布 SHA-256：`b2a0789ca456e7f6686755aec10b5ec3eb7f4fcc38b0bbc8d10856b7f9145a6a`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-201143`。
- lint、类型检查、9 项测试和生产构建通过；Server active，本机健康检查返回 `code: 0`、`status: ok`。

## DNS 详情页 Tab 重排（2026-09-05）

- DNS 详情抽屉改为“基础信息、记录、线路”三个 Tab，避免线路列表与基础信息重复占用页面空间。
- “记录”Tab 展示固定表头的 DNS 记录表；Cloudflare 显示代理状态列，阿里云不显示。
- “线路”Tab 单独承载线路列表，表头固定且表格内部滚动。
- 发布 SHA-256：`bb10c157ba50545c8bd418c201defd2d172a80df3391b0a18bc7cf829ebd2154`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-200803`。
- lint、类型检查、9 项测试和生产构建通过；Server active，本机健康检查返回 `code: 0`、`status: ok`。

## DNS 记录表头固定显示（2026-09-05）

- DNS 记录编辑抽屉增加固定表头，滚动记录时持续显示类型、主机记录、记录值、TTL、线路和操作列。
- Cloudflare 表头显示“代理 / 操作”；阿里云表头显示“操作”。
- 发布 SHA-256：`c2b7b126573fba927f487159284d5fdb76c190d33cbc89758f8dd6b7bf2382bf`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-200307`。
- Server active，本机健康检查返回 `code: 0`、`status: ok`。

## 节点 IP、中文线路树与相同记录冲突展示（2026-09-05）

- 节点列表将 Endpoint 拆分为“IP”和“线路”两列；IP 显示节点地址，线路显示服务商返回的中文层级名称，无法映射时保留线路代码。
- DNS 详情的“线路”Tab 改为可滚动层级树，线路名称显示中文和代码；树节点默认全部收起，点击节点后再逐级展开。
- DNS 详情和列表对“本地内容、远端内容”相同的重复映射不再标记为可处理冲突，状态显示为“记录一致”，只有内容确实不同的记录才显示冲突操作。
- DNS 列表和详情不再展示内部修订版本号，只显示“本地与远端已同步 / 同步中 / 未同步”的业务状态。
- 后端同步比较逻辑已在 `service/features/dns_sync/worker.h` 准备修正：仅 MX 记录比较 priority，避免非 MX 记录因无意义 priority 差异产生冲突。当前 Server 未具备构建该后端二进制的工具链，此修正尚未部署到运行中的 `/opt/flexedge/server`。
- Node 的 WebSocket + Protobuf 控制通道不承载 DNS 服务商同步；DNS 使用独立的 `dns-sync` 后台 Worker 调用阿里云或 Cloudflare API。空闲轮询间隔为 2 秒，失败后重试间隔为 15 秒，15 分钟执行一次兜底对账。
- 2026-09-05 现场核实：`a-z.xin` 的 DNS 任务 version 63 在 `20:28:05` 提交，`20:28:08` 已完成，零重试；任务不是慢在 Node 通道或队列。运行中旧二进制仍将 `A t1 default 8.160.189.44` 的相同本地/远端内容写为冲突，因此区域 `synced_revision` 停在 60。发布上述后端修复并再次同步后，该内部状态才能闭合。
- 本次前端产物 SHA-256：`14e235675cba2cc2ee6ad9db2aee2b2d61f163da6fc298e86caea011e20473d7`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-203543`。
- 已验证：生产浏览器节点表头为“IP、线路”，DNS 线路树显示中文名称且默认收起；Server active，本机 HTTP 页面哈希与构建一致，健康检查返回 `code: 0`、`status: ok`。
- lint、类型检查、9 项测试和生产构建通过；未执行真实 DNS 写入和后端二进制升级。

## 后台任务完成通知与自动刷新（2026-09-05）

- 登录后的统一布局每 2 秒检查最近 100 条后台任务；首次读取仅建立基线，不会为历史完成任务重复提醒。
- 当任务从等待、执行或重试变为完成时，控制台提示“资源 · 操作 已完成，相关数据已刷新”。同一任务版本只通知一次；同一任务的新版本再次完成会重新通知。
- 任务完成后立刻刷新后台任务、运行概览和受影响的资源查询：服务商、DNS 托管、集群/节点、证书或网站。当前可见页面立即重新请求，其他页面在打开时读取失效后的最新数据。
- 发布 SHA-256：`832a338e29c3094136b07e0bfec6a6a96760ec24ff413697f9d3fad51efe17ab`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-204141`。
- lint、类型检查、12 项测试和生产构建通过；Server active，本机健康检查返回 `code: 0`、`status: ok`。

## 移除运行概览与后台任务页面（2026-09-05）

- 侧栏移除“运行概览”和“后台任务”分组，保留集群管理、网站管理、服务商、域名管理和证书管理。
- 根路径 `/` 与旧的 `/tasks` 地址自动跳转到集群管理，避免旧书签落到已移除页面。
- 后台任务完成通知与相关资源自动刷新继续在登录后的共享布局内运行，不依赖后台任务列表页面。
- 发布 SHA-256：`10609b9760ff97a4ac435bea269944f18e1b4947dc9ed174c4871b3af78cbbe8`。
- 回滚备份：`/opt/flexedge/web/index.html.bak.20260905-204341`。
- lint、类型检查、12 项测试和生产构建通过；Server active，本机健康检查返回 `code: 0`、`status: ok`。

## XDB 本地 IP 地域库（待部署）

- 服务只读取 ip2region 的原生 `.xdb` 文件，不使用其他地域库格式、运行时解包步骤或缓存；不会调用外部 IP 查询服务。
- IPv4、IPv6 分别使用 `/opt/flexedge/geo/ip2region_v4.xdb` 与 `/opt/flexedge/geo/ip2region_v6.xdb`。两者都是 XDB；缺少其中一个时，服务仍可启动，只会跳过对应 IP 版本的国家/地区排行。
- 打包服务器发布物时，使用 `-DFLEXEDGE_XDB_V4_FILE=/absolute/path/to/ip2region_v4.xdb` 和（可选）`-DFLEXEDGE_XDB_V6_FILE=/absolute/path/to/ip2region_v6.xdb`。安装阶段会将文件复制到上述固定路径。
- 生产更新顺序：先上传为同目录 `.new` 文件并核对 SHA-256，再用同目录 `mv` 原子替换；执行 `systemctl daemon-reload && systemctl restart flexedge.service`。安装包中的 `xdb_lookup` 可用 `xdb_lookup --v4 /opt/flexedge/geo/ip2region_v4.xdb --ip 223.5.5.5` 验证查询，再确认服务 active、健康检查成功及看板排行有数据。
