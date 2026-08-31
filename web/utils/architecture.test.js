import { describe, expect, test } from 'bun:test';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const webRoot = resolve(import.meta.dir, '..');

function source(path) {
    return readFileSync(join(webRoot, path), 'utf8');
}

function moduleReferences(content, pattern) {
    return [...content.matchAll(/(?:from\s+|export\s+\*?\s*from\s+)["']([^"']+)["']/g)]
        .map((match) => match[1])
        .filter((module) => pattern.test(module));
}

function sourceFiles(directory) {
    return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
        const path = join(directory, entry.name);
        if (entry.isDirectory()) return sourceFiles(path);
        return entry.isFile() && /\.[jt]sx?$/.test(entry.name) ? [path] : [];
    });
}

describe('frontend architecture boundaries', () => {
    test('frontend uses Ant Design 6 as its component system', () => {
        const antDesignImports = sourceFiles(webRoot).flatMap((path) =>
            moduleReferences(
                readFileSync(path, 'utf8'),
                /^(?:antd(?:\/|$)|@ant-design(?:\/|$))/
            ).map((module) => `${path}: ${module}`)
        );
        const packageJson = JSON.parse(readFileSync(join(webRoot, '..', 'package.json'), 'utf8'));
        expect(packageJson.dependencies.antd).toMatch(/^\^6\./);
        expect(packageJson.dependencies['@ant-design/icons']).toBeDefined();
        expect(packageJson.dependencies['@ant-design/cssinjs']).toBeDefined();
        expect(packageJson.dependencies['radix-ui']).toBeUndefined();
        expect(packageJson.dependencies.shadcn).toBeUndefined();
        expect(antDesignImports.length).toBeGreaterThan(0);
        expect(moduleReferences(source('main.tsx'), /^@ant-design\/cssinjs$/)).toEqual([
            '@ant-design/cssinjs',
        ]);
        expect(source('main.tsx')).toContain('<StyleProvider layer>');
        expect(source('main.tsx')).toContain("cssVar: { prefix: 'ant', key: 'flexedge' }");
        expect(source('main.tsx')).not.toContain('colorPrimary:');
        expect(source('main.tsx')).not.toContain('borderRadius:');
        expect(source('main.tsx')).not.toContain('fontFamily:');
        expect(packageJson.dependencies['@fontsource-variable/inter']).toBeUndefined();
        expect(source('styles/index.css')).not.toContain('@fontsource');
        expect(source('styles/index.css')).not.toContain('--app-chart-');
        expect(source('styles/index.css')).toContain(
            'border-color: var(--ant-color-border-secondary);'
        );
        expect(source('styles/index.css')).toContain('background: transparent;');
        expect(existsSync(join(webRoot, 'components', 'ui', 'button.tsx'))).toBe(true);
        expect(existsSync(join(webRoot, 'components', 'ui', 'sheet.tsx'))).toBe(true);
        expect(existsSync(join(webRoot, 'components', 'ui', 'table.tsx'))).toBe(false);
        expect(moduleReferences(source('components/pagination_bar.tsx'), /^antd$/)).toEqual([
            'antd',
        ]);
    });

    test('data tables fill available width and pin actions only while overflowing', () => {
        const dataTable = source('components/data_table.tsx');

        expect(dataTable).toContain("scroll={{ x: 'max-content', y: '100%' }}");
        expect(dataTable).toContain("width: column.key === 'actions' ? 72");
        expect(dataTable).toContain('headerRow.scrollWidth > container.clientWidth + 1');
        expect(dataTable).toContain('onHeaderRow={captureHeaderRow}');
        expect(dataTable).not.toContain("querySelector('.ant-");
        expect(dataTable).toContain("column.key === 'actions' &&");
        expect(dataTable).toContain("fixed: column.key === 'actions' && hasHorizontalOverflow");
        expect(dataTable).toContain("import { Empty, Table } from 'antd'");

        const actionColumnViolations = sourceFiles(join(webRoot, 'pages')).flatMap((path) => {
            const content = readFileSync(path, 'utf8');
            return content
                .split(/key:\s*['"]actions['"]/)
                .slice(1)
                .filter((section) => !section.slice(0, 1200).includes('<DropdownMenu>'))
                .map(() => path);
        });
        expect(actionColumnViolations).toEqual([]);
    });

    test('business drawers can override shared Sheet width defaults', () => {
        const sheet = source('components/ui/sheet.tsx');

        expect(sheet).toContain("side === 'right'");
        expect(sheet).toContain("import { Drawer } from 'antd'");
        expect(sheet).toContain("width={horizontal ? 'min(860px, 92vw)'");
    });

    test('HTTP infrastructure does not depend on business pages or stores', () => {
        const http = source('utils/http.ts');
        expect(moduleReferences(http, /^@\/(?:pages|store)\//)).toEqual([]);
        expect(http).toContain('postOperation');
        expect(http).toContain('putOperation');
        expect(http).toContain('deleteOperation');
        expect(http).not.toContain('patch<T');
        expect(source('utils/api.response.ts')).toContain('服务器响应格式不正确');
        expect(source('pages/certificate/certificate.api.ts')).toContain('request.getRaw<Blob>');
        const ambiguousOperations = sourceFiles(webRoot)
            .filter((path) => path.endsWith('.api.ts'))
            .filter((path) =>
                /request\.(?:post|put|delete)<void>/.test(readFileSync(path, 'utf8'))
            );
        expect(ambiguousOperations).toEqual([]);
        const manualQueryStrings = sourceFiles(webRoot)
            .filter((path) => path.endsWith('.api.ts'))
            .filter((path) => /\?[^`]*=\$\{/.test(readFileSync(path, 'utf8')));
        expect(manualQueryStrings).toEqual([]);
    });

    test('shared components do not depend on business pages', () => {
        const imports = readdirSync(join(webRoot, 'components'), { withFileTypes: true })
            .filter((entry) => entry.isFile() && /\.[jt]sx?$/.test(entry.name))
            .flatMap((entry) =>
                moduleReferences(source(`components/${entry.name}`), /^@\/pages\//).map(
                    (module) => `components/${entry.name}: ${module}`
                )
            );
        expect(imports).toEqual([]);
        expect(existsSync(join(webRoot, 'components', 'DnsLineTreeSelect.tsx'))).toBe(false);
    });

    test('cross-page auth is a feature and its state has no API dependencies', () => {
        expect(existsSync(join(webRoot, 'pages', 'auth', 'auth.store.ts'))).toBe(false);
        expect(moduleReferences(source('features/auth/auth.store.ts'), /\.api$/)).toEqual([]);
        const violations = readdirSync(join(webRoot, 'features', 'auth'), { withFileTypes: true })
            .filter((entry) => entry.isFile() && /\.[jt]sx?$/.test(entry.name))
            .flatMap((entry) =>
                moduleReferences(source(`features/auth/${entry.name}`), /^@\/pages\//).map(
                    (module) => `features/auth/${entry.name}: ${module}`
                )
            );
        expect(violations).toEqual([]);
        expect(source('features/auth/auth.types.ts')).not.toContain('is_administrator');
        expect(source('features/auth/auth.types.ts')).not.toContain('permissions');
        expect(existsSync(join(webRoot, 'hooks', 'usePermission.ts'))).toBe(false);
        expect(existsSync(join(webRoot, 'types', 'permission.ts'))).toBe(false);
        expect(source('features/auth/auth.types.ts')).not.toContain('export interface JwtPayload');
        expect(source('features/auth/auth.service.ts')).toContain('function sameUser(');
        expect(existsSync(join(webRoot, 'utils', 'equality.ts'))).toBe(false);
    });

    test('shared query keys expose only capabilities used across business modules', () => {
        const queryKeys = source('utils/query.keys.ts');
        expect(queryKeys).not.toContain('trees:');
        expect(queryKeys).not.toContain('tree:');
        expect(queryKeys).not.toContain('options:');
        expect(queryKeys).not.toContain('details:');
        expect(source('pages/dns_zone/dns_zone.types.ts')).toContain('options: (query:');
    });

    test('page entries use services or selectors instead of APIs directly', () => {
        const imports = readdirSync(join(webRoot, 'pages'), { withFileTypes: true })
            .filter((entry) => entry.isDirectory())
            .flatMap((entry) => {
                const path = `pages/${entry.name}/index.tsx`;
                try {
                    return moduleReferences(source(path), /\.api$/).map(
                        (module) => `${path}: ${module}`
                    );
                } catch {
                    return [];
                }
            });
        expect(imports).toEqual([]);
    });

    test('provider transport is owned only by the provider module', () => {
        expect(existsSync(join(webRoot, 'pages', 'dns_provider'))).toBe(false);
        expect(source('routes/index.tsx')).toContain('@/pages/provider/dns_provider_page');
        const violations = readdirSync(join(webRoot, 'pages'), { withFileTypes: true })
            .filter((entry) => entry.isDirectory() && entry.name !== 'provider')
            .flatMap((entry) =>
                readdirSync(join(webRoot, 'pages', entry.name), { withFileTypes: true })
                    .filter((file) => file.isFile() && file.name.endsWith('.api.ts'))
                    .map((file) => `pages/${entry.name}/${file.name}`)
                    .filter((path) => source(path).includes('/api/providers'))
            );
        expect(violations).toEqual([]);
    });

    test('provider management UI and validation are owned by the provider module', () => {
        const certificatePage = source('pages/certificate/index.tsx');
        const certificateProviderManager = source(
            'pages/provider/certificate_provider_manager.tsx'
        );
        expect(certificatePage).toContain('<CertificateProviderManager');
        expect(certificatePage).not.toContain('useCertificateProviderSave');
        expect(certificatePage).not.toContain('useCertificateProviderVerify');
        expect(certificatePage).not.toContain('useCertificateProviderDelete');
        expect(certificatePage).not.toContain('providerForm');
        expect(source('pages/certificate/certificate.schema.ts')).not.toContain(
            'accountEmailRules'
        );
        expect(certificateProviderManager).toContain("from './certificate_provider.schema'");
        expect(certificateProviderManager).toContain('useCertificateProviderSave');
        expect(certificateProviderManager).toContain('useCertificateProviderVerify');
        expect(certificateProviderManager).toContain('useCertificateProviderDelete');
    });

    test('business page modules form an acyclic dependency graph', () => {
        const modules = readdirSync(join(webRoot, 'pages'), { withFileTypes: true })
            .filter((entry) => entry.isDirectory())
            .map((entry) => entry.name);
        const dependencies = new Map(
            modules.map((module) => [
                module,
                new Set(
                    sourceFiles(join(webRoot, 'pages', module)).flatMap((path) =>
                        moduleReferences(readFileSync(path, 'utf8'), /^@\/pages\/([^/]+)/).map(
                            (reference) => reference.split('/')[2]
                        )
                    )
                ),
            ])
        );
        const visited = new Set();
        const active = new Set();
        const cycles = [];
        function visit(module, path) {
            if (active.has(module)) {
                cycles.push([...path, module].join(' -> '));
                return;
            }
            if (visited.has(module)) return;
            active.add(module);
            for (const dependency of dependencies.get(module) ?? [])
                if (dependencies.has(dependency)) visit(dependency, [...path, module]);
            active.delete(module);
            visited.add(module);
        }
        for (const module of modules) visit(module, []);
        expect(cycles).toEqual([]);
    });

    test('DNS Zone uses one aggregate name and transport owner', () => {
        expect(existsSync(join(webRoot, 'pages', 'domain'))).toBe(false);
        expect(source('pages/dns_zone/dns_zone.api.ts')).toContain('/api/dns-zones');
        expect(source('pages/dns_zone/dns_zone.api.ts')).toContain('/options');
        expect(source('routes/index.tsx')).toContain('path="dns-zones"');
        expect(source('routes/index.tsx')).not.toContain('path="domains"');
    });

    test('DNS Zone embedded records use one aggregate save contract', () => {
        const dnsZoneApi = source('pages/dns_zone/dns_zone.api.ts');
        const dnsZoneService = source('pages/dns_zone/dns_zone.service.ts');
        const dnsZoneTypes = source('pages/dns_zone/dns_zone.types.ts');
        const dnsZonePage = source('pages/dns_zone/index.tsx');
        const recordView = dnsZoneTypes.slice(
            dnsZoneTypes.indexOf('export interface DnsRecordItem'),
            dnsZoneTypes.indexOf('export type DnsRecordQuery')
        );
        expect(dnsZoneApi).toContain(
            'export function saveDnsZone(target: RevisionedResourceRef, config: DnsZoneConfig)'
        );
        expect(dnsZoneApi).not.toContain('transform:');
        expect(dnsZoneApi).not.toMatch(/(?:create|update|remove)DnsRecord/);
        expect(dnsZoneService).toContain('export function useDnsZoneSave()');
        expect(dnsZoneService).not.toMatch(/useDnsRecord(?:Save|Delete)/);
        expect(dnsZoneTypes).not.toContain('SaveDnsRecordDto');
        expect(dnsZoneTypes).not.toContain('RevisionedConfigSnapshot');
        expect(recordView).not.toMatch(/revision|created_at|modified_at/);
        expect(dnsZonePage).toContain('{ id: createUuid(), ...record }');
        expect(dnsZonePage).toContain('config: { records }');
        expect(dnsZonePage).toContain('config: selectedDnsZone.config');
        expect(dnsZonePage).toContain('recordEditor.config.records.map');
        expect(dnsZonePage).toContain('revision: recordEditor.revision');
        expect(dnsZonePage).not.toContain("dataIndex: 'modified_at'");
    });

    test('certificate config and renewal use one revision-aware aggregate contract', () => {
        const certificateApi = source('pages/certificate/certificate.api.ts');
        const certificateService = source('pages/certificate/certificate.service.ts');
        const certificateTypes = source('pages/certificate/certificate.types.ts');
        const certificatePage = source('pages/certificate/index.tsx');
        expect(certificateApi).toContain(
            'export function saveCertificate(target: RevisionedResourceRef, config: CertificateConfig)'
        );
        expect(certificateApi).toContain(
            'export function renewCertificate(target: RevisionedResourceRef)'
        );
        expect(certificateApi).toContain('withExpectedRevision(target.revision)');
        expect(certificateApi).not.toContain('RevisionedConfigSnapshot');
        expect(certificateApi).not.toContain('{ config: {');
        expect(certificateService).toContain('export function useCertificateSave()');
        expect(certificateService).not.toContain('useCertificateUpdate');
        expect(certificateTypes).toContain('export interface CertificateCreateInput');
        expect(certificateTypes).not.toMatch(/CreateCertificateDto|UpdateCertificate/);
        expect(certificatePage).toContain('config: { auto_renew: data.auto_renew }');
        expect(certificatePage).toContain('checked={item.config.auto_renew}');
        expect(certificatePage).not.toContain('item.auto_renew');
    });

    test('search selectors do not treat a fixed first page as the complete option set', () => {
        const clusterSelect = source('pages/cluster/components/cluster_select.tsx');
        const dnsZoneSelect = source('pages/dns_zone/components/dns_zone_select.tsx');
        const certificatePage = source('pages/certificate/index.tsx');
        const clusterPage = source('pages/cluster/index.tsx');
        const websitePage = source('pages/website/index.tsx');
        expect(clusterSelect).toContain('keyword: keyword || undefined');
        expect(dnsZoneSelect).toContain('useDnsZoneOptions');
        expect(dnsZoneSelect).toContain('available: requireAvailable ? true : undefined');
        expect(certificatePage).toContain('<DnsZoneSelect');
        expect(certificatePage).toContain('requireAvailable');
        expect(clusterPage).toContain('<DnsZoneSelect');
        expect(clusterPage).toContain(
            'hostnamePrefix?.trim() && selectedZone && selectedZone.id === selectedZoneId'
        );
        expect(`${clusterSelect}\n${certificatePage}\n${clusterPage}`).not.toContain(
            'pageSize: 100'
        );
        expect(websitePage).not.toContain('{ page: 1, pageSize: 100, usable');
    });

    test('administrator identity changes reset query and page-local state at one shared boundary', () => {
        const authSession = source('features/auth/auth.session.ts');
        expect(authSession).toContain('queryClient.clear()');
        expect(authSession).toContain('clearBusinessQueries()');
        expect(authSession).toContain("query.queryKey[0] !== 'auth'");
        expect(source('features/auth/auth.service.ts')).toContain('applyAuthenticatedSession');
        const layout = source('layouts/AdminLayout.tsx');
        expect(layout).toContain('key={adminId}');
        expect(layout).not.toContain('tenant_id');
        expect(layout).not.toContain('useSwitchTenant');
        expect(layout).not.toContain('tenants.map');
    });

    test('the app shell follows the Ant Design admin content-surface structure', () => {
        const layout = source('layouts/AdminLayout.tsx');
        expect(layout).toContain('<main className="min-h-0 h-full flex-1 overflow-hidden">');
        expect(layout).toContain(
            '<Content className="m-4 min-h-0 flex-1 overflow-hidden rounded-lg bg-card">'
        );
        expect(layout).toContain('h-12 shrink-0 items-center');
        expect(layout).toContain('shadow-[0_2px_8px_rgba(0,0,0,0.06)]');
    });

    test('shared presentation uses Ant Design defaults where available', () => {
        expect(source('components/page_header.tsx')).toContain("import { Typography } from 'antd'");
        expect(source('components/empty_state.tsx')).toContain("import { Empty } from 'antd'");
        expect(source('components/description_list.tsx')).toContain(
            "import { Descriptions } from 'antd'"
        );
        expect(source('components/data_table.tsx')).not.toContain('PRESENTED_IMAGE_SIMPLE');
        expect(source('components/pagination_bar.tsx')).not.toContain('border-t bg-card');
        const overview = source('pages/overview/index.tsx');
        expect(moduleReferences(overview, /^antd$/)).toEqual(['antd']);
        expect(overview).toContain('<Statistic');
        expect(overview).not.toContain('<button');
    });

    test('login values are owned by Ant Design Form', () => {
        const loginPage = source('pages/auth/index.tsx');
        expect(moduleReferences(loginPage, /^antd$/)).toEqual(['antd']);
        expect(loginPage).toContain('<Form<LoginRequest>');
        expect(loginPage).toContain('onFinish={submit}');
        expect(loginPage).not.toContain('react-hook-form');
        expect(loginPage).not.toContain("register('username')");
    });

    test('sync markers use the generic task boundary', () => {
        expect(existsSync(join(webRoot, 'pages', 'sync_task'))).toBe(false);
        const taskApi = source('pages/task/task.api.ts');
        const taskService = source('pages/task/task.service.ts');
        const taskTypes = source('pages/task/task.types.ts');
        const taskPage = source('pages/task/index.tsx');
        expect(taskApi).toContain('/api/tasks');
        expect(taskApi).not.toContain('/retry');
        expect(taskApi).not.toContain('deleteOperation');
        expect(taskApi).not.toContain('/api/sync-tasks');
        expect(taskTypes).toContain('processed_version: number');
        expect(taskTypes).toContain('count_fails: number');
        expect(taskTypes).not.toContain('parent_task_id');
        expect(taskPage).toContain('暂无同步标记');
        expect(taskPage).not.toContain('parentSequence');
        expect(taskPage).not.toContain('useTaskRetry');
        expect(taskPage).not.toContain('useTaskDelete');
        expect(taskService).toContain('pending + running + retry > 0');
        expect(taskService).not.toContain('waiting');
        expect(source('layouts/AdminLayout.tsx')).toContain('TaskEntry');
        expect(source('layouts/AdminLayout.tsx')).not.toContain('SyncTaskEntry');
    });

    test('business spacing uses fixed production layout widths', () => {
        const dnsZonePage = source('pages/dns_zone/index.tsx');
        expect(dnsZonePage).toContain('id="dns-zone-create"');
        expect(dnsZonePage).toContain('className="min-h-0 flex-1 overflow-y-auto p-4 sm:p-6"');
        expect(dnsZonePage).toContain(
            'className="shrink-0 flex-row justify-end border-t p-4 sm:p-6"'
        );
        expect(source('components/pagination_bar.tsx')).toContain('<Pagination');
        expect(source('pages/cluster/index.tsx')).toContain('lg:w-56');
        expect(source('pages/cluster/index.tsx')).not.toContain('lg:w-64');
    });

    test('certificate sync status uses administrator-facing labels', () => {
        const certificatePage = source('pages/certificate/index.tsx');
        expect(certificatePage).toContain('syncStatusMeta');
        expect(certificatePage).toContain('sync_status');
        expect(certificatePage).toContain("retry: '等待重试'");
        expect(certificatePage).not.toContain('dns_task');
        expect(certificatePage).not.toContain('superseded');
    });

    test('node save exposes form validation failures instead of rejecting silently', () => {
        const nodePanel = source('pages/cluster/node/node_panel.tsx');
        expect(nodePanel).toContain('onClick={submitNodeForm}');
        expect(nodePanel).toContain("toast.warning('请检查节点配置')");
    });

    test('node installer receives persistent credentials as explicit arguments', () => {
        const nodePanel = source('pages/cluster/node/node_panel.tsx');
        expect(nodePanel).toMatch(/bash -s -- \$\{shellQuote\(credentials\.node_id\)\}/);
        expect(nodePanel).not.toMatch(/FLEXEDGE_NODE_(?:ID|SECRET)/);
    });

    test('node relationships and config use one aggregate save body', () => {
        const nodeApi = source('pages/cluster/node/node.api.ts');
        const nodeService = source('pages/cluster/node/node.service.ts');
        const nodeTypes = source('pages/cluster/node/node.types.ts');
        const nodePanel = source('pages/cluster/node/node_panel.tsx');
        expect(nodeApi).toContain('export function createNode(input: NodeSaveInput)');
        expect(nodeApi).toContain(
            'export function saveNode(target: RevisionedResourceRef, input: NodeSaveInput)'
        );
        expect(nodeApi).not.toMatch(/serializeNode|clusterId: input\.cluster_id|updateNode/);
        expect(nodeService).toContain('type NodeSaveCommand');
        expect(nodeService).not.toContain('SaveNodeCommand');
        expect(nodeTypes).toContain('export interface NodeSaveInput');
        expect(nodeTypes).toContain('cluster_id: string;');
        expect(nodeTypes).not.toMatch(/SaveNodeDto|SaveNodeRequest|agent_id/);
        expect(nodePanel).toContain('function buildNodeInput');
        expect(nodePanel).toContain('config: {');
    });

    test('client aggregate IDs work on non-secure HTTP origins', () => {
        const uuid = source('utils/uuid.ts');
        const aggregateInputOwners = [
            source('pages/cluster/node/node_panel.tsx'),
            source('pages/dns_zone/dns_zone.api.ts'),
            source('pages/website/website.api.ts'),
        ].join('\n');
        expect(uuid).toContain('crypto.getRandomValues');
        expect(aggregateInputOwners).toContain('createUuid()');
        expect(aggregateInputOwners).not.toContain('crypto.randomUUID()');
    });

    test('website HTTPS and origin actions expose effective administrator behavior', () => {
        const websiteDetail = source('pages/website/components/website_detail_sheet.tsx');
        expect(websiteDetail).not.toContain('所选证书必须覆盖全部绑定域名');
        expect(websiteDetail).toContain('部分域名将保持 HTTP');
        expect(websiteDetail).toContain('网站至少保留一个源站');
    });

    test('website access log view has an independent stream lifecycle', () => {
        const websitePage = source('pages/website/index.tsx');
        const accessLogDrawer = source('pages/website/access_log_drawer.tsx');
        expect(websitePage).toContain("from './access_log_drawer'");
        expect(websitePage).not.toContain('useWebsiteAccessLogs');
        expect(websitePage).not.toContain('WebsiteAccessLogLimit');
        expect(websitePage).toContain('useWebsiteSave');
        expect(accessLogDrawer).toContain('useWebsiteAccessLogs');
        expect(accessLogDrawer).toContain('LOG_TAIL_LIMIT_OPTIONS');
        expect(accessLogDrawer).not.toContain('useWebsiteSave');
        expect(accessLogDrawer).toContain('rows.length * LOG_ROW_HEIGHT');
        expect(accessLogDrawer).toContain('visibleWindow.rows.map');
        expect(accessLogDrawer).toContain('new ResizeObserver');
        expect(accessLogDrawer).not.toContain('antd');
        const websiteService = source('pages/website/website.service.ts');
        const websiteApi = source('pages/website/website.api.ts');
        expect(websiteService).toContain('EventSource');
        expect(websiteService).toContain('getWebsiteAccessLogStreamUrl');
        expect(websiteService).not.toContain('getWebsiteAccessLogs');
        expect(websiteService).not.toContain('LIVE_LOG_REFETCH_INTERVAL_MS');
        expect(websiteApi).not.toContain('getWebsiteAccessLogs');
    });

    test('website aggregate has one explicit save contract', () => {
        const websiteApi = source('pages/website/website.api.ts');
        const websiteService = source('pages/website/website.service.ts');
        const websiteTypes = source('pages/website/website.types.ts');
        const websiteCreate = source('pages/website/components/website_create_sheet.tsx');
        const websiteDetail = source('pages/website/components/website_detail_sheet.tsx');
        expect(websiteApi).toContain(
            'export function createWebsite(clusterId: string, input: WebsiteSaveInput)'
        );
        expect(websiteApi).toContain(
            'export function saveWebsite(target: WebsiteSaveTarget, input: WebsiteSaveInput)'
        );
        expect(websiteApi).not.toContain('transform:');
        expect(websiteApi).not.toMatch(
            /(?:add|remove|update|redeploy)Website(?:Domain|Origin|\w+)/
        );
        expect(websiteService).toContain('export function useWebsiteSave()');
        expect(websiteService).not.toMatch(
            /useWebsite(?:StatusUpdate|MetadataUpdate|DomainAdd|DomainRemove|OriginAdd|OriginRemove|OriginSettingsUpdate|CompressionUpdate|AccessLogUpdate|HttpsUpdate|Redeploy)/
        );
        expect(websiteTypes).toContain('export interface WebsiteSaveInput');
        expect(websiteTypes).not.toContain('UpdateWebsite');
        expect(websiteTypes).not.toContain('CreateWebsiteDto');
        expect(websiteCreate).toContain('input: createWebsiteInput(data, hostname)');
        expect(websiteDetail).toContain('config: { ...website.config, ...data }');
    });

    test('administrator overview is the authenticated landing page', () => {
        const routes = source('routes/index.tsx');
        const layout = source('layouts/AdminLayout.tsx');
        expect(routes).toContain("import('@/pages/overview')");
        expect(routes).toContain('to="/overview"');
        expect(routes).toContain('path="overview"');
        expect(layout).toContain("path: '/overview'");
        expect(source('pages/overview/overview.api.ts')).toContain('/api/overview');
        expect(source('pages/overview/index.tsx')).not.toContain("from './overview.api'");
    });

    test('aggregate columns are not duplicated inside node and website config', () => {
        const nodeApi = source('pages/cluster/node/node.api.ts');
        const nodePanel = source('pages/cluster/node/node_panel.tsx');
        const websiteApi = source('pages/website/website.api.ts');
        expect(nodeApi).not.toContain('node.config.name');
        expect(nodeApi).not.toContain('node.config.status');
        expect(nodeApi).toContain('input,');
        expect(nodePanel).toContain('config: {');
        expect(websiteApi).not.toContain('website.config.status');
        expect(websiteApi).not.toContain('website.config.category');
        expect(websiteApi).not.toContain('website.config.remark');
        expect(websiteApi).toContain('input,');
        expect(websiteApi).not.toContain('transform(website.config)');
    });

    test('cluster owns its embedded node page without a reverse page-module cycle', () => {
        expect(existsSync(join(webRoot, 'pages', 'node'))).toBe(false);
        expect(source('pages/cluster/index.tsx')).toContain("from './node/node_panel'");
        expect(source('pages/cluster/node/node_panel.tsx')).toContain(
            "from '../components/cluster_select'"
        );
    });
});
