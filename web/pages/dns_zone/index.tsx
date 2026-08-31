import { zodResolver } from '@hookform/resolvers/zod';
import {
    AlertTriangle,
    MoreHorizontal,
    Plus,
    RefreshCw,
    RotateCcw,
    Save,
    Search,
    Trash2,
} from 'lucide-react';
import { useEffect, useMemo, useState } from 'react';
import { Controller, useForm } from 'react-hook-form';
import { useSearchParams } from 'react-router-dom';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { DescriptionList } from '@/components/description_list';
import { EmptyState } from '@/components/empty_state';
import { PageHeader } from '@/components/page_header';
import { PaginationBar } from '@/components/pagination_bar';
import { StatusBadge } from '@/components/status_badge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Combobox } from '@/components/ui/combobox';
import {
    Dialog,
    DialogContent,
    DialogDescription,
    DialogFooter,
    DialogHeader,
    DialogTitle,
} from '@/components/ui/dialog';
import { Field, FieldDescription, FieldError, FieldGroup, FieldLabel } from '@/components/ui/field';
import { Input } from '@/components/ui/input';
import {
    DropdownMenu,
    DropdownMenuContent,
    DropdownMenuItem,
    DropdownMenuSeparator,
    DropdownMenuTrigger,
} from '@/components/ui/dropdown_menu';
import {
    Select,
    SelectContent,
    SelectItem,
    SelectTrigger,
    SelectValue,
} from '@/components/ui/select';
import {
    Sheet,
    SheetContent,
    SheetDescription,
    SheetFooter,
    SheetHeader,
    SheetTitle,
} from '@/components/ui/sheet';
import { Spinner } from '@/components/ui/spinner';
import { Switch } from '@/components/ui/switch';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { Textarea } from '@/components/ui/textarea';
import { DNS_PROVIDER_REGISTRY } from '@/config/providers';
import DnsProviderSelect from '@/pages/provider/components/dns_provider_select';
import DnsProviderTag from '@/pages/provider/components/dns_provider_tag';
import { useDnsProviderList } from '@/pages/provider/dns_provider.service';
import { formatDateTime } from '@/utils/date';
import { createUuid } from '@/utils/uuid';
import DnsLineTreeSelect, {
    buildDnsLineTreeData,
    type DnsLineTreeNode,
} from './components/dns_line_tree_select';
import { dnsZoneSyncStatusMeta } from './dns_zone.meta';
import {
    type DnsRecordValues,
    type DnsZoneCreateValues,
    dnsRecordSchema,
    dnsZoneCreateSchema,
} from './dns_zone.schema';
import { selectDnsLines, selectDnsRecords } from './dns_zone.selectors';
import {
    useAvailableDnsZones,
    useDnsLineRefresh,
    useDnsZoneCreate,
    useDnsZoneDelete,
    useDnsZoneDetail,
    useDnsZoneList,
    useDnsZoneSave,
    useDnsZoneSync,
} from './dns_zone.service';
import type {
    DnsProviderType,
    DnsRecordItem,
    DnsRecordQuery,
    DnsRecordType,
    DnsSyncConflictPolicy,
    DnsZoneConfig,
    DnsZoneItem,
    DnsZoneQuery,
} from './dns_zone.types';

const recordTypes: DnsRecordType[] = ['A', 'AAAA', 'CNAME', 'TXT', 'MX'];
const ttlOptions = [
    { value: 1, label: '自动' },
    { value: 60, label: '1 分钟' },
    { value: 600, label: '10 分钟' },
    { value: 3600, label: '1 小时' },
    { value: 86400, label: '1 天' },
];

interface DnsRecordEditor {
    id: string;
    revision: number;
    config: DnsZoneConfig;
    dnsProvider: DnsProviderType;
    record?: DnsRecordItem;
}

interface DnsLineRow extends DnsLineTreeNode {
    depth: number;
}

function flattenLines(nodes: DnsLineTreeNode[], depth = 0): DnsLineRow[] {
    return nodes.flatMap((node) => [
        { ...node, depth },
        ...flattenLines(node.children ?? [], depth + 1),
    ]);
}

function SyncBadge({ status }: { status: DnsZoneItem['sync_status'] }) {
    const meta = dnsZoneSyncStatusMeta[status];
    return (
        <StatusBadge tone={meta.tone} pulse={status === 'pending'}>
            {meta.label}
        </StatusBadge>
    );
}

const emptyRecordValues: DnsRecordValues = {
    type: 'A',
    name: '',
    content: '',
    ttl: 600,
    proxied: false,
    line_code: '',
};

