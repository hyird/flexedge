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

## 2026-09-08 WSL 构建与真实环境联调

- 22:11（UTC+8）部署当前工作区到 `103.236.69.112`，更新 `/opt/flexedge/server` 和 `/opt/flexedge/web/index.html`。源码基点为 `444fc82d977a43d94cfa2197950c2da674e5be0e`，包含本次尚未提交的重构；产物以以下哈希识别。
- C++ 在本机 WSL Arch 编译，GCC 16.2.1、glibc 2.44、Release、Ninja，构建目录 `/root/flexedge-build`。Ruvia 使用仓库锁定版本；vcpkg 为 `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`。前端在本机使用 Bun 构建。
- 配置参数：`-DBUILD_TESTING=ON -DFLEXEDGE_BUILD_WEB=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake`。C++ 全部目标构建成功，CTest 9/9；前端 lint、typecheck、Bun/Vitest 各 115 项测试和 build 通过。GCC 16 对上游 `HttpAcceptEncoding.h` 产生 `-Wmaybe-uninitialized` 警告，未修改上游源码或屏蔽警告。

| 产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `server` | 26900000 | `b8bf665dc2628f636bcc0970dbd7ee87059fae7a4696ef4c2054c3fe19b43230` |
| `web/index.html` | 1294505 | `60fb029b0ec6cfbbca420156f27d3846b1af9e923ff56b7bf9428373d9f61170` |

部署前核对了线上文件哈希及数据库 21 条迁移的全部校验值；无待执行迁移。旧服务端、前端和发布清单保存在 `/opt/flexedge/backups/wsl-refactor-20260908-221118/`。切换继承原文件权限和属主，重启后本机 HTTP 与公网 HTTPS 首页哈希均匹配；健康接口成功，服务 `active/running`，自动重启次数为 0。

真实环境验证：

- 登录、注销、重新登录成功；新会话节点心跳继续自动更新，成都和北京均在线。
- 桌面明暗主题、侧栏折叠、全局搜索、390px 移动端侧栏、节点表单必填校验及筛选空状态通过浏览器检查。
- 网站列表显示 2/2 节点已发布；详情看板取得真实统计、DNS 验证结果与源站健康数据；域名列表成功读取 13 条记录的同步状态。
- 打开实时访问日志后，分别经 `47.109.18.81`、`140.143.162.240` 请求 `https://test.a-z.xin/api/health`，附带 `deploy_check=20260908-wsl-chengdu` / `deploy_check=20260908-wsl-beijing`。两次均返回成功，浏览器无需刷新便收到对应 200 日志（22:13:29），验证节点请求、日志上报及浏览器 SSE 链路。

本次没有升级两台节点程序，也没有执行真实资源增删、DNS 写入或证书签发。故上述证据不代表这些业务写入流程全部通过。旧服务停止时出现连接关闭日志；新进程启动后的本次观察窗口没有 ERROR/WARN。

回滚时在 Server 执行以下命令（不修改数据库）：

```bash
set -eu
cd /opt/flexedge
cp -p backups/wsl-refactor-20260908-221118/server server.rollback
cp -p backups/wsl-refactor-20260908-221118/index.html web/index.html.rollback
chown --reference=server server.rollback
chown --reference=web/index.html web/index.html.rollback
mv -f server.rollback server
mv -f web/index.html.rollback web/index.html
systemctl restart flexedge.service
systemctl is-active flexedge.service
curl -fsS http://127.0.0.1:1102/api/health
```

### 22:22 会话失效处理补充发布

查询或提交在会话刷新失败后返回最终 401 时，统一停止资源事件流、取消查询、移除会话缓存并导航到登录页；并发失败只触发一次清理。初始登录失败、403 和网络故障保留各自错误处理。传输错误回调由 QueryClient 注入，认证生命周期仍归 `features/auth/` 管理。

- lint、typecheck、Bun/Vitest 各 118 项测试和前端生产构建通过。
- 当前前端为 1294825 字节，SHA-256 `a207d975b4e28edbe5ca10efb5da24bbf75f8ac4ac3a6cf82ab9624c470e510d`；服务端继续使用上述 WSL 编译产物，哈希不变。
- 回滚到 22:11 发布的备份位于 `/opt/flexedge/backups/wsl-refactor-20260908-222241/`，可沿用上面的回滚步骤并替换备份目录。
- 真实浏览器双标签验证：22:24:27 在第二标签注销；22:24:38 旧标签资源请求和刷新请求收到 401，自动返回登录页。重新登录后两台节点恢复显示在线。未撤销其他管理员会话或修改业务资源。
- Server 本机与公网首页哈希一致，服务 `active/running`，自动重启次数 0，健康接口正常。
### 22:32 任务领域数据边界发布

任务领域拆分为响应模型、显示映射和查询三层。列表、详情及历史接口统一由 `features/tasks/data.ts` 读取与校验，组件不再拼接任务 URL；所有查询接入 AbortSignal，未选中任务及节点任务历史使用 skipToken 禁止请求。分页元数据复用共享 schema，任务类型和状态文案由完整类型映射约束。

- lint、typecheck、Bun/Vitest 各 121 项测试及前端 build 通过；本轮未修改 C++，继续使用已验证的 WSL 服务端产物。
- 当前前端 1295706 字节，SHA-256 `8df9eec92a84a1036ad37c61f8e2ca1a96b1f2acd21387e127431e43c5fb230c`。
- 回滚备份 `/opt/flexedge/backups/wsl-refactor-20260908-223159/`；Server 本机与公网首页哈希匹配，服务 active。
- 真实浏览器核对最近任务列表、DNS 同步版本 87 的详情及成功/失败执行记录、完整列表第 8/8 页，以及节点发布版本 38 的详情；没有执行任务重试或业务配置修改。
## 2026-09-08 22:44 废弃概览前端清理

- 删除无路由入口的旧概览页面及专用 MetricCard，移除概览 query key 和资源事件中的无效刷新目标。首页仍按现有路由进入集群管理；后端概览接口未改动。
- 前端 1295286 字节，SHA-256 `000a61da2edfca0c42a02e4901b9ce810e84c1007ef2a5e8ddeebe9fd3a12bbd`。服务端沿用上述 WSL 产物，哈希不变。
- 回滚备份 `/opt/flexedge/backups/wsl-refactor-20260908-224355/`。本机与公网首页哈希一致，健康接口成功，服务 active/running，NRestarts=0。
- 静态验证：lint、typecheck、Bun/Vitest 各 123 项和 build 通过；随后补充空同步批次不触发查询刷新的测试，两个运行器均通过该文件全部 7 项测试。
- 真实浏览器：登录会话进入集群管理，成都和北京均在线；不刷新页面，两节点心跳从 22:44:25 自动推进至 22:44:54。此次未执行新的业务写入，也未重复完整明暗主题与移动端回归。

## 2026-09-08 22:47 认证会话事件装配边界

- ResourceEventMonitor 从通用组件目录迁入同步领域；鉴权路由负责挂载，AuthenticatedLayout 只负责视觉布局。原事件连接、刷新队列和会话清理逻辑不变，旧路径无兼容导出。
- lint、typecheck、Bun/Vitest 各 124 项与 build 通过。前端 1295350 字节，SHA-256 `f16f79b269eb5e2b9977c3083d428b6b2185fe23ab0148c3601380dda6a1369e`。
- 已部署，回滚备份 `/opt/flexedge/backups/wsl-refactor-20260908-224707/`。公网首页哈希匹配，服务 active/running，NRestarts=0，部署健康检查通过。服务端产物未变。
- 真实浏览器进入集群管理，两节点在线，未刷新页面时心跳从 22:47:09 推进至 22:47:38。未执行新业务写入或重复完整视觉矩阵。

## 2026-09-08 22:51 服务商读取边界与选项分页

- 服务商列表、DNS 完整选项与证书服务商查询集中至 providers/data.ts，三个页面消费同一领域查询定义。DNS 域名页的服务商选项改为完整分页加载，读取均传递 AbortSignal。
- lint、typecheck、Bun/Vitest 各 126 项与 build 通过。HTTP adapter 回归覆盖 101 条跨页选项和后页失败时不缓存部分结果；该规模场景为本地测试数据。
- 前端 1295422 字节，SHA-256 `b9803293190989bc3e14214c589a991b86d4fa6d3fbe28818bd145ecc0cea2af`。已部署，备份 `/opt/flexedge/backups/wsl-refactor-20260908-225047/`，公网哈希匹配，部署健康检查通过，服务端产物未变。
- 真实浏览器：域名页读取 a-z.xin 的 13 条记录，服务商下拉正常展示 Aliyun DNS。生产仅有当前选项，未以生产数据证明多页规模，未重复完整视觉矩阵。

## 2026-09-08 22:54 服务商写入领域边界

