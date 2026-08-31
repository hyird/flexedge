import { MoreHorizontal, Plus, RefreshCw, RotateCcw, Search } from 'lucide-react';
import { useState } from 'react';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { PageHeader } from '@/components/page_header';
import { PaginationBar } from '@/components/pagination_bar';
import { StatusBadge } from '@/components/status_badge';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import {
    DropdownMenu,
    DropdownMenuContent,
    DropdownMenuItem,
    DropdownMenuSeparator,
    DropdownMenuTrigger,
} from '@/components/ui/dropdown_menu';
import { Input } from '@/components/ui/input';
import ClusterSelect from '@/pages/cluster/components/cluster_select';
import WebsiteAccessLogDrawer from './access_log_drawer';
import WebsiteCreateSheet from './components/website_create_sheet';
import WebsiteDetailSheet from './components/website_detail_sheet';
import { ChoiceGroup } from './components/website_form_controls';
import {
    currentWebsiteInput,
    deployStatusMeta,
    websiteResolution,
    websiteSaveTarget,
    websiteStatusMeta,
} from './components/website_helpers';
import { useWebsiteDelete, useWebsiteList, useWebsiteSave } from './website.service';
import type { WebsiteItem, WebsiteStatus } from './website.types';

type StatusFilter = 'all' | WebsiteStatus;

function httpsStatus(item: WebsiteItem) {
    const total = item.config.domains.length;
    const enabled = item.runtime.domain_states.filter(
        (domain) => domain.access_protocol === 'https'
    ).length;
    if (enabled === 0) return { tone: 'neutral', label: '未开启' } as const;
    if (total > 0 && enabled === total) return { tone: 'success', label: '已开启' } as const;
    return { tone: 'warning', label: `${enabled}/${total}` } as const;
}