export default function DnsZonePage() {
    const [searchParams, setSearchParams] = useSearchParams();
    const initialProviderId = searchParams.get('dns_provider_id') || undefined;
    const [dnsZoneQuery, setDnsZoneQuery] = useState<DnsZoneQuery>({
        page: 1,
        pageSize: 10,
        dnsProviderId: initialProviderId,
    });
    const [filterKeyword, setFilterKeyword] = useState('');
    const [filterProviderId, setFilterProviderId] = useState<string | undefined>(initialProviderId);
    const [recordSearch, setRecordSearch] = useState('');
    const [recordQuery, setRecordQuery] = useState<DnsRecordQuery>({ page: 1, pageSize: 10 });
    const [selectedDnsZoneId, setSelectedDnsZoneId] = useState<string>();
    const [detailView, setDetailView] = useState('basic');
    const [dnsZoneOpen, setDnsZoneOpen] = useState(false);
    const [recordEditor, setRecordEditor] = useState<DnsRecordEditor>();
    const [syncConflictZoneId, setSyncConflictZoneId] = useState<string>();
    const [syncConflictPolicy, setSyncConflictPolicy] = useState<DnsSyncConflictPolicy>('local');
    const [dismissedConflictZoneId, setDismissedConflictZoneId] = useState<string>();
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();

    const createForm = useForm<DnsZoneCreateValues>({
        resolver: zodResolver(dnsZoneCreateSchema),
        defaultValues: { dns_provider_id: '', domain: '' },
    });
    const recordForm = useForm<DnsRecordValues>({
        resolver: zodResolver(dnsRecordSchema),
        defaultValues: emptyRecordValues,
    });
    const selectedProviderId = createForm.watch('dns_provider_id');
    const selectedRecordType = recordForm.watch('type');
    const editingRecord = recordEditor?.record;

    const verifiedProviderAvailability = useDnsProviderList(
        { page: 1, pageSize: 1, status: 'verified' },
        true
    );
    const availableDnsZones = useAvailableDnsZones(selectedProviderId || undefined);
    const dnsZones = useDnsZoneList(dnsZoneQuery);
    const dnsZoneDetail = useDnsZoneDetail(selectedDnsZoneId);
    const selectedDnsZone = dnsZoneDetail.data;
    const dnsLines = useMemo(
        () => (selectedDnsZone ? selectDnsLines(selectedDnsZone) : []),
        [selectedDnsZone]
    );
    const records = useMemo(
        () => (selectedDnsZone ? selectDnsRecords(selectedDnsZone, recordQuery) : undefined),
        [recordQuery, selectedDnsZone]
    );
    const lineRows = useMemo(() => flattenLines(buildDnsLineTreeData(dnsLines)), [dnsLines]);
    const refreshLines = useDnsLineRefresh();
    const createDnsZone = useDnsZoneCreate();
    const syncDnsZone = useDnsZoneSync();
    const deleteDnsZone = useDnsZoneDelete();
    const saveDnsZone = useDnsZoneSave();
    const createFormIsDirty = createForm.formState.isDirty;
    const recordFormIsDirty = recordForm.formState.isDirty;
    const dnsZoneRows = dnsZones.data?.list ?? [];
    const supportsShortTtl =
        (recordEditor?.dnsProvider ?? selectedDnsZone?.dns_provider) !== 'aliyun';
    const supportsProxy = Boolean(
        recordEditor && DNS_PROVIDER_REGISTRY[recordEditor.dnsProvider].supportsProxy
    );
    const syncConflictZone = [selectedDnsZone, ...dnsZoneRows].find((zone): zone is DnsZoneItem =>
        Boolean(zone && zone.id === syncConflictZoneId)
    );
    const detectedConflictZone = [selectedDnsZone, ...dnsZoneRows].find(
        (zone): zone is DnsZoneItem =>
            Boolean(zone && zone.sync_status === 'conflict' && zone.id !== dismissedConflictZoneId)
    );

    useEffect(() => {
        if (!detectedConflictZone) return;
        setSyncConflictZoneId(detectedConflictZone.id);
        setSyncConflictPolicy('local');
    }, [detectedConflictZone]);

    const applyFilters = () => {
        setDnsZoneQuery((current) => ({
            page: 1,
            pageSize: Number(current.pageSize) || 10,
            keyword: filterKeyword.trim() || undefined,
            dnsProviderId: filterProviderId,
        }));
        setSearchParams(filterProviderId ? { dns_provider_id: filterProviderId } : {});
    };

    const resetFilters = () => {
        setFilterKeyword('');
        setFilterProviderId(undefined);
        setSearchParams({});
        setDnsZoneQuery((current) => ({ page: 1, pageSize: Number(current.pageSize) || 10 }));
    };

    const openDnsZone = () => {
        createForm.reset({ dns_provider_id: '', domain: '' });
        setDnsZoneOpen(true);
    };

    const closeDnsZone = () => {
        setDnsZoneOpen(false);
        createForm.reset({ dns_provider_id: '', domain: '' });
    };

    const requestCloseDnsZone = () => {
        if (createDnsZone.isPending) return;
        if (!createFormIsDirty) {
            closeDnsZone();
            return;
        }
        setConfirmation({
            title: '放弃 DNS Zone 草稿？',
            content: '尚未保存的服务商账号和托管域名选择将被清除。',
            confirmText: '放弃草稿',
            danger: true,
            onConfirm: closeDnsZone,
        });
    };

    const openRecord = (record?: DnsRecordItem) => {
        if (!selectedDnsZone) return;
        const defaultLineCode =
            dnsLines.find((line) => line.status === 'enabled')?.line_code ?? 'default';
        setRecordEditor({
            id: selectedDnsZone.id,
            revision: selectedDnsZone.revision,
            config: selectedDnsZone.config,
            dnsProvider: selectedDnsZone.dns_provider,
            record,
        });
        recordForm.reset(
            record
                ? {
                      type: record.type,
                      name: record.name,
                      content: record.content,
                      ttl: record.ttl,
                      priority: record.priority,
                      proxied: record.proxied,
                      line_code: record.line_code,
                  }
                : { ...emptyRecordValues, line_code: defaultLineCode }
        );
    };

    const closeRecord = () => {
        setRecordEditor(undefined);
        recordForm.reset(emptyRecordValues);
    };

    const requestCloseRecord = () => {
        if (saveDnsZone.isPending) return;
        if (!recordFormIsDirty) {
            closeRecord();
            return;
        }
        setConfirmation({
            title: editingRecord ? '放弃解析记录修改？' : '放弃解析记录草稿？',
            content: '尚未保存的解析记录内容将被清除。',
            confirmText: '放弃修改',
            danger: true,
            onConfirm: closeRecord,
        });
    };

    const submitRecord = recordForm.handleSubmit((values) => {
        if (!recordEditor) return;
        const record = {
            ...values,
            name: values.name.trim(),
            content: values.content.trim(),
            priority: values.type === 'MX' ? values.priority : undefined,
            proxied:
                supportsProxy && ['A', 'AAAA', 'CNAME'].includes(values.type)
                    ? values.proxied
                    : false,
        };
        const records = editingRecord
            ? recordEditor.config.records.map((item) =>
                  item.id === editingRecord.id ? { id: editingRecord.id, ...record } : item
              )
            : [...recordEditor.config.records, { id: createUuid(), ...record }];
        saveDnsZone.mutate(
            {
                target: { id: recordEditor.id, revision: recordEditor.revision },
                config: { records },
            },
            { onSuccess: closeRecord }
        );
    });

    const zoneColumns: DataTableColumn<DnsZoneItem>[] = [
        {
            key: 'domain',
            header: '托管域名',
            cell: (zone) => <span className="font-medium text-primary">{zone.domain}</span>,
        },
        {
            key: 'provider',
            header: 'DNS 服务商',
            cell: (zone) => (
                <div className="space-y-1">
                    <DnsProviderTag provider={zone.dns_provider} />
                    <p className="max-w-40 truncate text-xs text-muted-foreground">
                        {zone.dns_provider_name}
                    </p>
                </div>
            ),
        },
        {
            key: 'records',
            header: '记录',
            cell: (zone) => (
                <span className="text-sm tabular-nums">
                    {zone.record_count} 条
                    <span className="ml-1 text-xs text-muted-foreground">
                        · {zone.website_count} 网站
                    </span>
                </span>
            ),
        },
        {
            key: 'status',
            header: '同步状态',
            cell: (zone) => <SyncBadge status={zone.sync_status} />,
        },
        {
            key: 'updated',
            header: '最后更新',
            cell: (zone) => (
                <span className="whitespace-nowrap">{formatDateTime(zone.updated_at)}</span>
            ),
        },
        {
            key: 'actions',
            header: '操作',
            className: 'text-right',
            cell: (zone) => (
                <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                        <Button
                            variant="ghost"
                            size="icon-sm"
                            aria-label={`打开${zone.domain}操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem
                            onSelect={() => {
                                setSelectedDnsZoneId(zone.id);
                                setRecordQuery({ page: 1, pageSize: 10 });
                                setRecordSearch('');
                                setDetailView('basic');
                            }}
                        >
                            查看
                        </DropdownMenuItem>
                        <DropdownMenuItem
                            disabled={syncDnsZone.isPending}
                            onSelect={() => syncDnsZone.mutate({ id: zone.id })}
                        >
                            同步
                        </DropdownMenuItem>
                        <DropdownMenuSeparator />
                        <DropdownMenuItem
                            variant="destructive"
                            onSelect={() =>
                                setConfirmation({
                                    title: `移除域名「${zone.domain}」？`,
                                    content:
                                        '仅删除本地托管配置，不会删除 DNS 服务商中的解析记录。',
                                    danger: true,
                                    confirmText: '删除',
                                    onConfirm: () =>
                                        deleteDnsZone.mutateAsync({
                                            id: zone.id,
                                            revision: zone.revision,
                                        }),
                                })
                            }
                        >
                            移除
                        </DropdownMenuItem>
                    </DropdownMenuContent>
                </DropdownMenu>
            ),
        },
    ];

    const recordColumns: DataTableColumn<DnsRecordItem>[] = [
        {
            key: 'name',
            header: '主机记录',
            cell: (record) => <span className="font-medium">{record.name}</span>,
        },
        {
            key: 'type',
            header: '类型',
            cell: (record) => <Badge variant="outline">{record.type}</Badge>,
        },
        {
            key: 'line',
            header: '线路',
            cell: (record) =>
                dnsLines
                    .find((line) => line.line_code === record.line_code)
                    ?.line_display_name.replace('_', ' / ') ?? record.line_name,
        },
        {
            key: 'content',
            header: '记录值',
            className: 'max-w-64',
            cell: (record) => (
                <span className="block truncate font-mono text-xs" title={record.content}>
                    {record.content}
                </span>
            ),
        },
        {
            key: 'source',
            header: '来源',
            cell: (record) => (
                <Badge variant={record.managed ? 'secondary' : 'outline'}>
                    {record.managed ? '系统自动' : '手动'}
                </Badge>
            ),
        },
        {
            key: 'ttl',
            header: 'TTL',
            cell: (record) => (record.ttl === 1 ? '自动' : `${record.ttl}s`),
        },
        {
            key: 'actions',
            header: '操作',
            className: 'text-right',
            cell: (record) => (
                <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                        <Button
                            variant="ghost"
                            size="icon-sm"
                            aria-label={`打开${record.name || '@'}记录操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem
                            disabled={record.managed}
                            onSelect={() => openRecord(record)}
                        >
                            编辑
                        </DropdownMenuItem>
                        <DropdownMenuSeparator />
                        <DropdownMenuItem
                            variant="destructive"
                            disabled={record.managed}
                            onSelect={() =>
                                selectedDnsZone &&
                                setConfirmation({
                                    title: '确定删除该解析记录？',
                                    content:
                                        '记录先从本地期望配置中删除，再由同步 Worker 推送至 DNS 服务商。',
                                    danger: true,
                                    confirmText: '删除记录',
                                    onConfirm: () =>
                                        saveDnsZone.mutateAsync({
                                            target: {
                                                id: selectedDnsZone.id,
                                                revision: selectedDnsZone.revision,
                                            },
                                            config: {
                                                records: selectedDnsZone.config.records.filter(
                                                    (item) => item.id !== record.id
                                                ),
                                            },
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

    const lineColumns: DataTableColumn<DnsLineRow>[] = [
        {
            key: 'name',
            header: '线路名称',
            cell: (row) => (
                <span className="block" style={{ paddingLeft: `${row.depth * 16}px` }}>
                    {row.title}
                </span>
            ),
        },
        { key: 'code', header: '线路代码', cell: (row) => row.line?.line_code ?? '—' },
        {
            key: 'status',
            header: '状态',
            cell: (row) =>
                row.line ? (
                    <StatusBadge tone={row.line.status === 'enabled' ? 'success' : 'neutral'}>
                        {row.line.status === 'enabled' ? '启用' : '停用'}
                    </StatusBadge>
                ) : (
                    '—'
                ),
        },
        {
            key: 'updated',
            header: '最后同步',
            cell: (row) => formatDateTime(row.line?.last_synced_at),
        },
    ];

    const submitCreate = createForm.handleSubmit((values) => {
        createDnsZone.mutate(values, {
            onSuccess: closeDnsZone,
        });
    });

    return (
        <div className="flex h-full w-full flex-col gap-4 overflow-y-auto p-4 md:overflow-hidden">
            <PageHeader
                eyebrow="域名与证书"
                title="DNS 托管"
                description="维护强类型 DNS 期望状态，并通过同步 Worker 与服务商安全同步。"
                actions={
                    <Button
                        disabled={!verifiedProviderAvailability.data?.list.length}
                        onClick={openDnsZone}
                    >
                        <Plus />
                        添加 Zone
                    </Button>
                }
            />

            <form
                className="flex flex-col gap-3 sm:flex-row sm:items-center"
                onSubmit={(event) => {
                    event.preventDefault();
                    applyFilters();
                }}
            >
                <div className="relative min-w-0 flex-1 sm:max-w-xs">
                    <Search className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                    <Input
                        value={filterKeyword}
                        onChange={(event) => setFilterKeyword(event.target.value)}
                        placeholder="搜索托管域名"
                        className="pl-9"
                    />
                </div>
                <DnsProviderSelect
                    value={filterProviderId}
                    onChange={setFilterProviderId}
                    allowClear
                    enabled
                    placeholder="全部 DNS 服务商账号"
                    className="sm:w-64"
                />
                <div className="flex flex-wrap gap-2 sm:ml-auto">
                    <Button type="button" variant="outline" onClick={resetFilters}>
                        <RotateCcw />
                        重置
                    </Button>
                    <Button
                        type="button"
                        variant="outline"
                        disabled={dnsZones.isFetching}
                        onClick={() => dnsZones.refetch()}
                    >
                        <RefreshCw className={dnsZones.isFetching ? 'animate-spin' : undefined} />
                        刷新
                    </Button>
                    <Button type="submit">
                        <Search />
                        搜索
                    </Button>
                </div>
            </form>

            {dnsZones.isError && (
                <Alert variant="destructive">
                    <AlertTriangle />
                    <AlertTitle>DNS 托管列表加载失败</AlertTitle>
                    <AlertDescription>请检查网络连接后重试。</AlertDescription>
                </Alert>
            )}

            <div className="flex min-h-[28rem] flex-none flex-col overflow-hidden md:min-h-0 md:flex-1">
                <DataTable
                    className="min-h-0 flex-1 rounded-none border-0"
                    columns={zoneColumns}
                    data={dnsZoneRows}
                    getRowKey={(zone) => zone.id}
                    loading={dnsZones.isLoading}
                    emptyTitle="暂无托管域名"
                    emptyDescription="添加 DNS Zone 后即可维护解析记录和线路。"
                />
                <PaginationBar
                    page={Number(dnsZoneQuery.page) || 1}
                    pageSize={Number(dnsZoneQuery.pageSize) || 10}
                    total={dnsZones.data?.total ?? 0}
                    onPageChange={(page) => setDnsZoneQuery((current) => ({ ...current, page }))}
                    onPageSizeChange={(pageSize) =>
                        setDnsZoneQuery((current) => ({ ...current, page: 1, pageSize }))
                    }
                />
            </div>

            <Sheet
                open={Boolean(selectedDnsZoneId)}
                onOpenChange={(open) => {
                    if (open) return;
                    setSelectedDnsZoneId(undefined);
                    setDetailView('basic');
                }}
            >
                <SheetContent className="w-full gap-0 sm:max-w-[min(1280px,calc(100vw-2rem))]">
                    {dnsZoneDetail.isLoading ? (
                        <>
                            <SheetHeader className="sr-only">
                                <SheetTitle>DNS Zone 详情</SheetTitle>
                                <SheetDescription>正在加载 DNS Zone 详情。</SheetDescription>
                            </SheetHeader>
                            <div className="flex h-full items-center justify-center gap-2 text-sm text-muted-foreground">
                                <Spinner />
                                正在加载 DNS Zone
                            </div>
                        </>
                    ) : selectedDnsZone ? (
                        <>
                            <SheetHeader className="border-b pr-14">
                                <div className="flex flex-col gap-4 xl:flex-row xl:items-center xl:justify-between">
                                    <div className="min-w-0">
                                        <div className="flex items-center gap-2">
                                            <SheetTitle className="truncate text-xl">
                                                {selectedDnsZone.domain}
                                            </SheetTitle>
                                            <SyncBadge status={selectedDnsZone.sync_status} />
                                        </div>
                                        <SheetDescription className="mt-1 flex items-center gap-2">
                                            <DnsProviderTag
                                                provider={selectedDnsZone.dns_provider}
                                            />
                                            <span className="truncate">
                                                {selectedDnsZone.dns_provider_name}
                                            </span>
                                        </SheetDescription>
                                    </div>
                                    <div className="flex flex-wrap gap-2">
                                        <Button
                                            variant="outline"
                                            disabled={dnsZoneDetail.isFetching}
                                            onClick={() => dnsZoneDetail.refetch()}
                                        >
                                            <RefreshCw
                                                className={
                                                    dnsZoneDetail.isFetching
                                                        ? 'animate-spin'
                                                        : undefined
                                                }
                                            />
                                            刷新
                                        </Button>
                                        <Button
                                            disabled={syncDnsZone.isPending}
                                            onClick={() =>
                                                syncDnsZone.mutate({ id: selectedDnsZone.id })
                                            }
                                        >
                                            <RefreshCw
                                                className={
                                                    syncDnsZone.isPending
                                                        ? 'animate-spin'
                                                        : undefined
                                                }
                                            />
                                            立即同步
                                        </Button>
                                        <Button
                                            variant="outline"
                                            className="text-destructive hover:text-destructive"
                                            onClick={() =>
                                                setConfirmation({
                                                    title: `移除 DNS Zone「${selectedDnsZone.domain}」？`,
                                                    content:
                                                        '仅删除本地托管配置，不会删除 DNS 服务商中的解析记录。',
                                                    danger: true,
                                                    confirmText: '移除托管',
                                                    onConfirm: async () => {
                                                        await deleteDnsZone.mutateAsync({
                                                            id: selectedDnsZone.id,
                                                            revision: selectedDnsZone.revision,
                                                        });
                                                        setSelectedDnsZoneId(undefined);
                                                    },
                                                })
                                            }
                                        >
                                            <Trash2 />
                                            删除
                                        </Button>
                                    </div>
                                </div>
                            </SheetHeader>
                            <Tabs
                                value={detailView}
                                onValueChange={setDetailView}
                                className="flex min-h-0 flex-1 flex-col p-4 sm:p-5"
                            >
                                <TabsList variant="line" className="shrink-0 justify-start">
                                    <TabsTrigger value="basic">概览</TabsTrigger>
                                    <TabsTrigger value="records">
                                        DNS 记录
                                        <Badge variant="secondary">
                                            {selectedDnsZone.record_count}
                                        </Badge>
                                    </TabsTrigger>
                                    <TabsTrigger value="lines">DNS 线路</TabsTrigger>
                                </TabsList>
                                <TabsContent value="basic" className="mt-5 overflow-y-auto">
                                    {selectedDnsZone.last_error && (
                                        <Alert variant="destructive" className="mb-4">
                                            <AlertTriangle />
                                            <AlertTitle>最近一次操作失败</AlertTitle>
                                            <AlertDescription>
                                                {selectedDnsZone.last_error}
                                            </AlertDescription>
                                        </Alert>
                                    )}
                                    <DescriptionList
                                        columns={3}
                                        items={[
                                            { label: '托管域名', value: selectedDnsZone.domain },
                                            {
                                                label: '服务商账号',
                                                value: selectedDnsZone.dns_provider_name,
                                            },
                                            {
                                                label: 'DNS 线路',
                                                value: `${selectedDnsZone.runtime.lines.length} 条`,
                                            },
                                            {
                                                label: '自定义记录',
                                                value: `${selectedDnsZone.config.records.length} 条`,
                                            },
                                            {
                                                label: '节点记录',
                                                value: `${selectedDnsZone.runtime.projected_records.filter((record) => ['A', 'AAAA'].includes(record.type)).length} 条`,
                                            },
                                            {
                                                label: '网站记录',
                                                value: `${selectedDnsZone.website_count} 个`,
                                            },
                                            {
                                                label: '服务商记录',
                                                value: selectedDnsZone.runtime.records_imported
                                                    ? '已读取'
                                                    : '尚未读取',
                                            },
                                            {
                                                label: '线路更新时间',
                                                value: formatDateTime(
                                                    selectedDnsZone.runtime.lines_synced_at,
                                                    '尚未更新'
                                                ),
                                            },
                                            {
                                                label: '最近远端同步',
                                                value: formatDateTime(
                                                    selectedDnsZone.last_synced_at,
                                                    '尚未更新'
                                                ),
                                            },
                                            {
                                                label: '接入时间',
                                                value: formatDateTime(selectedDnsZone.created_at),
                                            },
                                            {
                                                label: '数据更新时间',
                                                value: formatDateTime(selectedDnsZone.updated_at),
                                            },
                                            {
                                                label: '配置版本',
                                                value: `r${selectedDnsZone.revision}`,
                                            },
                                        ]}
                                    />
                                </TabsContent>
                                <TabsContent
                                    value="records"
                                    className="mt-5 flex min-h-0 flex-col gap-4"
                                >
                                    <div className="flex flex-col gap-3 sm:flex-row sm:items-center">
                                        <form
                                            id="dns-record-search"
                                            className="relative min-w-0 flex-1 sm:max-w-sm"
                                            onSubmit={(event) => {
                                                event.preventDefault();
                                                setRecordQuery((current) => ({
                                                    ...current,
                                                    page: 1,
                                                    keyword: recordSearch.trim() || undefined,
                                                }));
                                            }}
                                        >
                                            <Search className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                                            <Input
                                                value={recordSearch}
                                                onChange={(event) =>
                                                    setRecordSearch(event.target.value)
                                                }
                                                placeholder="搜索主机记录或记录值"
                                                className="pl-9"
                                            />
                                        </form>
                                        <div className="flex gap-2 sm:ml-auto">
                                            <Button
                                                type="button"
                                                variant="outline"
                                                onClick={() => {
                                                    setRecordSearch('');
                                                    setRecordQuery((current) => ({
                                                        ...current,
                                                        page: 1,
                                                        keyword: undefined,
                                                    }));
                                                }}
                                            >
                                                <RotateCcw />
                                                重置
                                            </Button>
                                            <Button
                                                type="button"
                                                variant="outline"
                                                disabled={dnsZoneDetail.isFetching}
                                                onClick={() => dnsZoneDetail.refetch()}
                                            >
                                                <RefreshCw
                                                    className={
                                                        dnsZoneDetail.isFetching
                                                            ? 'animate-spin'
                                                            : undefined
                                                    }
                                                />
                                                刷新
                                            </Button>
                                            <Button type="submit" form="dns-record-search">
                                                <Search />
                                                搜索
                                            </Button>
                                            <Button onClick={() => openRecord()}>
                                                <Plus />
                                                添加记录
                                            </Button>
                                        </div>
                                    </div>
                                    <div className="min-h-0 flex-1 overflow-hidden">
                                        <DataTable
                                            className="h-full"
                                            columns={recordColumns}
                                            data={records?.list ?? []}
                                            getRowKey={(record) => record.id}
                                            loading={dnsZoneDetail.isLoading}
                                            emptyTitle="暂无 DNS 记录"
                                        />
                                    </div>
                                    <PaginationBar
                                        page={Number(recordQuery.page) || 1}
                                        pageSize={Number(recordQuery.pageSize) || 10}
                                        total={records?.total ?? 0}
                                        onPageChange={(page) =>
                                            setRecordQuery((current) => ({ ...current, page }))
                                        }
                                        onPageSizeChange={(pageSize) =>
                                            setRecordQuery((current) => ({
                                                ...current,
                                                page: 1,
                                                pageSize,
                                            }))
                                        }
                                    />
                                </TabsContent>
                                <TabsContent
                                    value="lines"
                                    className="mt-5 flex min-h-0 flex-col gap-4"
                                >
                                    <div className="flex justify-end">
                                        <Button
                                            variant="outline"
                                            disabled={!selectedDnsZoneId || refreshLines.isPending}
                                            onClick={() =>
                                                selectedDnsZoneId &&
                                                refreshLines.mutate(selectedDnsZoneId)
                                            }
                                        >
                                            <RefreshCw
                                                className={
                                                    refreshLines.isPending
                                                        ? 'animate-spin'
                                                        : undefined
                                                }
                                            />
                                            刷新线路
                                        </Button>
                                    </div>
                                    <DataTable
                                        className="min-h-0 flex-1"
                                        columns={lineColumns}
                                        data={lineRows}
                                        getRowKey={(line) => line.value}
                                        loading={dnsZoneDetail.isLoading}
                                        emptyTitle="暂无 DNS 线路"
                                    />
                                </TabsContent>
                            </Tabs>
                        </>
                    ) : (
                        <>
                            <SheetHeader className="sr-only">
                                <SheetTitle>托管域名不存在</SheetTitle>
                                <SheetDescription>
                                    该资源可能已被删除，请关闭后刷新列表。
                                </SheetDescription>
                            </SheetHeader>
                            <EmptyState
                                title="托管域名不存在"
                                description="该资源可能已被删除，请关闭后刷新列表。"
                                action={
                                    <Button onClick={() => setSelectedDnsZoneId(undefined)}>
                                        关闭
                                    </Button>
                                }
                                className="h-full"
                            />
                        </>
                    )}
                </SheetContent>
            </Sheet>

            <Sheet open={dnsZoneOpen} onOpenChange={(open) => !open && requestCloseDnsZone()}>
                <SheetContent className="w-full gap-0 p-0 sm:max-w-xl">
                    <SheetHeader className="shrink-0 border-b px-4 py-4 pr-12 sm:px-6 sm:pr-14">
                        <SheetTitle>添加 DNS Zone</SheetTitle>
                        <SheetDescription>
                            选择已验证账号以及该账号下可以托管的根域名。
                        </SheetDescription>
                    </SheetHeader>
                    <form
                        id="dns-zone-create"
                        className="min-h-0 flex-1 overflow-y-auto p-4 sm:p-6"
                        onSubmit={submitCreate}
                    >
                        <FieldGroup>
                            <Controller
                                control={createForm.control}
                                name="dns_provider_id"
                                render={({ field, fieldState }) => (
                                    <Field data-invalid={fieldState.invalid}>
                                        <FieldLabel htmlFor="dns-zone-provider">
                                            DNS 服务商账号
                                        </FieldLabel>
                                        <DnsProviderSelect
                                            id="dns-zone-provider"
                                            value={field.value || undefined}
                                            onChange={(value) => {
                                                field.onChange(value ?? '');
                                                createForm.setValue('domain', '');
                                            }}
                                            enabled={dnsZoneOpen}
                                            requireVerified
                                            invalid={fieldState.invalid}
                                        />
                                        <FieldError>{fieldState.error?.message}</FieldError>
                                    </Field>
                                )}
                            />
                            <Controller
                                control={createForm.control}
                                name="domain"
                                render={({ field, fieldState }) => (
                                    <Field data-invalid={fieldState.invalid}>
                                        <FieldLabel htmlFor="dns-zone-domain">托管域名</FieldLabel>
                                        <Combobox
                                            id="dns-zone-domain"
                                            value={field.value || undefined}
                                            options={(availableDnsZones.data ?? []).map((zone) => ({
                                                value: zone.domain,
                                                label: zone.domain,
                                            }))}
                                            onValueChange={field.onChange}
                                            placeholder={
                                                selectedProviderId
                                                    ? '选择要托管的域名'
                                                    : '请先选择服务商账号'
                                            }
                                            searchPlaceholder="搜索域名…"
                                            emptyText="该账号下没有可添加的域名"
                                            loading={availableDnsZones.isLoading}
                                            disabled={!selectedProviderId}
                                            invalid={fieldState.invalid}
                                        />
                                        <FieldDescription>
                                            列表来自服务商实时接口，只展示尚未托管的域名。
                                        </FieldDescription>
                                        <FieldError>{fieldState.error?.message}</FieldError>
                                    </Field>
                                )}
                            />
                        </FieldGroup>
                    </form>
                    <SheetFooter className="shrink-0 flex-row justify-end border-t p-4 sm:p-6">
                        <Button
                            variant="outline"
                            disabled={createDnsZone.isPending}
                            onClick={requestCloseDnsZone}
                        >
                            取消
                        </Button>
                        <Button
                            type="submit"
                            form="dns-zone-create"
                            disabled={createDnsZone.isPending}
                        >
                            <Save />
                            {createDnsZone.isPending ? '正在保存' : '保存并同步'}
                        </Button>
                    </SheetFooter>
                </SheetContent>
            </Sheet>

            <Sheet
                open={Boolean(recordEditor)}
                onOpenChange={(open) => !open && requestCloseRecord()}
            >
                <SheetContent className="flex w-full flex-col sm:max-w-xl">
                    <SheetHeader>
                        <SheetTitle>{editingRecord ? '编辑解析记录' : '添加解析记录'}</SheetTitle>
                        <SheetDescription>
                            保存后会创建后台同步任务，不会在请求中直接调用服务商。
                        </SheetDescription>
                    </SheetHeader>
                    <form
                        id="dns-record-form"
                        className="min-h-0 flex-1 overflow-y-auto px-1"
                        onSubmit={submitRecord}
                    >
                        <FieldGroup>
                            <div className="grid gap-4 sm:grid-cols-2">
                                <Controller
                                    control={recordForm.control}
                                    name="type"
                                    render={({ field, fieldState }) => (
                                        <Field data-invalid={fieldState.invalid}>
                                            <FieldLabel>记录类型</FieldLabel>
                                            <Select
                                                value={field.value}
                                                onValueChange={field.onChange}
                                            >
                                                <SelectTrigger
                                                    className="w-full"
                                                    aria-invalid={fieldState.invalid}
                                                >
                                                    <SelectValue />
                                                </SelectTrigger>
                                                <SelectContent>
                                                    {recordTypes.map((type) => (
                                                        <SelectItem key={type} value={type}>
                                                            {type}
                                                        </SelectItem>
                                                    ))}
                                                </SelectContent>
                                            </Select>
                                            <FieldError>{fieldState.error?.message}</FieldError>
                                        </Field>
                                    )}
                                />
                                <Controller
                                    control={recordForm.control}
                                    name="ttl"
                                    render={({ field, fieldState }) => (
                                        <Field data-invalid={fieldState.invalid}>
                                            <FieldLabel>TTL</FieldLabel>
                                            <Select
                                                value={String(field.value)}
                                                onValueChange={(value) =>
                                                    field.onChange(Number(value))
                                                }
                                            >
                                                <SelectTrigger
                                                    className="w-full"
                                                    aria-invalid={fieldState.invalid}
                                                >
                                                    <SelectValue />
                                                </SelectTrigger>
                                                <SelectContent>
                                                    {ttlOptions
                                                        .filter(
                                                            (option) =>
                                                                supportsShortTtl ||
                                                                option.value >= 600
                                                        )
                                                        .map((option) => (
                                                            <SelectItem
                                                                key={option.value}
                                                                value={String(option.value)}
                                                            >
                                                                {option.label}
                                                            </SelectItem>
                                                        ))}
                                                </SelectContent>
                                            </Select>
                                            <FieldError>{fieldState.error?.message}</FieldError>
                                        </Field>
                                    )}
                                />
                            </div>
                            <Field data-invalid={Boolean(recordForm.formState.errors.name)}>
                                <FieldLabel htmlFor="dns-record-name">主机记录</FieldLabel>
                                <Input
                                    id="dns-record-name"
                                    maxLength={253}
                                    placeholder="@ 或 www"
                                    aria-invalid={Boolean(recordForm.formState.errors.name)}
                                    {...recordForm.register('name')}
                                />
                                <FieldError>{recordForm.formState.errors.name?.message}</FieldError>
                            </Field>
                            <Controller
                                control={recordForm.control}
                                name="line_code"
                                render={({ field, fieldState }) => (
                                    <Field data-invalid={fieldState.invalid}>
                                        <FieldLabel htmlFor="dns-record-line">DNS 线路</FieldLabel>
                                        <DnsLineTreeSelect
                                            id="dns-record-line"
                                            value={field.value}
                                            onChange={(lineCode) => field.onChange(lineCode ?? '')}
                                            loading={dnsZoneDetail.isLoading}
                                            refreshing={
                                                dnsZoneDetail.isFetching || refreshLines.isPending
                                            }
                                            invalid={fieldState.invalid}
                                            lines={dnsLines}
                                            onRefresh={
                                                selectedDnsZoneId
                                                    ? () => refreshLines.mutate(selectedDnsZoneId)
                                                    : undefined
                                            }
                                        />
                                        <FieldError>{fieldState.error?.message}</FieldError>
                                    </Field>
                                )}
                            />
                            <Field data-invalid={Boolean(recordForm.formState.errors.content)}>
                                <FieldLabel htmlFor="dns-record-content">记录值</FieldLabel>
                                <Textarea
                                    id="dns-record-content"
                                    maxLength={4096}
                                    rows={4}
                                    aria-invalid={Boolean(recordForm.formState.errors.content)}
                                    {...recordForm.register('content')}
                                />
                                <FieldError>
                                    {recordForm.formState.errors.content?.message}
                                </FieldError>
                            </Field>
                            {selectedRecordType === 'MX' && (
                                <Controller
                                    control={recordForm.control}
                                    name="priority"
                                    render={({ field, fieldState }) => (
                                        <Field data-invalid={fieldState.invalid}>
                                            <FieldLabel htmlFor="dns-record-priority">
                                                优先级
                                            </FieldLabel>
                                            <Input
                                                id="dns-record-priority"
                                                type="number"
                                                min={0}
                                                max={65535}
                                                value={field.value ?? ''}
                                                onChange={(event) =>
                                                    field.onChange(
                                                        event.target.value === ''
                                                            ? undefined
                                                            : Number(event.target.value)
                                                    )
                                                }
                                                aria-invalid={fieldState.invalid}
                                            />
                                            <FieldError>{fieldState.error?.message}</FieldError>
                                        </Field>
                                    )}
                                />
                            )}
                            {supportsProxy &&
                                ['A', 'AAAA', 'CNAME'].includes(selectedRecordType) && (
                                    <Controller
                                        control={recordForm.control}
                                        name="proxied"
                                        render={({ field }) => (
                                            <Field className="flex flex-row items-center justify-between rounded-lg border p-4">
                                                <div>
                                                    <FieldLabel htmlFor="dns-record-proxy">
                                                        Cloudflare 代理
                                                    </FieldLabel>
                                                    <FieldDescription className="mt-1">
                                                        开启后流量先经过 Cloudflare 网络。
                                                    </FieldDescription>
                                                </div>
                                                <Switch
                                                    id="dns-record-proxy"
                                                    checked={field.value}
                                                    onCheckedChange={field.onChange}
                                                />
                                            </Field>
                                        )}
                                    />
                                )}
                        </FieldGroup>
                    </form>
                    <SheetFooter className="flex-row justify-end">
                        <Button
                            variant="outline"
                            disabled={saveDnsZone.isPending}
                            onClick={requestCloseRecord}
                        >
                            取消
                        </Button>
                        <Button
                            type="submit"
                            form="dns-record-form"
                            disabled={saveDnsZone.isPending}
                        >
                            <Save />
                            {saveDnsZone.isPending ? '正在保存' : '保存并同步'}
                        </Button>
                    </SheetFooter>
                </SheetContent>
            </Sheet>

            <Dialog
                open={Boolean(syncConflictZone)}
                onOpenChange={(open) => {
                    if (open) return;
                    if (syncConflictZone) setDismissedConflictZoneId(syncConflictZone.id);
                    setSyncConflictZoneId(undefined);
                }}
            >
                <DialogContent>
                    <DialogHeader>
                        <DialogTitle>处理 DNS 同步冲突</DialogTitle>
                        <DialogDescription>
                            检测到 {syncConflictZone?.runtime.conflicts.length ?? 0}{' '}
                            条本地与服务商内容不一致的记录。
                        </DialogDescription>
                    </DialogHeader>
                    <div className="max-h-56 space-y-2 overflow-y-auto rounded-lg border bg-muted/40 p-3 text-xs">
                        {syncConflictZone?.runtime.conflicts.map((conflict) => (
                            <div key={conflict.id} className="rounded-md bg-background p-2">
                                <p className="font-medium">
                                    {conflict.type} · {conflict.name}
                                </p>
                                <p className="mt-1 truncate text-muted-foreground">
                                    本地 {conflict.local_content} / 远端 {conflict.remote_content}
                                </p>
                            </div>
                        ))}
                    </div>
                    <Select
                        value={syncConflictPolicy}
                        onValueChange={(value) =>
                            setSyncConflictPolicy(value as DnsSyncConflictPolicy)
                        }
                    >
                        <SelectTrigger className="w-full">
                            <SelectValue />
                        </SelectTrigger>
                        <SelectContent>
                            <SelectItem value="local">本地覆盖远端（保留本地期望值）</SelectItem>
                            <SelectItem value="remote">远端覆盖本地（采用服务商值）</SelectItem>
                        </SelectContent>
                    </Select>
                    <DialogFooter>
                        <Button
                            variant="outline"
                            onClick={() => {
                                if (syncConflictZone)
                                    setDismissedConflictZoneId(syncConflictZone.id);
                                setSyncConflictZoneId(undefined);
                            }}
                        >
                            取消
                        </Button>
                        <Button
                            disabled={syncDnsZone.isPending}
                            onClick={() =>
                                syncConflictZone &&
                                syncDnsZone.mutate(
                                    { id: syncConflictZone.id, conflictPolicy: syncConflictPolicy },
                                    {
                                        onSuccess: () => {
                                            setDismissedConflictZoneId(undefined);
                                            setSyncConflictZoneId(undefined);
                                        },
                                    }
                                )
                            }
                        >
                            <RefreshCw
                                className={syncDnsZone.isPending ? 'animate-spin' : undefined}
                            />
                            开始同步
                        </Button>
                    </DialogFooter>
                </DialogContent>
            </Dialog>

            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </div>
    );
}