- DNS/证书创建、编辑、验证、删除统一至 providers/data.ts；领域输入类型独立于表单，页面无直接 HTTP 调用。保留 DNS 空 token 不更新、编辑不发送不可变平台字段、证书凭据模式映射与 If-Match revision。
- lint、typecheck、Bun/Vitest 各 127 项和 build 通过。新增 HTTP adapter 测试核对写入请求体、地址和 revision；未执行生产凭据修改或删除。
- 前端 1295554 字节，SHA-256 `42f6c687aec9b563dabe404f4b860031c29256f6c17e9d0e896b427026c6b02e`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-225350/`，公网哈希与产物一致，健康检查通过。服务端产物未变。
- 真实浏览器：服务商列表正常读取，添加 DNS 账号表单正常打开并展示各字段及保存按钮；未提交表单，未重复完整视觉矩阵。

## 2026-09-08 22:57 证书供应商凭据语义修复

- 证书表单校验独立成模块：仅原 Access Key 账号编辑允许空值保留；新建及从邮箱切换必须提供密钥，拒绝空白字符和 Let's Encrypt 的 Access Key 模式。
- 数据层省略非当前模式的凭据，留空保留密钥时不发送 access_key。此前发送空字符串与后端 optional 字段语义不符；本次依据后端 schema/service 修正，覆盖旧映射测试。
- lint、typecheck、Bun/Vitest 各 130 项和 build 通过。前端 1295882 字节，SHA-256 `22bfeeb596be70ad5e2fc68462de4b5ff8d99c5ddad36cbb775937fb718966e5`。
- 已部署，备份 `/opt/flexedge/backups/wsl-refactor-20260908-225657/`，公网哈希一致，部署健康检查通过，服务端未变。
- 真实浏览器：两条证书供应商记录正常读取，新建空表单显示“请输入有效邮箱”。密钥保留和模式切换以本地表单及 HTTP adapter 测试验证，未修改生产凭据，未重复完整视觉矩阵。

## 2026-09-08 23:01 DNS 账号表单契约

- DNS 表单校验独立成模块：新建 Token 必须 16–256 字符，编辑仅空值表示保留；替换 Token 同样校验长度。账户标识拒绝内部空白字符，规则依据后端 schema。
- lint、typecheck、Bun/Vitest 各 132 项和 build 通过。新增测试覆盖 0/15/16/256/257 长度及编辑保留场景。
- 前端 1296052 字节，SHA-256 `1a476aad97faaf9e0f4cf60e8cfd02069c546be954999ff50eb41a0181cd4608`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-230108/`，公网哈希匹配，部署健康检查通过，服务端产物未变。
- 真实浏览器新增空表单显示“访问密钥至少 16 个字符”，未填写或修改生产凭据；未重复完整视觉矩阵。

## 2026-09-08 23:05 DNS 领域数据边界

- 域名列表、可用域名查询、创建、记录保存、同步和删除集中至 dns-zones/data.ts，业务 TSX 不再直接调用 HTTP。查询传递 AbortSignal，缓存按筛选和服务商隔离；数据层处理非 Cloudflare 记录的 proxied=false，保留更新/删除 revision 契约。
- lint、typecheck、Bun/Vitest 各 134 项和 build 通过。HTTP adapter 验证缓存隔离、请求参数、取消信号、记录不变性及 If-Match；未执行生产写入。
- 前端 1296298 字节，SHA-256 `0588c4141211f438ef209971e7156b9befd6cbbee2f0636ec03051c01480bef3`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-230513/`，公网哈希匹配，健康检查通过，服务端产物未变。
- 真实浏览器域名列表显示 a-z.xin 的 13 条记录；添加域名可选 Aliyun DNS，随后域名选项为空。空列表当前缺少说明文案，列入下一轮改进；未重复完整视觉矩阵。

## 2026-09-08 23:08 可用域名交互状态

- 添加域名表单区分未选择账号、加载、错误及空结果；错误可重试，空结果可刷新，并使用 FormDescription/status 说明原因。仅选中当前可用域名时启用提交。
- lint、typecheck、134 项 Bun 测试与 build 通过。前端部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-230815/`，SHA-256 `c2a6822d309a3bbaafe12c23f5dab9bec64c62654e8c03c0cb53dc5924888fae`，部署健康检查通过。
- 真实浏览器确认 Aliyun DNS 空结果显示说明、刷新按钮及禁用提交。未执行生产写入；错误重试分支本轮只做静态检查，未注入生产故障，未重复完整视觉矩阵。

## 2026-09-08 23:10 DNS 记录内容保真

- 记录校验迁入 records-form.ts，界面消费领域表单规则。移除 content 的无条件 trim，遵循后端保留记录原文的契约，避免修改 TXT 首尾空白；未增加后端未定义的类型限制。
- lint、typecheck、Bun/Vitest 各 136 项和 build 通过。测试覆盖原文保留、空内容、最大长度以及 TTL/priority 边界。
- 前端 1297221 字节，SHA-256 `766f1adc476219bbf9cd67de86e340b447815ad0a1a8fe008f5692517203b698`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-231045/`，公网哈希一致，健康检查通过，服务端产物未变。
- 本轮未执行生产记录写入或新的浏览器视觉回归，原文保留的证据是本地校验测试，不能视为供应商端到端验证。

## 2026-09-08 23:13 证书领域数据边界

- 证书列表、创建、续期设置、续期、删除、下载统一至 certificates/data.ts；页面仅编排表单、通知和浏览器保存动作。列表按筛选建立查询键并传递 AbortSignal，更新/续期/删除保留 If-Match。
- lint、typecheck、Bun/Vitest 各 138 项与 build 通过。HTTP adapter 覆盖创建请求、三类 revision 操作和下载 Blob/文件名；业务 TSX 已无直接 HTTP 调用。
- 前端 1297469 字节，SHA-256 `cd792a05fdbf333f6d9001f5581a4c462acfd53ac295dc3642085713789d4dc2`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-231310/`，公网哈希一致，健康检查通过，服务端产物未变。
- 真实浏览器列表显示 *.a-z.xin 的有效证书及关联网站。未触发生产签发、续期、删除或私钥下载；未重复完整视觉矩阵。

## 2026-09-08 23:15 证书表单操作校验边界

- 证书表单规则独立至 certificate-form.ts；创建按后端域名标签规则拒绝首尾连字符和超过 63 字符的标签。续期设置只约束 auto_renew，不用创建必填规则校验不可编辑字段。
- lint、typecheck、Bun/Vitest 各 140 项与 build 通过。测试覆盖通配符、连字符、标签长度及续期设置校验范围。
- 前端 1297568 字节，SHA-256 `9bc13d534d186727cdf94b6771fbadd993a5695b0da5ec833d59fdb26e6476e3`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-231513/`，公网哈希一致，健康检查通过，服务端产物未变。
- 本轮未新增浏览器视觉验证或生产签发/续期写入；表单边界以本地测试验证。

## 2026-09-08 23:18 节点线路缓存归属

- 移除仅按集群 ID 缓存所有线路的聚合查询。DNS 领域定义按 zone ID 的线路查询，节点列表通过 useQueries 组合并对重复域名去重，节点表单复用同一查询。集群改绑域名会切换查询键，单域名失败不再使整个聚合 Promise 失败。
- lint、typecheck、Bun/Vitest 各 141 项和 build 通过。新增测试验证同域名并发请求合并及新域名独立读取/缓存；未在生产改绑集群。
- 前端 1300941 字节，SHA-256 `18d3aadb18bfbdf3676dea1572e8ad0644f6b5631e02e52230ad4431d96d08f1`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-231752/`，公网哈希一致，健康检查通过。
- 真实浏览器成都、北京节点均在线且显示“默认”线路；未修改生产节点，未重复完整视觉矩阵。

## 2026-09-08 23:20 节点列表数据边界（本地）

- 节点列表与删除请求迁入 nodes/data.ts，列表查询传递 AbortSignal，删除保留 revision；节点 TSX 不再直接调用 HTTP。
- lint、typecheck、141 项 Bun 测试和 build 通过。本轮未部署，线上仍为 23:18 线路缓存版本，未执行新的浏览器或生产写入验证。
- 审查发现节点 IP 表单仅非空校验，后端使用 Asio 地址解析。后续补齐前端规则前需验证 IPv6 scope 等接受范围。

## 2026-09-08 23:22 节点地址校验与领域表单