export default function WebsitePage() {
    const [page, setPage] = useState(1);
    const [pageSize, setPageSize] = useState(20);
    const [searchDraft, setSearchDraft] = useState('');
    const [keyword, setKeyword] = useState('');
    const [clusterId, setClusterId] = useState<string>();
    const [statusFilter, setStatusFilter] = useState<StatusFilter>('all');
    const [appliedClusterId, setAppliedClusterId] = useState<string>();
    const [appliedStatusFilter, setAppliedStatusFilter] = useState<StatusFilter>('all');
    const [createOpen, setCreateOpen] = useState(false);
    const [detailId, setDetailId] = useState<string>();
    const [accessLogId, setAccessLogId] = useState<string>();
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();
    const websites = useWebsiteList({
        page,
        pageSize,
        keyword: keyword || undefined,
        clusterId: appliedClusterId,
        status: appliedStatusFilter === 'all' ? undefined : appliedStatusFilter,
    });
    const save = useWebsiteSave();
    const remove = useWebsiteDelete();

    const columns: Array<DataTableColumn<WebsiteItem>> = [
        {
            key: 'website',
            header: '网站',
            cell: (item) => {
                const domains = item.config.domains.map((domain) => domain.hostname);
                return (
                    <div className="max-w-64">
                        <span className="block max-w-full truncate text-sm font-semibold text-foreground">
                            {item.website_name}
                        </span>
                        <p
                            className="mt-1 truncate font-mono text-xs text-muted-foreground"
                            title={domains.join('、')}
                        >
                            {domains.join('、') || '暂无域名'}
                        </p>
                    </div>
                );
            },
        },
        {
            key: 'cluster',
            header: '部署集群',
            cell: (item) => (
                <span className="block max-w-40 truncate" title={item.cluster_name}>
                    {item.cluster_name}
                </span>
            ),
        },
        {
            key: 'https',
            header: 'HTTPS',
            cell: (item) => {
                const meta = httpsStatus(item);
                return <StatusBadge tone={meta.tone}>{meta.label}</StatusBadge>;
            },
        },
        {
            key: 'access-domain',
            header: '接入域名',
            cell: (item) => (
                <Badge variant="info" className="max-w-48 font-mono" title={item.access_domain}>
                    <span className="truncate">{item.access_domain}</span>
                </Badge>
            ),
        },
        {
            key: 'resolution',
            header: '解析状态',
            cell: (item) => {
                const meta = websiteResolution(item);
                return <StatusBadge tone={meta.tone}>{meta.label}</StatusBadge>;
            },
        },
        {
            key: 'deployment',
            header: '下发状态',
            cell: (item) => {
                const meta = deployStatusMeta[item.deploy_status];
                return (
                    <div>
                        <StatusBadge tone={meta.tone} pulse={item.deploy_status === 'pending'}>
                            {meta.label}
                        </StatusBadge>
                        {item.target_node_count > 0 && (
                            <p className="mt-1 text-xs text-muted-foreground tabular-nums">
                                {item.synced_node_count}/{item.target_node_count} 节点
                            </p>
                        )}
                    </div>
                );
            },
        },
        {
            key: 'status',
            header: '状态',
            cell: (item) => {
                const meta = websiteStatusMeta[item.status];
                return <StatusBadge tone={meta.tone}>{meta.label}</StatusBadge>;
            },
        },
        {
            key: 'actions',
            header: '操作',
            className: 'text-right',
            cell: (item) => (
                <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                        <Button
                            variant="ghost"
                            size="icon-sm"
                            aria-label={`打开${item.website_name}操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem onSelect={() => setDetailId(item.id)}>
                            配置
                        </DropdownMenuItem>
                        <DropdownMenuItem onSelect={() => setAccessLogId(item.id)}>
                            日志
                        </DropdownMenuItem>
                        <DropdownMenuItem
                            disabled={save.isPending}
                            variant={item.status === 'enabled' ? 'destructive' : 'default'}
                            onSelect={() =>
                                save.mutate({
                                    target: websiteSaveTarget(item),
                                    input: {
                                        ...currentWebsiteInput(item),
                                        status: item.status === 'enabled' ? 'disabled' : 'enabled',
                                    },
                                })
                            }
                        >
                            {item.status === 'enabled' ? '暂停' : '启用'}
                        </DropdownMenuItem>
                        <DropdownMenuSeparator />
                        <DropdownMenuItem
                            variant="destructive"
                            onSelect={() =>
                                setConfirmation({
                                    title: `删除网站「${item.website_name}」？`,
                                    content: '删除后会向集群节点下发移除配置。',
                                    danger: true,
                                    confirmText: '删除',
                                    onConfirm: () =>
                                        remove.mutateAsync({
                                            id: item.id,
                                            revision: item.revision,
                                        }),
                                })
                            }
                        >
                            删除
                        </DropdownMenuItem>
                    </DropdownMenuContent>
                </DropdownMenu>
            ),
        },
    ];

    const submitSearch = () => {
        setKeyword(searchDraft.trim());
        setAppliedClusterId(clusterId);
        setAppliedStatusFilter(statusFilter);
        setPage(1);
    };

    return (
        <div className="flex h-full w-full flex-col gap-4 overflow-y-auto p-4 md:overflow-hidden">
            <PageHeader
                eyebrow="Traffic delivery"
                title="网站管理"
                description="管理加速域名、源站与 HTTPS；配置保存后由同步 Worker 下发至节点。"
                actions={
                    <Button onClick={() => setCreateOpen(true)}>
                        <Plus />
                        添加网站
                    </Button>
                }
            />

            <section className="grid shrink-0 gap-3 lg:grid-cols-3 lg:items-end 2xl:grid-cols-[auto_minmax(180px,240px)_minmax(220px,1fr)_auto]">
                <div className="space-y-1.5">
                    <p className="text-xs font-medium text-muted-foreground">启用状态</p>
                    <ChoiceGroup
                        aria-label="启用状态"
                        value={statusFilter}
                        options={[
                            { value: 'all', label: '全部' },
                            { value: 'enabled', label: '已启用' },
                            { value: 'disabled', label: '已暂停' },
                        ]}
                        onValueChange={setStatusFilter}
                        className="min-w-64"
                    />
                </div>
                <div className="space-y-1.5">
                    <p className="text-xs font-medium text-muted-foreground">集群</p>
                    <ClusterSelect
                        value={clusterId}
                        onChange={setClusterId}
                        placeholder="全部集群"
                        className="min-w-0"
                    />
                </div>
                <form
                    id="website-search"
                    className="space-y-1.5"
                    onSubmit={(event) => {
                        event.preventDefault();
                        submitSearch();
                    }}
                >
                    <p className="text-xs font-medium text-muted-foreground">关键词</p>
                    <div>
                        <Input
                            value={searchDraft}
                            placeholder="搜索域名或集群"
                            onChange={(event) => setSearchDraft(event.target.value)}
                        />
                    </div>
                </form>
                <div className="flex flex-wrap items-center gap-2 lg:col-span-3 lg:justify-end 2xl:col-span-1">
                    <Button
                        type="button"
                        variant="outline"
                        onClick={() => {
                            setSearchDraft('');
                            setKeyword('');
                            setClusterId(undefined);
                            setStatusFilter('all');
                            setAppliedClusterId(undefined);
                            setAppliedStatusFilter('all');
                            setPage(1);
                        }}
                    >
                        <RotateCcw />
                        重置
                    </Button>
                    <Button
                        type="button"
                        variant="outline"
                        disabled={websites.isFetching}
                        onClick={() => void websites.refetch()}
                    >
                        <RefreshCw className={websites.isFetching ? 'animate-spin' : undefined} />
                        刷新
                    </Button>
                    <Button type="submit" form="website-search">
                        <Search />
                        搜索
                    </Button>
                </div>
            </section>

            <div className="flex min-h-[28rem] flex-none flex-col overflow-hidden md:min-h-0 md:flex-1">
                <DataTable
                    columns={columns}
                    data={websites.data?.list ?? []}
                    getRowKey={(item) => item.id}
                    loading={websites.isLoading}
                    emptyTitle="暂无网站"
                    emptyDescription="添加首个网站后，即可配置域名、源站与 HTTPS。"
                    className="min-h-0 flex-1 rounded-none border-0"
                    tableClassName="min-w-[1320px]"
                />
                <PaginationBar
                    page={page}
                    pageSize={pageSize}
                    total={websites.data?.total ?? 0}
                    onPageChange={setPage}
                    onPageSizeChange={(nextPageSize) => {
                        setPageSize(nextPageSize);
                        setPage(1);
                    }}
                />
            </div>

            <WebsiteCreateSheet open={createOpen} onClose={() => setCreateOpen(false)} />
            <WebsiteDetailSheet websiteId={detailId} onClose={() => setDetailId(undefined)} />
            <WebsiteAccessLogDrawer
                websiteId={accessLogId}
                onClose={() => setAccessLogId(undefined)}
            />
            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </div>
    );
}