- 节点表单校验独立至 node-form.ts，校验 IP 主体并按 Linux Asio 实现允许 IPv6 scope 后缀，保留 45 字符限制；拒绝重复 Endpoint ID 和 IP，在对应行显示错误。
- lint、typecheck、Bun/Vitest 各 143 项与 build 通过。测试覆盖 IPv4、IPv6、映射地址、scope、非法地址与重复项。本轮也发布上一轮节点列表/删除数据层迁移。
- 前端 1301584 字节，SHA-256 `f5576c00f04058c6f2b43d4a59ea3fe9548b0fdf0b298bde73c90ccd4606e0e4`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-232147/`，公网哈希一致，健康检查通过。
- 本轮未新增浏览器视觉验证，未修改生产节点配置；地址校验以本地测试及 Asio 源码对照验证，未宣称所有平台完全一致。

## 2026-09-08 23:24 集群与网站数据边界（本地）

- 集群无限分页、保存和删除迁入 clusters/data.ts。网站列表、详情、删除、DNS 探测和历史访问日志迁入 websites/data.ts，查询传递 AbortSignal，日志保持 schema 解析；DOM/表单/通知编排留在界面。
- lint、typecheck、Bun/Vitest 各 143 项与 build 通过。全 features TSX 检索未发现 getData/sendData/Axios 直接请求，后端路由与 revision 约定保持原契约。
- 本轮尚未部署，线上仍为 23:22 节点校验版本；未新增浏览器或生产操作验证。后续验证这次完整边界迁移后统一发布。

## 2026-09-08 23:27 数据边界回归与发布

- 新增集群无限分页失败/重试测试，确认保留前页并能恢复；新增历史日志无效 DTO 拒绝及查询缓存、过滤参数和取消信号测试。
- lint、typecheck、Bun/Vitest 各 145 项通过，发布上一轮已构建的集群/网站数据边界迁移产物。本轮只新增测试，不改变生产代码。
- 前端 1301921 字节，SHA-256 `c30146d0a0a48e94f80c41efc86dfeebb3999574d7256967406c2f8f93dbcdb3`。部署备份 `/opt/flexedge/backups/wsl-refactor-20260908-232615/`，公网哈希一致，健康检查通过。
- 真实浏览器网站列表和详情正常读取，显示已发布 2/2 节点、三个域名已验证及源站健康。未触发 DNS 探测或配置写入，分页失败与无效日志为本地 adapter 验证，未重复完整视觉矩阵。

## 2026-09-08 23:28 前端 HTTP 导入边界检查

- ESLint 限制 features 下 TSX 导入 api/getData/getAllPages/sendData、Axios 或 api-client；允许 apiErrorMessage，领域 TS 数据模块保留请求能力。
- 实际 ESLint 配置测试覆盖别名导入、相对路径、再导出、namespace 导入、Axios/工厂及允许场景。
- lint、typecheck、Bun/Vitest 各 146 项、build 和 diff 检查通过。本轮仅开发配置和测试变化，无运行时代码变化，未部署或新增浏览器验证。

## 2026-09-08 23:33 后端旧快照模型清理与架构检查修复

- 删除 provider_verification/model.h 的 253 行未使用快照 DTO/解析/variant 代码。全局符号与 include 检索确认无调用方；当前 VerificationTask 由 task_loader 从实时供应商记录加载。
- 修复 C++ 架构测试中的三处旧表单 schema 名称断言与 ResourceEventMonitor 旧路径，反映已完成的领域模块迁移。首次测试确实失败，修复后重新编译架构测试。
- Windows Release 架构测试目标构建通过，当前 build 的 CTest 8/8 通过（10.20 秒）。本轮未重编全部服务端产物或部署，不宣称新的 Linux 运行验证。

## 2026-09-08 23:36 DNS 凭据配置分层

- provider_config.h 拆为 provider_config_model.h（仅标准库数据）、provider_config_transport.h（Ruvia DTO）、provider_config_mapper.h（解析/序列化）。四个实际调用方迁移至 mapper，旧入口删除，无兼容导出。
- 独立模型编译目标新增 DNS 配置模型，未提供框架 include/link 仍编译通过。Windows Release 架构测试目标构建通过，CTest 8/8 通过（9.13 秒）。
- 映射函数和存储格式未改变。本轮未重编全部服务端或部署 Linux 产物，不宣称新的线上运行验证。

## 2026-09-08 23:39 证书配置模型分层

- certificate/model.h 仅保留纯 CertificateConfigData；证书设置 DTO 进入 config_transport.h，供应商凭据 DTO 进入 provider_config_transport.h，解析/输出转换进入 config_mapper.h。
- 命令服务、响应映射、schema、维护任务和供应商配置按职责显式引入所需层；没有兼容重导出。原序列化逻辑不变。
- 纯模型加入无框架 include/link 的独立编译目标并通过。Windows Release 架构测试与完整 flexedge_service 构建通过，CTest 8/8 通过（9.17 秒）。
- 本轮未生成新的 Linux 服务端产物或部署，线上仍为之前已验证版本。

## 2026-09-08 23:42 证书供应商配置与凭据适配分层

- 将 provider_config.h 的纯配置/运行态移入 provider_config_model.h，运行态 DTO 合入 provider_config_transport.h，普通解析/映射移入 provider_config_mapper.h。旧入口删除，全部调用方迁移。
- EAB 密钥加密和运行态保存进入 provider_runtime_credentials.h，仅验证持久化显式依赖该适配层；普通映射不再包含 secret.h 或 ACME 凭据类型。
- 无框架依赖的纯模型编译、架构测试和完整 Windows Release 服务端构建通过，CTest 8/8 通过（9.11 秒）。映射/加密函数体保持原语义。
- 本轮未构建或部署新的 Linux 产物，未执行生产凭据变更。

## 2026-09-08 23:46 后端分层 Linux 构建与发布

- WSL Arch 的 /root/flexedge-build 直接使用当前工作区，Release 全目标构建通过；Linux CTest 9/9 通过（2.54 秒），包含安装脚本语法检查。
- 服务端 26900000 字节，SHA-256 `0510da73d34184d434fa19478a6bc335c0c06fa34ed5d74301c78d7dc0628348`，ELF x86-64，动态库依赖完整。前端仍为 `c30146d0a0a48e94f80c41efc86dfeebb3999574d7256967406c2f8f93dbcdb3`。
- 按发布前哈希和上传哈希核对后部署，备份 `/opt/flexedge/backups/wsl-refactor-20260908-234519/`。线上服务端哈希匹配，服务 active/running、NRestarts=0，公网健康接口成功。
- 真实浏览器读取 Let's Encrypt 与 ZeroSSL 两条已验证供应商记录。未修改凭据或触发签发；读取验证不代表重新验证供应商或签发端到端流程。

## 2026-09-08 23:49 DNS 同步模型分层

- 删除混合职责的 dns_sync/snapshot.h，七个纯数据结构迁入 model.h，十四个 Ruvia DTO 定义迁入 transport.h，JSON 转换迁入 mapper.h。所有调用方按职责迁移，协调算法直接依赖模型层，API 类型依赖传输层。
- 字段与映射函数语义不变，无旧入口兼容导出。DNS 同步模型加入无框架 include/link 的独立编译检查。
- 纯模型、架构测试和完整 Windows Release 服务端构建通过，CTest 8/8 通过（9.14 秒）。本轮未部署，Linux 产物仍为 23:46 发布版本。

## 2026-09-08 23:52 DNS 服务商记录数据契约

- ProviderZone、ProviderRecord、ProviderLine 从网络驱动移入 record_model.h，数据契约仅依赖标准库，并加入无框架 include/link 的独立模型编译检查。
- 架构测试与独立模型编译目标构建通过。本轮未重编完整服务端或部署。
- 协调算法仍通过驱动调用名称规范化，尚未完全移除驱动依赖；下一步继续拆分这部分策略，不能把本轮称为算法完全独立。

## 2026-09-08 23:56 DNS 名称策略与协调算法解耦

- 驱动的本地/远端记录名称转换迁入纯 RecordNamePolicy，驱动提供显式 recordNames()；旧名称方法移除。协调算法改为接收策略对象，网络执行和持久化调用方全部迁移。
- 整个 reconciliation.h 加入无框架 include/link 的独立编译检查并通过；该目标补齐 MSVC /utf-8，解决中文错误消息的代码页误读。转换逻辑保持原语义。
- 纯模型/算法编译、架构测试与完整 Windows Release 服务端构建通过，CTest 8/8 通过（9.18 秒）。本轮未部署 Linux 产物或修改生产 DNS。

## 2026-09-08 DNS 分层 Linux 验证

- WSL Arch Release 全目标构建通过，包含 DNS 同步模型、记录模型与名称策略解耦后的完整服务端及独立模型编译目标。
- Linux CTest 9/9 通过（2.75 秒）。本轮未部署，也未操作生产 DNS；测试结果不代表服务商同步端到端验证。
- 审查发现名称策略与协调计划目前缺少直接行为测试，下一轮补齐纯算法回归覆盖。

## 2026-09-09 DNS 协调行为测试

- 新增无网络/数据库链接依赖的 dns_reconciliation 测试，覆盖名称转换、普通同步冲突、远端优先更新和删除、本地保留、相同快照不变更及重复远端 ID 拒绝。
- Windows Release 新目标构建及 CTest 9/9 通过。Linux 新目标构建和 DNS 测试通过；全套 9/10，node_runtime 出现一次段错误，单独连续复测五次通过。该间歇性失败尚未定位，不将复测通过视为已解决。
- 未修改生产逻辑或部署。下一轮优先排查 node_runtime 间歇性崩溃。

## 2026-09-09 监听器停止线程边界

- GDB 捕获 node_runtime 段错误，工作线程位于 HttpListener::accept / epoll_reactor::start_op。发现测试三处主线程直接 stop 关闭 acceptor，与异步 accept 存在竞争；全部改为 requestStop。HTTP/HTTPS stop 收为私有，外部只能请求所属循环调度。
- Linux 重编 node_runtime 后连续二十次通过（31.36 秒），全套 CTest 10/10 通过（2.80 秒）。本轮未重编 Windows 或部署；有限次数复测不证明不存在其他竞争。
- 后续继续审查 requestStop 投递失败时直接 stop 的分支及停止回调生命周期。

## 2026-09-09 停止投递失败路径

- 审查实际 Ruvia 源码确认停止回调调度到所属 worker；移除 HTTP/HTTPS requestStop 在 mailbox 拒绝时从调用线程直接关闭 socket 的分支。清理直接投递所属 executor，弱引用避免未执行任务持有已停止循环。
- Linux node 和 node_runtime 目标重编通过，CTest 10/10 通过（2.77 秒）；Windows 两目标重编通过。未部署。

## 2026-09-09 监听器启动线程边界

- HTTP start 收为私有，测试三处直接启动全部迁移 requestStart。HTTP/HTTPS 激活门赋值移入所属线程执行，启动状态由所属线程维护，重复启动不增加 accept 链。关闭后的启动回调不再注册激活计时器。
- Linux node/runtime 构建通过，CTest 10/10 通过（2.75 秒）。未重编本轮 Windows 产物，未部署。重复启动及关闭后启动的专门时序测试仍待补充。

## 2026-09-09 监听器生命周期回归

- node_runtime 新增 HTTP/HTTPS 未激活门下重复启动再停止的弱引用释放检查，以及循环 join 后再次 requestStop 不保留监听器的检查。
- Linux 测试目标重编通过，node_runtime 连续五次通过（7.94 秒）。框架不允许在已停止 worker 注册监听器，测试先创建对象再停循环。未部署，本轮未运行 Windows 或全套测试。

## 2026-09-09 生命周期 Windows 验证与审查

- 最近启动边界与生命周期测试通过 Windows node/runtime Release 构建，CTest 9/9 通过（9.36 秒）。未部署。
- 审查实际 Ruvia EventLoopStopRegistration::reset 仅释放注册对象，workerStopping 可把回调移入延迟队列；监听器裸 this 停止回调仍存在生命周期风险，尚未修复。后续需改为共享所有权建立后注册弱引用回调，并迁移十二处创建入口。

## 2026-09-09 停止回调所有权修复

- HTTP/HTTPS 改用 create 工厂，构造函数私有；共享所有权建立后注册弱引用停止回调，移除裸 this 捕获。data_plane 与测试十二处创建入口全部迁移。
- Linux node/runtime 构建通过，CTest 10/10 通过（2.83 秒）；Windows node/runtime 构建通过。未部署。

## 2026-09-09 健康检查调度器所有权

- OriginHealthSupervisor 私有构造加 create 工厂，停止/定时回调使用弱引用，启动任务持有共享引用。外部 stop 移除，所有调用方改为所属线程 requestStop；started/stopped 状态阻止重复启动和停止后重启。DataPlane 成员与两处测试完整迁移。
- Linux node/runtime 构建通过，CTest 10/10 通过（2.76 秒）。本轮未重编 Windows 或部署。DataPlane 自身的裸 this 停止回调及其引用资源的退出顺序仍需审查。

## 2026-09-09 节点异常退出顺序

- main 新增 LoopShutdown 作用域所有者，在启动线程前建立，保证启动/任务/日志异常退出时先 stop/join 两个池，再析构被任务引用的对象。显式停止即使第一个 join 抛异常也继续第二个 join，然后重抛首个异常。
- Linux node 构建通过。新增异常分支专门测试尚待补齐；未部署。DataPlane 的线程边界仍待进一步收紧。

## 2026-09-09 退出异常路径回归

- 新增 loop_shutdown 独立测试：异常展开保留原异常且两池停止回调完成；首池停止回调抛错仍等待第二池完成，显式清理重抛首错且重复调用安全。
- 新目标 Linux/Windows 构建与测试通过；Linux 全套 CTest 11/11（2.80 秒）。Windows 本轮仅运行新增测试，未重编先前健康调度器相关目标。未部署。

## 2026-09-09 DataPlane 停止线程边界

- requestStopAccepting 移除 mailbox 拒绝后调用线程清空监听器映射的分支，改投递所属 executor。DataPlane 仍是进程作用域对象，注释明确依赖 main 的 LoopShutdown 先 join 再析构约束。
- Windows node/runtime 重新构建通过，CTest 10/10（9.45 秒），覆盖最近健康调度器改动；Linux node 构建通过。未部署，未进行真实节点退出端到端验证。

## 2026-09-09 本地节点进程 SIGTERM 验证

- 新增 tests/node_shutdown_smoke.py，临时目录与合成凭据，仅访问占用的 loopback 端口。运行真实 Linux node 进程并发送 SIGTERM，检查退出码 0 和 shutdown requested 日志。
- 连接被拒绝场景 0.018 秒退出；TCP 可连接但 WebSocket 握手无回应场景 9.151 秒退出。后者仍可能等待连接超时，后续审查取消传播，不称为即时取消。此检查不涉及生产控制端、真实配置、代理流量或部署。

## 2026-09-09 控制连接握手取消

- 控制/日志通道共用 connectControlTransport，在建连期间把进程停止令牌注册到 WebSocketClient::abort；修复此前 OperationOptions 只覆盖连接后操作的缺口。
- Linux node 构建通过；本地真实进程 SIGTERM 测试中，握手挂起退出由 9.151 秒降至 0.018 秒，连接拒绝同为 0.018 秒。测试增加 3 秒退出上限防回归。CTest 11/11（2.86 秒）。未重编 Windows 或部署。

## 2026-09-09 自动进程退出回归

- UNIX BUILD_TESTING 增加 Python3 Interpreter 依赖和 node_shutdown CTest（60 秒总超时），正式覆盖连接拒绝、WebSocket 握手挂起、TLS 握手挂起三种 SIGTERM 退出场景，每种退出上限 3 秒。
- 握手场景先 accept 并收到真实握手字节才发送信号，避免仅凭固定等待推断已进入握手。Linux CTest 12/12 通过（4.23 秒），进程退出测试 1.44 秒。未部署。

## 2026-09-09 CI 退出回归依赖

- build.yml 显式安装 python3，保证 UNIX BUILD_TESTING 的进程退出回归依赖有来源；现有全目标构建加 CTest 步骤会包含 node_shutdown。仅完成工作流源文件检查，尚未触发远端 CI。
- 建连取消调整后的 Windows node Release 构建通过。未部署。

## 2026-09-09 认证交换职责集中

- 控制/日志通道重复的认证消息发送、request_id 与 welcome 类型校验、凭据字节清理迁入 authenticateControlEnvelope。业务认证字段仍分别构造，日志 node_id 非空规则保留。
- Linux node 构建通过，CTest 12/12（4.21 秒）。现有测试不覆盖成功认证交换，不能据此宣称认证端到端验证完成；未部署。

## 2026-09-09 认证交换回归

- control_stream 本地 WebSocket 测试新增认证发送后 welcome 成功、request_id 不匹配和非 welcome 响应拒绝三例，调用实际共享认证协程。使用合成凭据，非生产认证。
- Linux control_stream 目标构建及测试通过（0.05 秒）。本轮未运行 Windows 或全套测试，未部署；没有验证凭据内存擦除的底层效果。

## 2026-09-09 认证分层双平台验证

- 移除控制/日志通道不再使用的 secret_buffer 直接依赖，凭据清理由共享认证模块负责。
- Windows control_stream 与 node 重新构建通过，CTest 10/10（9.31 秒）；Linux 全目标增量构建通过。未触发远端 CI 或部署。

## 2026-09-09 日志空闲等待事件化

- 移除 LogChannel 空闲 10ms 轮询，改为可取消 ChannelReceiver 等待。main 建立容量 1 通知通道并注入缓冲区成功入队回调，合并重复唤醒；缓冲区不依赖 Ruvia，确认与重试保留语义不变。
- Linux node/runtime 构建通过。消费者注册前入队、并发通知与停止等待专项测试仍待补齐；本轮未部署。

## 2026-09-09 日志通知行为回归

- 新增 log_notification 独立 CTest，覆盖通知先于等待、跨线程入队与合并唤醒、十条事件完整保留、restore 后仍可重取确认、空闲等待取消。
- Linux 新目标构建通过，CTest 13/13（4.26 秒）。本轮未验证 Windows、生产日志吞吐或线上投递，未部署。

## 2026-09-09 日志成功与失败路径分离

- LogChannel 完整投递成功后直接返回队列就绪判断，不再执行至少 1 秒失败退避；异常与流提前结束仍按原退避策略处理。
- Windows 日志通知、runtime、node 构建通过，CTest 11/11（9.69 秒）；Linux node 构建通过。未进行生产投递延迟测量或部署。

## 2026-09-09 日志投递事务分离

- 对照 agent.controller 确认服务端 enqueue 成功后以 request_id 返回 LogDeliveryAck。客户端的取批次、发送、确认校验、失败 restore 迁入 log_delivery.h，LogChannel 仅编排连接与重试，确认规则不变。
- Linux node 构建通过。下一轮直接对投递事务测试错误确认保留队列、成功确认释放队列；本轮未部署。

## 2026-09-09 日志确认事务回归

- 本地 WebSocket 测试直接调用 deliverLogs：正确确认后 queued/retained 归零；错误 request_id 和错误响应类型均抛错，事件计数保持 1 且可再次 take。
- Linux control_stream 目标构建与专项测试通过（0.07 秒）。仅为本地协议回归，不代表生产持久化或线上日志投递端到端验证；未部署。

## 2026-09-09 配置激活与控制传输分离

- applyPersisted/apply 从 ControlChannel 移至 runtime/config_activation.h，显式接收状态存储、运行时与 DataPlane；通道继续负责消息和错误报告。暂存、验证、prime、持久化激活、运行时发布及失败 abort 顺序保持不变。
- Linux node 构建通过；迁移后的事务专门故障注入尚待补齐，未部署。

## 2026-09-09 事务模块整理与 Windows 验证

- 整理 config_activation 与 log_delivery 提取后的函数缩进。Windows control_stream/node 构建通过。
- 核对现有 node_runtime 夹具仅直接 RuntimeState::apply，不覆盖 StateStore 与 DataPlane 联合激活事务；后续补真实资源事务测试。未部署。

## 2026-09-09 配置激活故障回归

- node_runtime 新增真实 StateStore/DataPlane 联合用例：禁用节点配置成功持久化并发布 revision 3；目标 active 路径被普通文件占用时，revision 4 激活失败，phase 为 ACTIVATE，运行时和原持久化仍为 3。
- Linux 测试目标重编和 node_runtime 通过（1.60 秒）。该用例没有创建活动监听器，不证明监听器撤销；未部署。

## 2026-09-09 配置失败监听器撤销

- 激活故障用例改为启用候选节点、分配临时 HTTP 端口；确认失败 phase 为 ACTIVATE，说明监听器准备已通过。失败后以未启用端口复用的 socket 重新 bind 成功，验证候选监听端口释放，旧运行时仍为 revision 3。
- Linux node_runtime 重编及专项测试通过（1.60 秒）。不代表生产热更新验证；未部署。

## 2026-09-09 激活失败后恢复

- 同一候选 revision 4 在故障激活释放端口后，重新向正常 StateStore 激活，检查存储与运行时均为 4，并实际请求临时 HTTP 端口获得未知域名 421 响应，证明监听器已开始服务。
- Linux/Windows node_runtime 重编通过，Windows 专项测试通过（8.98 秒）。未访问真实业务源站或部署。

## 2026-09-09 配置激活独立测试目标

- 配置激活成功、故障撤销、恢复用例从 node_runtime 移到 config_activation 独立 CTest；配置构造与 HTTP 请求/等待工具提到 tests/support 共享夹具，删除原重复定义。
- Linux 两目标重编通过，CTest 14/14（4.25 秒），激活专项约 0.01 秒。未重编 Windows 或部署。

## 2026-09-09 健康状态事务发布与回滚验证

- OriginHealthRegistry 将候选集合构造与发布分开；DataPlane 在准备阶段构造集合，持久化成功后才在 activate 发布。保留的源站共享状态对象，准备期间的探测结果得以保留。
- Windows node_runtime/config_activation 重编与 2/2 测试通过（10.37 秒）。随后加强激活集成用例：候选源站改名，持久化失败后报告仍为旧源站，成功重试后才报告新源站；专项重编与测试通过（0.71 秒）。
- 本次修改尚未做 Linux 重编、生产部署或真实源站联调。

## 2026-09-09 健康状态成员与探测生命周期

- 健康反馈只更新已发布的源站；移除自动插入路径，避免旧请求和探测恢复已删除记录。Linux 与 Windows 三项相关测试均已通过。
- 探测改为启动时持有 ProbeTarget 状态对象，删除按 ID 回写探测结果的接口。删除后重新添加同 ID 的回归确认旧探测不影响新记录；Linux 三项相关测试通过（2.55 秒）。
- HTTP 请求反馈尚需按尝试绑定状态对象；以上不代表该路径已完成迁移。未部署。

## 2026-09-09 HTTP 健康反馈状态绑定

- Target 统一承载探测与请求反馈，删除按 ID 写入成功/失败的接口。HTTP/1 在每次源站尝试绑定对象，HTTP/2 缓冲与流式交换在创建时捕获对象。
- Linux 三项相关测试通过（2.55 秒），Linux 与 Windows 节点可执行文件重编成功。尚未进行真实请求跨配置切换验证或部署。

## 2026-09-09 真实 HTTP 旧请求反馈隔离

- node_runtime 新增本地上游用例：收到实际 HTTP 请求后删除并重建同 ID 健康记录，再关闭上游连接。断言客户端收到 502，新记录仍健康，覆盖真实请求失败回调的状态对象绑定。
- Linux node_runtime 重编与专项测试通过（1.59 秒）。本用例直接替换健康集合，不代表完整 DataPlane 配置切换或 HTTP/2 跨配置联调；未部署。

## 2026-09-09 健康状态测试分层与候选集合封装

- HTTP/2 缓冲、流式响应前失败及流式成功回调新增真实本地上游隔离回归：重建同 ID 状态后，旧失败不会污染新健康记录，旧成功不会清除新故障记录。Linux 与 Windows node_runtime 专项均通过。
- origin_health 成为仅依赖 Threads 的独立测试目标；注册表阈值、候选集合、旧反馈隔离、报告字段和并发读取测试归入该目标。读线程以 latch 同步启动，网络场景保留在 node_runtime。
- PreparedStates 隐藏内部容器，只能由 prepare 创建，publish 不再接受空指针；DataPlane 所有 prepare 返回路径均经 finishPrepare 初始化候选集合。Linux origin_health/config_activation/architecture 三项验证通过。
- 上述请求用例直接替换健康集合，不代表生产完整配置热切换验证；未部署。

## 2026-09-09 配置校验与发布归属统一

- 版本校验移入 DataPlane.prepare，使用其绑定的 RuntimeState；激活协调函数移除额外运行时参数，避免校验与发布指向不同对象。
- 激活测试新增 revision 4 生效后提交 revision 3：在 VALIDATE 阶段拒绝，运行时与持久化保持 4，现有 HTTP 端口仍返回预期 421。
- Linux 与 Windows 激活专项均通过，Windows 节点重编成功；前一轮 Linux 节点重编通过。未部署。

## 2026-09-09 运行时接口收敛与全量验证

- 删除仅由测试调用的 RuntimeState.apply，配置构造由测试工具承担，RuntimeState 保留版本校验与快照发布/读取职责。
- Windows 节点与 node_runtime 重编成功，运行时专项通过（9.71 秒）。Linux 项目定义的全部测试目标增量重编完成，CTest 15/15 通过（4.28 秒），包含安装布局、节点退出和安装脚本语法检查。
- 本轮没有前端改动或生产部署；以上不是生产热更新或真实业务端到端证明。

## 2026-09-09 控制回复协议校验归属

- 欢迎消息、心跳确认和版本探测确认的纯字段校验归入 control_reply，ControlChannel 保留收发、调度与错误反馈。
- 新增欢迎字段缺失、错误请求 ID、错误回复类型及无效摘要回归；Linux 与 Windows 控制流专项均通过，两个平台节点重编通过。
- 协议模块目前通过 artifact.h 引入摘要格式检查，其 OpenSSL/Protobuf 编解码依赖仍待收窄。未部署。

## 2026-09-09 摘要格式依赖收窄

- isSha256Digest 独立到 node/proto/digest.h，仅依赖 string_view；控制回复、节点发布响应和代理协议格式校验直接包含该规则，不再为格式检查引入完整制品编解码实现。
- Linux 服务端与节点生产目标重编成功；此前相关 Linux 架构/发布测试、Windows 控制流测试通过。未部署。

## 2026-09-09 移除废弃发布响应解析

- 全部生产调用改用 release_metadata 中的版本规则与 ETag 构造；移除无生产调用方的 release_response、HTTP 响应头解析、响应头文件读取及仅覆盖旧路径的测试。
- 节点、服务端和测试代码未发现旧解析入口残留。Linux 节点与两项相关测试通过；Windows 节点、运行时和发布测试目标重编完成。
- 未变更现有控制协议更新路径，也未部署。

## 2026-09-09 节点安装文件操作分层

- 平台候选文件命名、清理与安装移入 binary_install，SelfUpdater 保留传输、完整性检查和升级编排。升级记录复用统一摘要规则。
- Windows 新增完整传输后 commit 拒绝回归：原文件不变、不请求重启、候选文件清除，旧二进制摘要消费待升级记录返回空并移除记录。运行时测试通过（9.63 秒）。
- Linux 节点和运行时目标重编通过，运行时测试通过（1.59 秒），覆盖临时二进制成功替换和升级记录读取。未部署或执行生产自升级。

## 2026-09-09 服务端发布制品分层

- artifact 只管理文件快照和范围读取；manifest 管理清单解析；release 组合并校验发布文件；catalog 管理发布快照缓存与刷新。生产调用方改为显式包含 catalog，不保留 artifact 聚合兼容层。
- 文件制品层移除原缓存调度等无关标准库引用。Linux 架构和发布测试通过（2/2）；Windows 发布测试通过（0.30 秒）。
- Linux 服务端生产目标重编成功；未部署。

## 2026-09-09 发布目录初始化原子性

- Catalog 配置操作由初始化锁串行化，全部路径写入后才发布 Release 快照；读取方通过 acquire 观察完整配置。
- 新增并发初始化用例：8 个线程同时进入，恰好 1 个成功、7 个明确拒绝；新增首次缺失文件失败后仍未发布、随后成功重试的用例。
- Linux 与 Windows 发布专项均通过。本轮未部署。

## 2026-09-09 发布共享能力与刷新快照边界

- 发布版本规则和 ETag 构造迁入 node/proto/release_metadata.h；文件 SHA-256 迁入 common/file_digest.h，节点与服务统一调用，删除旧运行时入口。
- 服务代码已无 node/runtime/ 头文件依赖。Windows 节点、运行时测试、发布测试重编通过；Linux 节点与两项专项验证通过。
- Catalog 获得独占刷新权后重新读取当前快照，避免使用调用方此前捕获的过期版本进行来源检查和退休保留；实际被替换的版本进入保留队列。
- Linux 发布专项通过。并发时序修正经代码路径审查，现有测试未确定性重现该线程交错；未部署或进行线上下载联调。
## 2026-09-09 文件响应生命周期审查

- 当前依赖的 Ruvia FileResponseOptions 仅接收路径和内容类型，HttpResponse 无公开快照保活入口；文件发送器在响应头发送后打开文件。
- 发布下载仍使用 c.file()。保留 Catalog 的退休快照队列，避免移除后使 /proc/self/fd 路径在异步响应阶段失效。服务未覆盖框架默认 60 秒写超时；当前保留窗口为两分钟。
- 后续若替换此机制，需要响应拥有文件资源的接口，并验证条件请求、范围请求及慢速下载；仅改为普通流式写入不能视为等价迁移。此次审查未修改下载行为。
## 2026-09-09 协议文本与凭据校验归属

- 发布版本校验改为显式 ASCII 字母、数字和 .-_+，保留 64 字节上限，避免 locale 改变协议允许的字符集合。Linux 与 Windows 发布测试覆盖全部字节及长度边界并通过。
- 节点凭据 ID、密钥格式规则迁入 node/proto/credential_validation.h，NodeCredentials 文件加载和服务 Authenticate 校验共同使用；删除凭据类的重复静态校验入口。
- 凭据类继续负责安全文件解析、值所有权与密钥清理；共享规则不依赖文件、OpenSSL 或 WebSocket。
## 2026-09-09 安装入口凭据契约

- install_node.sh 固定 LC_ALL=C，按字节验证密钥长度和非空白可打印 ASCII 范围，与共享凭据校验一致。
- 节点 ID、密钥校验均在 mktemp、凭据写入及下载之前完成。
- bash -n 通过；实际脚本配合 id/mktemp 测试替身执行 260 个用例，覆盖 255 种非 NUL 字节和 0/31/32/128/129 长度。有效输入在 mktemp 测试替身处终止，非法输入提前拒绝；未执行下载、安装或部署。NUL 无法通过进程参数传入。
## 2026-09-09 凭据重复字段解析

- NodeCredentials 独立记录 node_id 和 secret 是否出现，不再以字段值为空表示尚未读取；空值后重复同名字段也会被拒绝。
- 新增真实安全文件读取回归，覆盖两个字段各自的空值重复和非空值重复场景。有效凭据读取测试保留。
- 安装入口的 266 个字节、长度和 ID 用例已纳入 UNIX CTest，安装校验与语法测试通过；测试不执行安装。
## 2026-09-09 Agent 协议职责拆分

- agent_transport.h 负责 WebSocket 二进制消息到 ClientEnvelope 的解码；agent_protocol.h 仅保留协议字段校验；agent_report.mapper.h 负责 Heartbeat 到领域报告的转换，控制器显式组合三层。
- UUID 解析实现迁入 service/common/uuid.h；HTTP 参数处理复用它，Agent 校验无需引入 HTTP Context。函数行为保持不变，没有保留重复实现。
- Linux 架构专项通过，覆盖协议层不直接依赖 WebSocket/HTTP、不包含领域报告映射的约束。此项属于源代码架构检查，不能代替真实网络认证联调。
## 2026-09-09 独立协议测试与心跳有限值

- 新增 agent_protocol_test，仅链接 flexedge_node_protocol，验证认证字段组合、心跳字段边界和领域报告映射；Linux 和 Windows 编译、测试通过，证明该校验与映射模块不需要 Web 框架依赖。
- 回归先复现 load_1m 正无穷被接受的问题：后续 JSON 序列化将其转换为 null。协议边界现要求负载值为非负有限数。
- 正负无穷、NaN 拒绝及最大有限值接受测试在两个平台均通过。此验证不包含真实网络认证、持久化联调或部署。
## 2026-09-09 压缩策略校验边界

- CompiledConfig 的压缩大小限制、算法集合及匹配值校验迁入 compression_validation.h，通过 validateResponseCompression 调用；配置索引构建不再内置这些策略细节。
- 校验规则保持不变。现有运行时测试包含无效 MIME 类型经配置编译被拒绝，以及响应压缩行为验证；Linux 运行时专项与 Windows 节点、运行时测试构建通过。
- HTTP 字段名与压缩 MIME 类型采用不同语法，本轮未将二者合并成单一字符校验。未部署。
## 2026-09-09 编译配置快照所有权

- CompiledConfig 的网站、域名及路由索引指向自身持有的对象，默认复制会保留指向旧快照的索引。现明确禁止复制和移动，使用现有 shared_ptr<const CompiledConfig> 共享整个快照。
- 编译期断言覆盖复制/移动构造与赋值不可用，防止后续重新引入不安全值语义。
- 压缩匹配值校验改用 MimeType/Extension 枚举替代两个布尔开关；该接口改动已通过 Windows 节点构建和运行时回归。
## 2026-09-09 TLS 上下文地址稳定性

- TlsContextSet 的 SNI 回调参数保存 this，禁止复制和移动，避免 OpenSSL 回调持有旧对象地址；继续通过共享指针管理整个上下文集合。
- 编译期断言约束不可复制/移动；Linux 节点构建与运行时测试通过，Windows 节点与运行时测试构建通过。
- 相邻 DataPlane 的自定义析构及不可复制成员已阻止隐式对象迁移，本轮未对其新增重复约束。未部署。
## 2026-09-09 状态持久化重复副本清理

- StateStore 停止写入无人读取的 releases/<id>/manifest.pb，移除仅为该路径服务的标识符检查。恢复仍从 active/state.pb 读取状态，并按摘要加载 objects 下的对象。
- staged 状态、active 原子替换及 previous 备份行为保持原样；已有磁盘副本未删除。previous 当前不是自动恢复入口，不能将其存在视为自动回滚能力。
- Linux 暂存、激活和恢复回归通过，并验证新状态目录不再创建 releases 副本目录。未部署。
## 2026-09-09 日志 envelope 依赖收窄

- 日志 envelope 使用公共 UUID 解析和共享凭据 ID 规则，移除重复 Agent ID 校验及 HTTP 工具头依赖。
- 日志专项仅链接协议库，Linux 和 Windows 构建、测试均通过；此验证覆盖队列 envelope 编解码和字段校验，不代表 Redis 或真实节点日志联调。
- tail 模块仍负责 HTTP 查询参数与 SSE 请求头，因此保留 HTTP 依赖；其纯游标编解码可以作为后续独立边界。未部署。
## 2026-09-09 日志游标与 HTTP 适配边界

- TailCursor、游标解析和编码迁入 tail_cursor.h，只依赖标准库与 UUID 工具。tail.h 保留 HTTP 参数校验和 SSE 请求头适配。
- 节点日志和网站访问日志查询服务直接引用 tail_cursor.h；控制器与 SSE 发送层继续使用需要的 HTTP 适配。
- Linux、Windows 无 Web 框架依赖的日志专项均通过，新增覆盖游标往返、UUID 大小写规范化、零/负数/溢出时间戳、非法 UUID 和数字尾随字符。未部署。
## 2026-09-09 公共连接错误识别

- 公共 connection_error.h 统一识别断管、连接重置和连接中止错误码，并保留已有文本错误识别；连接日志与 SSE 共同复用。
- 删除节点控制器、同步事件控制器和日志 tail 中的重复函数。SSE 保留 Redis 取消处理，普通连接日志不将 Redis 取消归类为客户端断开。
- Linux 日志专项通过，覆盖类型化断连、SSE 取消及业务错误继续传播。未进行真实客户端断连联调或部署。
## 2026-09-09 证书覆盖规则与 SAN 来源

- 数据库 subject_alt_names 为生成列：普通域名包含自身，通配符包含通配符与根域。部署查询现在显式读取该 SAN 集合，与界面运行时判断采用同一来源。
- 单个通配符名称不再隐式匹配根域；根域覆盖由独立 SAN 提供。旧部署逻辑在当前生成列约束下能推断出同样结果，因此此次修正是消除跨层隐含规则，不能据此声称线上根域证书已发生故障。
- 新增真实下发选择函数测试，验证仅通配符、加入根域 SAN、深层子域和选择摘要。Linux 架构专项通过；测试复用预先存在的证书摘要，不覆盖密钥解密、真实签发或 TLS 握手。未部署。
## 2026-09-09 签发名称来源统一

- 证书更新接口只更新签发配置，不修改 domain，已签发材料不会因域名编辑而错配新的名称集合。
- work_loader 改为仅从 subject_alt_names 两项生成订单域名列表，移除同时读取主域名导致的重复名称；查询移除无用列，并同步核对所有后续列索引。
- 普通证书传递一个名称，通配符证书传递通配符与根域。此结论基于数据库生成列及加载路径，未执行真实 CA 订单。
## 2026-09-09 路由策略构建依赖

- RE2 从 flexedge_node_protocol 的 PUBLIC 依赖移至独立 flexedge_route_policy INTERFACE 目标；服务、节点及实际编译路由规则的测试显式引用该目标。
- Linux 全部目标构建通过，完整 CTest 17/17 通过。核查 Ninja 实际链接命令，Agent 协议和日志 envelope 专项均不包含 RE2 或 Ruvia。
- Windows 节点及相关测试目标构建通过；节点运行时、配置激活、Agent 协议和日志摄取 CTest 4/4 通过。
- 此验证为本地构建与测试，不包含线上部署或实际业务联调。

## 2026-09-09 控制通道配置职责

- 控制通道地址解析、WebSocket 子协议及超时配置迁入 node/control/transport_config.h；启动入口只调用配置工厂，不再持有传输协议细节。
- 显式声明版本工具依赖，保留现有配置行为。新增默认 WSS、IPv6、端口与路径映射，以及非法地址拒绝回归。
- Linux 节点与控制流专项构建通过，控制流测试通过；未进行本次 Windows 验证或部署。

## 2026-09-09 控制地址校验边界

- 配置阶段明确拒绝非 ws/wss 协议和冒号后的空端口，避免误映射为默认 WSS 配置或默认端口。
- 同时拒绝原始空白/控制字符、反斜杠、未加括号的 IPv6 及多余方括号；新增对应地址回归用例。
- Linux 与 Windows 节点、控制流测试目标构建通过，两端控制流测试均通过。此校验不涉及 DNS 可达性或真实服务端连接，未部署。

## 2026-09-09 节点构建版本依赖

- 新增 flexedge_node_version 接口目标，节点与消费传输配置的测试显式依赖它，从 PROJECT_VERSION 获取同一版本宏。
- 删除 version.h 的 0.3.20 默认值，未声明版本依赖时编译报错；移除服务端未使用的节点版本宏。
- Linux、Windows 节点及控制流测试构建通过，两端控制流测试通过。未部署。
- 后续 Linux 全量构建通过，覆盖服务端移除版本宏后的编译；完整 CTest 17/17 通过，包括安装布局和本地节点退出测试。未进行真实后端联调。

## 2026-09-09 控制地址 IPv6 校验

- 方括号内主机交由 Asio IPv6 解析器校验，拒绝被括号包裹的域名、IPv4 和非法 IPv6，避免去掉括号后改变地址含义。
- Linux 节点及控制流测试目标构建通过，控制流测试通过；未进行本次 Windows 验证或部署。

## 2026-09-09 安装 origin 输入边界

- 安装脚本在文件分配及下载前校验服务器 origin，仅允许 HTTP(S) 协议、主机和可选有效端口，规范化一个尾部斜杠；拒绝路径、凭据、空白和 systemd 展开字符。
- 该检查保护写入 ExecStart 的参数边界；IPv6 的完整语义仍由节点地址解析器校验。
- 真实安装脚本输入测试在首次 mktemp 前停止，不修改系统；Linux 输入验证及语法测试 2/2 通过。此前 IPv6 改动的 Windows 节点构建及控制流测试也已通过。未部署。

## 2026-09-09 安装暂存资源生命周期

- 将三次独立临时文件分配改为一个私有暂存目录，分配成功后立即注册 EXIT 清理，下载、响应头和凭据统一归该目录管理。
- 消除后续临时文件分配失败时尚未注册清理的窗口。新增下载失败回归，运行真实脚本写入测试凭据后由替身 curl 返回失败，确认暂存目录已删除。
- Linux 安装验证及语法测试 2/2 通过；未访问真实网络、修改系统安装或部署。
- 后续增加八组产物校验用例：合法产物到达安装关卡，缺失/非法/错误摘要及非法版本提前退出；每组均验证暂存清理。替身 install 在首条安装命令退出，Linux 安装测试 2/2 通过，不代表真实安装成功。

## 2026-09-09 安装响应头完整解析

- 回归复现版本值包含额外单词时被截断并接受的问题；响应头改为读取冒号后的完整值，只去除首尾空格、制表符及行尾 CR。
- 新增版本/摘要尾随内容拒绝及首尾空白接受测试。Linux 安装输入验证与语法测试 2/2 通过；使用替身下载和安装命令，未部署。

## 2026-09-09 安装与自更新交界审查（待修复）

- 安装脚本在最终 systemctl restart 之前清理旧状态、写入凭据并覆盖节点文件，没有先停止已有服务。节点自身仍可通过 SelfUpdater 提交候选二进制，并通过 StateStore 写入同一状态目录。
- 源码表明两条写入路径缺少协调；尚未执行并发复现，不能据此声称已发生线上数据损坏。
- 现有安装输入/产物测试在首次 install 命令停止，不覆盖状态迁移、二进制替换及服务恢复。后续修复需覆盖停止旧服务、写入失败的恢复、并发安装互斥及新服务启动失败路径，使用隔离目录和替身服务管理器验证。

### 第一阶段：写入互斥与停止关卡

- 安装通过 flock 排斥并发实例，读取已有 unit 状态并停止旧节点后才修改业务文件；停止失败直接退出。
- 隔离目录中运行脚本，验证停止失败保留旧二进制、凭据和状态，以及锁占用时拒绝另一实例；两个路径均清理暂存文件。Linux 安装测试 2/2 通过。
- 写入失败及启动失败后的恢复尚未实现，本阶段不能视为完整安装事务。未执行真实 systemd 操作或部署。

### 第二阶段：失败恢复

- 停止旧服务后完整备份二进制、凭据、状态及 unit 等受影响文件，再开始修改；失败时停止新服务、恢复文件并启动原先运行中的服务。
- 新服务启动且状态检查通过后再 enable；失败恢复若无法完成，保留暂存备份并输出路径。
- 隔离目录与替身 systemctl 验证凭据写入失败、服务重启失败后旧文件/状态/unit 恢复，并调用旧服务启动。Linux 安装测试 2/2 通过。新装、enable 失败及恢复失败路径仍需专项验证，未部署。
- 首次安装写入失败回归先复现恢复被不存在的服务阻断，再通过 start_attempted 区分是否需要停止新进程；验证清除已写入文件、状态及暂存目录，并保留原始写入错误码。Linux 安装测试 2/2 通过。
- enable 失败回归验证旧文件恢复并执行 disable；恢复停止操作失败时，验证旧二进制备份保留且错误输出包含恢复目录。Linux 安装测试 2/2 通过，服务管理仍使用替身。
- 隔离的新装成功回归验证二进制、凭据内容，0755/0600/0700 权限，unit 启动命令，先 restart 再 enable 的顺序及暂存清理。Linux 安装测试 2/2 通过；未启动真实节点或 systemd 服务。
- 新装 enable 失败场景验证撤销启用必须在移除新 unit 之前；调整恢复顺序后保留原始错误码并清理新装资源。替身服务管理器回归先失败再通过，Linux 安装测试 2/2 通过。

## 2026-09-09 摘要编码公共边界

- 文件摘要与协议产物摘要复用 common/hex.h 的 span 十六进制编码，移除重复实现；编码工具只依赖标准库，哈希计算和错误处理仍归各自模块。
- Linux 节点发布和架构测试构建、运行通过；未部署。
- Windows 节点发布与架构测试也通过。随后令牌和密码工具中的重复编码实现一并迁移，全仓只保留公共 hexEncode；Linux 服务端及架构测试重建通过，架构测试通过。密码派生算法及敏感缓冲区清理未改动，未部署。
- 令牌与密码行为回归迁入独立 credential_crypto 测试目标，仅链接 OpenSSL Crypto。Windows、Linux 独立测试通过，Linux 迁移后的架构测试也通过。
- 后续 Linux 全量构建完成，完整 CTest 18/18 通过（6.76 秒），覆盖本阶段公共编码、独立密码测试、安装恢复回归及安装布局；不代表真实 systemd 安装或线上联调。

## 2026-09-09 会话时长解析分层

- 纯时长解析迁入 service/config/duration.h，会话工具保留环境读取与 Cookie 适配。独立 duration 测试无需链接框架，覆盖秒/分/时/天、非法输入和溢出。
- Windows、Linux 独立测试均通过，Linux 服务端构建通过。未进行真实会话联调或部署。

## 2026-09-09 会话凭据格式与 HTTP 适配

- SessionCredential 及其编码、解析迁入认证领域头文件，Cookie 适配层只调用格式转换并读写请求/响应。
- 保留原有首个点分隔规则和敏感字符串管理。独立测试覆盖格式往返、秘密值包含点和缺失字段；Windows、Linux 测试通过，Linux 服务端构建通过。未进行真实登录联调或部署。

## 2026-09-09 会话身份读取职责

- 会话有效性查询、管理员/租户选择和令牌摘要校验迁入认证领域读取服务；身份类型也由认证领域持有。
- 认证中间件负责 Cookie、401 处理和请求身份绑定，移除 SQL 与摘要校验。查询条件、租户排序和失效响应保持原样。
- Linux 服务端构建通过；本轮未进行真实数据库会话或登录联调，未部署。
- 身份读取进一步改为按值接收 DbHandle，由中间件提供数据库能力，删除对 HTTP Context 的直接依赖；Linux 服务端重建通过，未进行数据库联调。
- 认证中间件复用认证领域的 UNAUTHORIZED 与 SESSION_INVALID 定义，删除重复错误码/文案/状态组合；Linux 服务端构建通过。查询异常仍直接传播，不转换为会话失效。

## 2026-09-09 安全状态临时文件清理

- 安全状态写入显式检查 close 结果，成功打开的临时文件由作用域清理器管理，写入或替换失败后尝试删除临时文件。
- Linux 配置激活回归通过，新增不可替换目录场景验证原目录保留且 .tmp 删除。未验证断电持久性或部署。
- 替换成功后解除临时路径清理所有权，避免再操作已移交的路径；Windows 配置激活构建及回归通过。
- 读取改为分块读取并明确检查正常 EOF，读取错误不返回部分数据。Windows 回归覆盖读取目录失败及跨块二进制内容（含 NUL）往返，配置激活测试通过。
- 后续 Linux 节点与配置激活测试重建通过，配置激活回归通过，补齐上述读写与临时路径所有权变更的 Linux 验证。
- 安全文件回归已迁入独立 secure_file 目标，不链接网络运行时或协议库。Windows、Linux 独立测试通过，Linux 迁移后的配置激活测试也通过。

## 2026-09-09 升级记录解析职责

- 升级记录格式解析提取为 parseUpgradeRecord，文件读取/消费和当前二进制摘要匹配保留在 takePendingUpgradeRecord。
- Windows、Linux 节点运行时回归通过，Linux 节点构建通过。未执行真实节点升级或部署。

## 2026-09-09 并发状态写入暂存隔离

- 每次写入使用包含进程、时钟和序号的独立临时路径，并以 noreplace 排他创建，避免写入者共享并截断固定 .tmp。
- Windows 八线程回归验证至少一次成功写入、最终内容完整及所有临时文件清理。系统可能拒绝竞争中的替换，调用者仍需处理写入异常；不承诺全部并发写入成功。未部署。
- Linux 节点及独立文件测试构建通过，并发文件回归通过，验证 noreplace 排他创建在当前 Linux 工具链可用。
- 后续 Linux 全量构建通过，覆盖全部安全文件调用方；完整 CTest 20/20 通过。未执行真实部署。

## 2026-09-09 SHA-256 文本校验归属

- 摘要文本校验迁入 common/sha256.h，服务端与节点统一使用公共命名空间，旧 node/proto/digest.h 已删除。
- Linux 全量构建通过，完整 CTest 20/20 通过。未进行远端 CI 或部署。
- Windows 节点、Agent 协议及节点发布目标构建通过，协议与发布测试 2/2 通过。

## 2026-09-09 Agent 失败目标存储边界

- 失败目标更新集中到 node_dispatch/target.store.h，Agent 命令服务传入已校验且截断的失败详情，保留原事务和集群范围约束。
- Windows Release 目标存储测试在本地隔离 PostgreSQL 中通过：校验租户、发布、节点和集群边界、终态保护及失败详情字段。
- Windows Release 服务端构建在 auth_session.service.h 的协程出现 MSVC C4737，尚未通过；这次数据库测试不代表服务端 Release 构建或真实 Agent 联调通过。未部署。
- Windows Debug 服务端构建通过。
- 后续修复：会话创建和刷新先读取命名配置快照，再调用异步存储；避免在 co_await 参数中内联配置解析。MSVC Release 服务端重新构建通过，未关闭优化或屏蔽诊断。
- 使用此次 Release 程序在隔离 PostgreSQL、Redis 环境执行 auth_http_test.py 通过，覆盖登录、Cookie、刷新轮换、并发重放、退出和登录锁定；测试进程与数据库均已停止。未部署。

## 2026-09-09 Agent 存储迁移 Linux 验证

- 当前源码在 WSL Arch 的 Release 配置下全量构建通过，涵盖服务端、节点及新增存储测试。
- 完整 CTest 实际执行 27 项并全部通过；10 项 PostgreSQL 测试因未提供本机数据库环境跳过，不能记为 37 项全部执行通过。Windows 隔离数据库测试结果见前述记录。
- 心跳存储测试夹具存在 GCC 未显式初始化部分字段的警告，不影响本次构建；未执行 Linux 数据库集成或部署。

## 2026-09-09 Agent 发布集群边界

- 期望状态与摘要查询同时校验发布和节点集群，对象查询同步校验节点当前集群；解密后的清单必须属于认证主体的集群。
- Windows Release 服务端构建通过。tests/agent_release_sql_test.py 从服务源码提取三个实际查询，在隔离 PostgreSQL 临时表中验证合法关联返回数据，跨集群发布、迁移后的旧主体以及删除节点无数据，事务回滚。
- 该回归只执行实际 SQL，不经过 C++ 服务调用或 protobuf 清单校验；清单新增检查仅完成编译验证，后续需补协议层回归。未部署。

## 2026-09-09 发布清单校验职责

- release_manifest.h 负责 protobuf 解析、摘要一致性与发布/集群身份校验，返回 optional 清单；命令服务负责解密、应用错误映射和事务编排。
- Windows 与 Linux 的 agent_protocol 测试通过，覆盖合法内容、错误摘要、错误发布/集群、无效 protobuf 和摘要未更新的内容篡改。测试目标显式链接 OpenSSL Crypto。
- Windows Release 服务端构建通过。本次直接调用生产解析函数，未执行真实 Agent 网络交互或部署。

## 2026-09-09 Agent 期望摘要读取边界

- desired_summary.store.h 返回 optional<DesiredSummary>，仅接受请求数据库句柄或现有事务；HTTP 错误映射保留在 AgentReadService。
- 直接调用存储函数的 Windows PostgreSQL 测试通过：合法摘要字段、租户/节点/集群/Agent 身份不匹配、跨集群发布、删除和待注册状态。已有实际 SQL 回归同步调整路径后通过。
- Windows Release 服务端构建通过，未执行本次 Linux 构建或真实 Agent 网络联调，未部署。

## 2026-09-09 Agent 期望配置快照存储

- desired_state.store.h 返回带行锁读取的 DesiredStateRecord，命令服务负责协议构造、解密校验及事务提交；摘要写入返回是否更新成功，失败按 revision 错误处理。
- Windows PostgreSQL 直接调用生产函数回归通过：快照字段、身份/集群/注册/删除边界、摘要成功写入，以及旧 revision、错误租户/节点写入失败且整行不变。源码提取 SQL 回归也通过。
- Windows Release 服务端构建通过。本轮未验证并发连接上的锁等待行为，也未执行 Linux 构建或真实 Agent 网络联调。未部署。

## 2026-09-09 阶段发布与实际节点验收

当前工作区服务端、前端与两个实际节点已发布。最终摘要、数据库备份、TLS 转发与访问日志证据、主动退出修复、统一用户图标及未覆盖范围见 [阶段发布记录](release-20260909.md)。

## 2026-09-09 集群信息卡片紧凑化

四张信息卡片改为单行标签和值，内容高度 44px，减少间距并移除图标、阴影和多余留白。前端检查、147 项测试和构建通过；线上桌面与窄屏明暗主题检查通过。公网首页哈希 1f4226fb1eb0ff24d9eaeedbdea2efc588944d6c32a269ecd90b95969623bc7d；回滚页面位于 /opt/flexedge/backups/final-avatar-20260909-101155/index.html。

## 2026-09-09 信息行与加载、日志阅读稳定性

- 根据后续要求，集群摘要移除卡片和主机前缀展示，托管域名、接入域名、节点状态在桌面排列为一行，窄屏允许自然换行。
- 资源表、集群导航、任务中心与详情、网站看板、历史访问日志统一使用 150ms 延迟与 300ms 最短骨架显示时间；等待阶段保留占位，已有数据的刷新不替换整块骨架。网站看板仅在打开时开始计时，骨架与统计内容互斥。
- 日志详情改为紧凑的详情/收起按钮。展开实时日志详情时冻结可见列表，SSE 继续接收；全部收起后恢复最新列表。空详情字段保留一行文字加原内边距、边框的高度。
- 静态验证：lint、typecheck、152 项测试及生产构建通过。300ms 边界由虚拟定时器测试验证。代码复核发现的首批数据提前移除骨架、未打开即计时和骨架正文重叠问题均已修复。
- 真实后端浏览器验证：测试请求 `log-details-20260909-a` 展开期间，新增 `log-details-20260909-b` 后总数由 100 变为 101，可见列表保持原记录；收起后显示两条匹配记录。历史详情按钮状态、桌面和窄屏布局、明暗主题及空字段高度均已检查。本轮未做网络故障注入或慢网浏览器计时，不将定时器测试视为真实慢网验证。
- 仅发布前端，公网首页 SHA-256 为 `e5a642392737e394b024dfa4b742f7439645c69625103fc85e39f819ff68d988`，健康检查正常。最近一次回滚页面：`/opt/flexedge/backups/final-avatar-20260909-102656/index.html`；本轮首次部署前页面：`/opt/flexedge/backups/final-avatar-20260909-102429/index.html`。

## 2026-09-09 统一分页大小

- 所有资源页与历史日志复用页大小选择器，选项为 100、500、1000，默认 100；切换页大小回到第一页。任务中心六条摘要不属于分页页面，保持原样。
- 后端公共分页默认改为 100，上限改为 1000；历史日志移除原 50 条默认覆盖，继续保留正整数与偏移乘法溢出校验。
- lint、typecheck、153 项前端测试与生产构建通过，Linux Release 服务端构建通过。查询测试断言三种大小保存在不同缓存条目中。
- 线上历史日志共 18578 条：100 条时 186 页，从第二页切换 500 条后回到第一页、共 38 页；1000 条时共 19 页。网站列表 1000 条选项可正常返回。桌面与 390px 窄屏分页控件检查通过。本轮未进行千条详情同时展开的性能压测。
- 服务端 SHA-256 `5599f15811e9cc3f3efcba104091b962d6e06ca5a7a9f357d7640c319e107076`；前端 SHA-256 `b2be5aa65c91a63ba5c9e0acb81701b9ea2e4f73f3cb0864bd4b139f05782082`。健康检查正常，回滚产物和数据库备份在 `/opt/flexedge/backups/pagination-server-20260909-103552`，节点产物未更换。

## 2026-09-09 网站详情 AS 提示与来源显示

- 本地五字段 XDB 解析保留 ASN 和完整 AS 名称。客户端 IP 的流量与请求数排行使用专用 DTO，输出 `asn`、`as_name`；悬浮或键盘聚焦时显示完整名称，缺失时明确提示暂无数据。
- 国家未知但 AS 有效时保留 AS 元数据，地理显示未知地区；国家排行继续排除无国家记录。旧格式地理显示保持原行为，五字段 AS 信息不再混入地理名称。
- 网站详情提示区域使用普通箭头光标；窄屏排行网格明确使用可收缩单列，避免 IP 标签挤出统计数值。请求来源中的空值按要求显示 `/`，保留其他来源原文，不再生成“直接访问”名称。
- 静态与本地验证：154 项前端测试、lint、typecheck、生产构建通过；Linux Release 服务端构建通过；GeoIP 解析测试以及使用真实 CLI 的 IPv4/IPv6 合成 XDB 范围、无效输入测试通过。
- 真实浏览器验证：两个 IP 排行均显示实际 `AS4134 / Chinanet`、`AS37963 / Hangzhou Alibaba Advertising Co.,Ltd.`，键盘聚焦可打开提示；390px 窄屏长标签缩略且数值可见。DOM 计算样式确认 IP 光标为 `default`。最新来源排行实际显示 `/` 及 58 次请求，没有“直接访问”。实际数据来自生产 SSE，未使用 mock 冒充。
- 最终服务端 SHA-256 `65cca27df7fcec5ebfd4621ce5a50ab1a47743a6eb4033fa17ca0f4f1109101a`，最终前端 SHA-256 `4a338b2a7d5f65714ddf8079c9eff90584c942d12d7859a7cb3d12c15bfbb5dc`，公网首页哈希一致、健康检查正常。最近的服务端与数据库备份位于 `/opt/flexedge/backups/referer-server-20260909-105350`；本功能首次发布前备份在 `/opt/flexedge/backups/as-tooltip-server-20260909-104618`。
