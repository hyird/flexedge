import { zodResolver } from '@hookform/resolvers/zod';
import { Empty, Table as AntTable } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import {
    Copy,
    KeyRound,
    LoaderCircle,
    MoreHorizontal,
    Plus,
    RefreshCw,
    Save,
    Terminal,
    Trash2,
    TriangleAlert,
    X,
} from 'lucide-react';
import { useCallback, useEffect, useRef, useState } from 'react';
import { Controller, useFieldArray, useForm } from 'react-hook-form';
import { toast } from '@/components/ui/notification';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { DescriptionList } from '@/components/description_list';
import { EmptyState } from '@/components/empty_state';
import { PaginationBar } from '@/components/pagination_bar';
import { StatusBadge } from '@/components/status_badge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import {
    DropdownMenu,
    DropdownMenuContent,
    DropdownMenuItem,
    DropdownMenuSeparator,
    DropdownMenuTrigger,
} from '@/components/ui/dropdown_menu';
import { Field, FieldDescription, FieldError, FieldLabel } from '@/components/ui/field';
import { Input } from '@/components/ui/input';
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
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { Textarea } from '@/components/ui/textarea';
import { cn } from '@/lib/utils';
import DnsLineTreeSelect from '@/pages/dns_zone/components/dns_line_tree_select';
import { useDnsLineList, useDnsLineRefresh } from '@/pages/dns_zone/dns_zone.service';
import type { DnsLineItem } from '@/pages/dns_zone/dns_zone.types';
import { formatDateTime } from '@/utils/date';
import { LOG_TAIL_LIMIT_OPTIONS } from '@/utils/log_tail';
import { createUuid } from '@/utils/uuid';
import type { ClusterItem } from '../cluster.types';
import ClusterSelect from '../components/cluster_select';
import {
    IP_ADDRESS_MAX_LENGTH,
    NODE_IP_MAX_COUNT,
    NODE_NAME_MAX_LENGTH,
    type NodeFormValues,
    nodeFormSchema,
} from './node.schema';
import {
    useNodeCredentials,
    useNodeCredentialsReset,
    useNodeDelete,
    useNodeList,
    useNodeLogs,
    useNodeSave,
} from './node.service';
import type {
    NodeConnectionStatus,
    NodeCredentialsInfo,
    NodeEndpoint,
    NodeItem,
    NodeLog,
    NodeLogLimit,
    NodeQuery,
    NodeSaveInput,
} from './node.types';

interface NodePanelProps {
    cluster: ClusterItem;
    createRequest: number;
}

type StatusTone = 'success' | 'warning' | 'destructive' | 'info' | 'neutral';

const connectionMeta: Record<NodeConnectionStatus, { tone: StatusTone; label: string }> = {
    unregistered: { tone: 'neutral', label: '离线' },
    online: { tone: 'success', label: '在线' },
    offline: { tone: 'destructive', label: '离线' },
};

const nodeLogLevelMeta: Record<NodeLog['level'], { tone: StatusTone; label: string }> = {
    info: { tone: 'info', label: '信息' },
    warning: { tone: 'warning', label: '警告' },
    error: { tone: 'destructive', label: '错误' },
};

function createDefaultEndpoint(): NodeEndpoint {
    return { ip_address: '', line_code: 'default' };
}

function createDefaultFormValues(clusterId: string): NodeFormValues {
    return {
        cluster_id: clusterId,
        name: '',
        endpoints: [createDefaultEndpoint()],
        status: 'enabled',
    };
}

function normalizeNodeIpAddress(value: string) {
    return value.trim().replace(/\/(?:32|128)$/, '');
}

function getNodeEndpoints(item: NodeItem): NodeEndpoint[] {
    return item.endpoints.map((endpoint) => ({
        ...endpoint,
        ip_address: normalizeNodeIpAddress(endpoint.ip_address),
    }));
}

function getDnsLineLabel(endpoint: NodeEndpoint, lines?: DnsLineItem[]) {
    return (
        lines
            ?.find((line) => line.line_code === endpoint.line_code)
            ?.line_display_name.replace('_', ' / ') ??
        endpoint.line_name ??
        `未知线路（${endpoint.line_code}）`
    );
}

function shellQuote(value: string) {
    return `'${value.replace(/'/g, "'\"'\"'")}'`;
}

function buildInstallCommand(credentials: NodeCredentialsInfo) {
    const serverOrigin = window.location.origin;
    return `curl -fsSL ${shellQuote(`${serverOrigin}/api/agent/install-node.sh`)} | FLEXEDGE_SERVER_ORIGIN=${shellQuote(serverOrigin)} bash -s -- ${shellQuote(credentials.node_id)} ${shellQuote(credentials.secret)}`;
}

function formatUsage(value?: number) {
    return value === undefined ? '—' : `${(value * 100).toFixed(value >= 0.1 ? 0 : 1)}%`;
}

function formatBandwidth(value?: number) {
    if (value === undefined) return '—';
    const units = ['bps', 'Kbps', 'Mbps', 'Gbps', 'Tbps'];
    let amount = value;
    let unitIndex = 0;
    while (amount >= 1000 && unitIndex < units.length - 1) {
        amount /= 1000;
        unitIndex += 1;
    }
    return `${amount.toFixed(amount >= 100 || Number.isInteger(amount) ? 0 : 1)} ${units[unitIndex]}`;
}

function buildNodeInput(data: NodeFormValues): NodeSaveInput {
    return {
        cluster_id: data.cluster_id,
        name: data.name.trim(),
        status: data.status,
        config: {
            endpoints: data.endpoints
                .map((endpoint) => ({
                    id: endpoint.id || createUuid(),
                    ip_address: normalizeNodeIpAddress(endpoint.ip_address),
                    line_code: endpoint.line_code,
                }))
                .filter((endpoint) => endpoint.ip_address),
        },
    };
}

interface NodeInstallScriptPanelProps {
    credentials: NodeCredentialsInfo;
    description: string;
    onCopy: (script: string) => void;
}

function NodeInstallScriptPanel({ credentials, description, onCopy }: NodeInstallScriptPanelProps) {
    const command = buildInstallCommand(credentials);
    return (
        <div className="space-y-3 rounded-xl border border-blue-200 bg-blue-50/60 p-4">
            <div className="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
                <div className="flex min-w-0 gap-3">
                    <span className="flex size-9 shrink-0 items-center justify-center rounded-lg bg-blue-100 text-blue-700">
                        <Terminal className="size-4" />
                    </span>
                    <div>
                        <p className="text-sm font-semibold text-blue-950">一键安装命令</p>
                        <p className="mt-1 text-xs leading-relaxed text-blue-800/75">
                            {description}
                        </p>
                    </div>
                </div>
                <Button size="sm" onClick={() => onCopy(command)}>
                    <Copy />
                    复制命令
                </Button>
            </div>
            <Textarea
                value={command}
                readOnly
                rows={4}
                aria-label="节点一键安装命令"
                className="resize-none bg-background font-mono text-xs"
            />
            <p className="text-xs text-blue-800/70">
                在目标节点的 root shell 粘贴执行；安装逻辑会从 Server 拉取 install-node.sh。
            </p>
        </div>
    );
}

interface VirtualNodeLogTableProps {
    logs: NodeLog[];
    loading: boolean;
}

function VirtualNodeLogTable({ logs, loading }: VirtualNodeLogTableProps) {
    const observer = useRef<ResizeObserver | null>(null);
    const [viewportHeight, setViewportHeight] = useState(320);
    const observeHost = useCallback((host: HTMLDivElement | null) => {
        observer.current?.disconnect();
        if (!host) return;
        const updateHeight = () => setViewportHeight(Math.max(160, host.clientHeight));
        updateHeight();
        const nextObserver = new ResizeObserver(updateHeight);
        nextObserver.observe(host);
        observer.current = nextObserver;
    }, []);

    useEffect(() => () => observer.current?.disconnect(), []);

    const columns: ColumnsType<NodeLog> = [
        {
            title: '时间',
            dataIndex: 'occurred_at',
            width: 190,
            render: (value: string) => (
                <span className="text-xs text-muted-foreground tabular-nums">
                    {formatDateTime(value)}
                </span>
            ),
        },
        {
            title: '级别',
            dataIndex: 'level',
            width: 100,
            align: 'center',
            render: (level: NodeLog['level']) => {
                const meta = nodeLogLevelMeta[level];
                return <StatusBadge tone={meta.tone}>{meta.label}</StatusBadge>;
            },
        },
        {
            title: '分类',
            dataIndex: 'category',
            width: 140,
            render: (value: string) => <Badge variant="outline">{value}</Badge>,
        },
        {
            title: '日志内容',
            dataIndex: 'message',
            ellipsis: true,
            render: (value: string) => <span title={value}>{value}</span>,
        },
    ];

    return (
        <div ref={observeHost} className="h-full min-h-40 overflow-hidden rounded-xl border">
            <AntTable<NodeLog>
                rowKey="id"
                columns={columns}
                dataSource={logs}
                loading={loading}
                pagination={false}
                sticky
                virtual
                scroll={{ x: 760, y: viewportHeight }}
                locale={{
                    emptyText: (
                        <Empty
                            image={Empty.PRESENTED_IMAGE_SIMPLE}
                            description={
                                <div>
                                    <div>暂无节点日志</div>
                                    <div className="mt-1 text-xs text-muted-foreground">
                                        Node 上报日志后会实时显示在这里
                                    </div>
                                </div>
                            }
                        />
                    ),
                }}
            />
        </div>
    );
}

export default function NodePanel({ cluster, createRequest }: NodePanelProps) {
    const clusterId = cluster.id;
    const [open, setOpen] = useState(false);
    const [editing, setEditing] = useState<NodeItem>();
    const [detailId, setDetailId] = useState<string>();
    const [detailTab, setDetailTab] = useState('basic');
    const [logLimit, setLogLimit] = useState<NodeLogLimit>(100);
    const [credentials, setCredentials] = useState<NodeCredentialsInfo>();
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();
    const [selectedClusterOption, setSelectedClusterOption] = useState<ClusterItem>(cluster);
    const [query, setQuery] = useState<NodeQuery>({ page: 1, pageSize: 10, clusterId });
    const lastCreateRequest = useRef(0);
    const form = useForm<NodeFormValues>({
        resolver: zodResolver(nodeFormSchema),
        defaultValues: createDefaultFormValues(clusterId),
    });
    const formIsDirty = form.formState.isDirty;
    const endpointFields = useFieldArray({
        control: form.control,
        name: 'endpoints',
        keyName: 'fieldKey',
    });
    const list = useNodeList(query, true);
    const nodeDnsLines = useDnsLineList(cluster.dns_zone_id);
    const formDnsLines = useDnsLineList(selectedClusterOption?.dns_zone_id);
    const refreshLines = useDnsLineRefresh();
    const saveNodeMutation = useNodeSave();
    const resetCredentials = useNodeCredentialsReset();
    const removeNode = useNodeDelete();
    const detailNode = list.data?.list.find((item) => item.id === detailId);
    const nodeLogs = useNodeLogs(detailId, logLimit, Boolean(detailId) && detailTab === 'logs');
    const storedCredentials = useNodeCredentials(editing?.id, open && editing !== undefined);
    const activeCredentials = credentials ?? storedCredentials.data;
    const canRefreshDnsLines = Boolean(selectedClusterOption?.dns_zone_id);

    const closeForm = () => {
        setOpen(false);
        setEditing(undefined);
        setCredentials(undefined);
        setSelectedClusterOption(cluster);
        form.reset(createDefaultFormValues(clusterId));
    };

    const requestCloseForm = () => {
        if (saveNodeMutation.isPending) return;
        if (!formIsDirty) {
            closeForm();
            return;
        }
        setConfirmation({
            title: editing ? '放弃未保存的节点修改？' : '放弃正在创建的节点？',
            content: '当前节点配置尚未保存，关闭后本次填写的内容将丢失。',
            confirmText: editing ? '放弃修改' : '放弃创建',
            danger: true,
            onConfirm: closeForm,
        });
    };

    const saveNode = (data: NodeFormValues) => {
        const command = {
            target: editing ? { id: editing.id, revision: editing.revision } : undefined,
            input: buildNodeInput(data),
        };
        saveNodeMutation.mutate(command, {
            onSuccess: (result) => {
                if (result) {
                    setCredentials(result);
                    setEditing(undefined);
                    form.reset(createDefaultFormValues(clusterId));
                    return;
                }
                closeForm();
            },
        });
    };

    const submitNodeForm = form.handleSubmit(saveNode, () => {
        toast.warning('请检查节点配置');
    });

    const copyInstallScript = async (script: string) => {
        try {
            await navigator.clipboard.writeText(script);
            toast.success('一键安装命令已复制');
        } catch {
            toast.error('复制失败，请手动选择命令内容复制');
        }
    };

    const openForm = (item?: NodeItem) => {
        setEditing(item);
        setCredentials(undefined);
        setSelectedClusterOption(cluster);
        form.reset(
            item
                ? {
                      cluster_id: item.cluster_id,
                      name: item.name,
                      endpoints: getNodeEndpoints(item),
                      status: item.status,
                  }
                : createDefaultFormValues(clusterId)
        );
        setOpen(true);
    };

    const openDetail = (id: string) => {
        setDetailTab('basic');
        setLogLimit(100);
        setDetailId(id);
    };

    const closeDetail = () => {
        setDetailId(undefined);
        setDetailTab('basic');
        setLogLimit(100);
    };

    useEffect(() => {
        setQuery((current) => ({ ...current, page: 1, clusterId }));
        setSelectedClusterOption(cluster);
        setDetailId(undefined);
        setDetailTab('basic');
        setLogLimit(100);
    }, [cluster, clusterId]);

    useEffect(() => {
        if (createRequest <= 0 || createRequest === lastCreateRequest.current) return;
        lastCreateRequest.current = createRequest;
        setEditing(undefined);
        setCredentials(undefined);
        setSelectedClusterOption(cluster);
        form.reset(createDefaultFormValues(clusterId));
        setOpen(true);
    }, [cluster, clusterId, createRequest, form]);

    const nodeColumns: DataTableColumn<NodeItem>[] = [
        {
            key: 'name',
            header: '节点名称',
            cell: (item) => (
                <div className="min-w-36">
                    <p className="truncate font-medium" title={item.name}>
                        {item.name}
                    </p>
                    <p className="mt-0.5 truncate text-xs text-muted-foreground">
                        {item.agent_version ?? item.runtime.agent_version ?? '版本未上报'}
                    </p>
                </div>
            ),
        },
        {
            key: 'ip',
            header: 'IP',
            cell: (item) => (
                <div className="flex flex-col items-start gap-1">
                    {getNodeEndpoints(item).map((endpoint) => (
                        <Badge
                            key={endpoint.id ?? endpoint.ip_address}
                            variant="info"
                            className="font-mono"
                        >
                            {endpoint.ip_address}
                        </Badge>
                    ))}
                </div>
            ),
        },
        {
            key: 'dns-line',
            header: 'DNS 线路',
            cell: (item) => (
                <div className="flex flex-col items-start gap-1">
                    {getNodeEndpoints(item).map((endpoint) => (
                        <Badge key={endpoint.id ?? endpoint.ip_address} variant="outline">
                            {getDnsLineLabel(endpoint, nodeDnsLines.data)}
                        </Badge>
                    ))}
                </div>
            ),
        },
        {
            key: 'cpu',
            header: 'CPU',
            className: 'text-center tabular-nums',
            headerClassName: 'text-center',
            cell: (item) => (
                <span
                    className={cn(
                        item.runtime.cpu_usage && item.runtime.cpu_usage > 0.5 && 'text-destructive'
                    )}
                >
                    {formatUsage(item.runtime.cpu_usage)}
                </span>
            ),
        },
        {
            key: 'memory',
            header: '内存',
            className: 'text-center tabular-nums',
            headerClassName: 'text-center',
            cell: (item) => (
                <span
                    className={cn(
                        item.runtime.memory_usage &&
                            item.runtime.memory_usage > 0.8 &&
                            'text-destructive'
                    )}
                >
                    {formatUsage(item.runtime.memory_usage)}
                </span>
            ),
        },
        {
            key: 'bandwidth',
            header: '下行带宽',
            className: 'text-center tabular-nums',
            headerClassName: 'text-center',
            cell: (item) => formatBandwidth(item.runtime.traffic_out_bps),
        },
        {
            key: 'connections',
            header: '连接数',
            className: 'text-center tabular-nums',
            headerClassName: 'text-center',
            cell: (item) =>
                item.runtime.connection_count === undefined
                    ? '—'
                    : item.runtime.connection_count.toLocaleString('zh-CN'),
        },
        {
            key: 'load',
            header: '负载',
            className: 'text-center tabular-nums',
            headerClassName: 'text-center',
            cell: (item) =>
                item.runtime.load_1m === undefined ? '—' : item.runtime.load_1m.toFixed(2),
        },
        {
            key: 'status',
            header: '状态',
            cell: (item) => {
                const meta =
                    item.status === 'disabled'
                        ? { tone: 'neutral' as const, label: '停用' }
                        : connectionMeta[item.connection_status];
                return (
                    <StatusBadge tone={meta.tone} pulse={item.connection_status === 'online'}>
                        {meta.label}
                    </StatusBadge>
                );
            },
        },
        {
            key: 'actions',
            header: '操作',
            className: 'text-right',
            cell: (item) => {
                const toggling =
                    saveNodeMutation.isPending &&
                    saveNodeMutation.variables?.target?.id === item.id;
                return (
                    <DropdownMenu>
                        <DropdownMenuTrigger asChild>
                            <Button
                                variant="ghost"
                                size="icon-sm"
                                aria-label={`打开${item.name}操作菜单`}
                            >
                                <MoreHorizontal />
                            </Button>
                        </DropdownMenuTrigger>
                        <DropdownMenuContent align="end">
                            <DropdownMenuItem onSelect={() => openDetail(item.id)}>
                                详情
                            </DropdownMenuItem>
                            <DropdownMenuItem onSelect={() => openForm(item)}>
                                设置
                            </DropdownMenuItem>
                            <DropdownMenuSeparator />
                            <DropdownMenuItem
                                variant={item.status === 'enabled' ? 'destructive' : 'default'}
                                disabled={toggling}
                                onSelect={() =>
                                    saveNodeMutation.mutate({
                                        target: { id: item.id, revision: item.revision },
                                        input: {
                                            cluster_id: item.cluster_id,
                                            name: item.name,
                                            status:
                                                item.status === 'enabled' ? 'disabled' : 'enabled',
                                            config: item.config,
                                        },
                                    })
                                }
                            >
                                {item.status === 'enabled' ? '停用' : '启用'}
                            </DropdownMenuItem>
                        </DropdownMenuContent>
                    </DropdownMenu>
                );
            },
        },
    ];

    return (
        <div className="flex min-h-0 min-w-0 flex-1 flex-col gap-3 overflow-hidden">
            {list.isError && (
                <Alert variant="destructive" className="shrink-0">
                    <TriangleAlert />
                    <AlertTitle>节点列表加载失败</AlertTitle>
                    <AlertDescription>请稍后刷新页面重试。</AlertDescription>
                </Alert>
            )}

            <div className="flex min-h-[28rem] flex-none flex-col overflow-hidden rounded-xl border bg-card md:min-h-0 md:flex-1">
                <DataTable
                    columns={nodeColumns}
                    data={list.data?.list}
                    getRowKey={(item) => item.id}
                    loading={list.isLoading || list.isFetching}
                    emptyTitle="暂无边缘节点"
                    emptyDescription="使用上方“添加节点”创建节点并获取一键安装命令"
                    className="min-h-0 flex-1 rounded-none border-0"
                    tableClassName="min-w-[1280px]"
                />
                <PaginationBar
                    page={Number(query.page) || 1}
                    pageSize={Number(query.pageSize) || 10}
                    total={list.data?.total ?? 0}
                    pageSizeOptions={[10, 20, 50, 100]}
                    onPageChange={(page) => setQuery({ ...query, page })}
                    onPageSizeChange={(pageSize) => setQuery({ ...query, page: 1, pageSize })}
                />
            </div>

            <Sheet open={Boolean(detailId)} onOpenChange={(nextOpen) => !nextOpen && closeDetail()}>
                <SheetContent
                    side="right"
                    className="w-full gap-0 p-0 sm:max-w-[min(56rem,calc(100vw-2rem))]"
                >
                    <SheetHeader className="border-b px-4 py-4 pr-12 sm:px-6 sm:pr-14">
                        <div className="flex flex-wrap items-start justify-between gap-3">
                            <div>
                                <SheetTitle>
                                    {detailNode ? `${detailNode.name} 详情` : '节点详情'}
                                </SheetTitle>
                                <SheetDescription>
                                    查看节点配置、运行状态和实时日志
                                </SheetDescription>
                            </div>
                            {detailNode && (
                                <div className="flex items-center gap-2">
                                    <StatusBadge
                                        tone={
                                            detailNode.status === 'enabled' ? 'success' : 'neutral'
                                        }
                                    >
                                        {detailNode.status === 'enabled' ? '启用' : '停用'}
                                    </StatusBadge>
                                    <Button
                                        variant="outline"
                                        size="sm"
                                        disabled={list.isFetching}
                                        onClick={() => list.refetch()}
                                    >
                                        <RefreshCw
                                            className={cn(list.isFetching && 'animate-spin')}
                                        />
                                        刷新
                                    </Button>
                                </div>
                            )}
                        </div>
                    </SheetHeader>

                    {detailNode ? (
                        <Tabs
                            value={detailTab}
                            onValueChange={setDetailTab}
                            className="min-h-0 flex-1 gap-0 overflow-hidden"
                        >
                            <div className="shrink-0 border-b px-4 sm:px-6">
                                <TabsList variant="line">
                                    <TabsTrigger value="basic">基本信息</TabsTrigger>
                                    <TabsTrigger value="runtime">运行状态</TabsTrigger>
                                    <TabsTrigger value="logs">节点日志</TabsTrigger>
                                </TabsList>
                            </div>

                            <TabsContent
                                value="basic"
                                className="min-h-0 overflow-y-auto p-4 sm:p-6"
                            >
                                <div className="space-y-5">
                                    <DescriptionList
                                        items={[
                                            { label: '节点名称', value: detailNode.name },
                                            { label: '所属集群', value: detailNode.cluster_name },
                                            {
                                                label: '启用状态',
                                                value: (
                                                    <StatusBadge
                                                        tone={
                                                            detailNode.status === 'enabled'
                                                                ? 'success'
                                                                : 'neutral'
                                                        }
                                                    >
                                                        {detailNode.status === 'enabled'
                                                            ? '已启用'
                                                            : '已停用'}
                                                    </StatusBadge>
                                                ),
                                            },
                                            {
                                                label: '连接状态',
                                                value: (
                                                    <StatusBadge
                                                        tone={
                                                            connectionMeta[
                                                                detailNode.connection_status
                                                            ].tone
                                                        }
                                                    >
                                                        {
                                                            connectionMeta[
                                                                detailNode.connection_status
                                                            ].label
                                                        }
                                                    </StatusBadge>
                                                ),
                                            },
                                            {
                                                label: 'Node 版本',
                                                value:
                                                    detailNode.agent_version ??
                                                    detailNode.runtime.agent_version ??
                                                    '尚未上报',
                                            },
                                            {
                                                label: '创建时间',
                                                value: formatDateTime(detailNode.created_at),
                                            },
                                            {
                                                label: '更新时间',
                                                value: formatDateTime(detailNode.updated_at),
                                            },
                                            {
                                                label: '资源版本',
                                                value: String(detailNode.revision),
                                            },
                                        ]}
                                    />

                                    <section className="space-y-2">
                                        <h3 className="text-sm font-semibold">IP 与 DNS 线路</h3>
                                        <div className="overflow-hidden rounded-xl border">
                                            <AntTable<NodeEndpoint>
                                                rowKey={(endpoint) =>
                                                    endpoint.id ?? endpoint.ip_address
                                                }
                                                dataSource={getNodeEndpoints(detailNode)}
                                                pagination={false}
                                                size="small"
                                                scroll={{ x: 560 }}
                                                columns={[
                                                    {
                                                        title: 'IP 地址',
                                                        dataIndex: 'ip_address',
                                                        render: (value: string) => (
                                                            <Badge
                                                                variant="info"
                                                                className="font-mono"
                                                            >
                                                                {value}
                                                            </Badge>
                                                        ),
                                                    },
                                                    {
                                                        title: '地址类型',
                                                        dataIndex: 'ip_address',
                                                        render: (value: string) =>
                                                            value.includes(':') ? 'IPv6' : 'IPv4',
                                                    },
                                                    {
                                                        title: 'DNS 线路',
                                                        render: (_value, endpoint) =>
                                                            getDnsLineLabel(
                                                                endpoint,
                                                                nodeDnsLines.data
                                                            ),
                                                    },
                                                ]}
                                            />
                                        </div>
                                    </section>
                                </div>
                            </TabsContent>

                            <TabsContent
                                value="runtime"
                                className="min-h-0 overflow-y-auto p-4 sm:p-6"
                            >
                                <DescriptionList
                                    items={[
                                        {
                                            label: '连接状态',
                                            value: (
                                                <StatusBadge
                                                    tone={
                                                        connectionMeta[detailNode.connection_status]
                                                            .tone
                                                    }
                                                >
                                                    {
                                                        connectionMeta[detailNode.connection_status]
                                                            .label
                                                    }
                                                </StatusBadge>
                                            ),
                                        },
                                        {
                                            label: '健康状态',
                                            value: detailNode.runtime.health ?? '未上报',
                                        },
                                        {
                                            label: 'CPU 用量',
                                            value: formatUsage(detailNode.runtime.cpu_usage),
                                        },
                                        {
                                            label: '内存用量',
                                            value: formatUsage(detailNode.runtime.memory_usage),
                                        },
                                        {
                                            label: '下行带宽',
                                            value: formatBandwidth(
                                                detailNode.runtime.traffic_out_bps
                                            ),
                                        },
                                        {
                                            label: '连接数',
                                            value:
                                                detailNode.runtime.connection_count === undefined
                                                    ? '—'
                                                    : detailNode.runtime.connection_count.toLocaleString(
                                                          'zh-CN'
                                                      ),
                                        },
                                        {
                                            label: '1 分钟负载',
                                            value:
                                                detailNode.runtime.load_1m === undefined
                                                    ? '—'
                                                    : detailNode.runtime.load_1m.toFixed(2),
                                        },
                                        {
                                            label: '待发送日志',
                                            value:
                                                detailNode.runtime.queued_log_events === undefined
                                                    ? '—'
                                                    : detailNode.runtime.queued_log_events.toLocaleString(
                                                          'zh-CN'
                                                      ),
                                        },
                                        {
                                            label: '本次启动丢弃日志',
                                            value:
                                                detailNode.runtime.dropped_log_events ===
                                                undefined ? (
                                                    '—'
                                                ) : detailNode.runtime.dropped_log_events > 0 ? (
                                                    <span className="text-destructive">
                                                        {detailNode.runtime.dropped_log_events.toLocaleString(
                                                            'zh-CN'
                                                        )}
                                                    </span>
                                                ) : (
                                                    '0'
                                                ),
                                        },
                                        {
                                            label: '最后心跳',
                                            value: formatDateTime(
                                                detailNode.last_heartbeat_at,
                                                '尚未上报'
                                            ),
                                        },
                                        {
                                            label: '期望配置版本',
                                            value: String(detailNode.node_spec_revision),
                                        },
                                        {
                                            label: '已应用配置版本',
                                            value: String(detailNode.applied_node_spec_revision),
                                        },
                                        {
                                            label: '活动发布',
                                            value: detailNode.active_release_id ?? '—',
                                        },
                                        {
                                            label: '活动清单摘要',
                                            value: detailNode.active_manifest_digest ?? '—',
                                        },
                                        {
                                            label: '最近错误',
                                            value: detailNode.runtime.last_error ? (
                                                <span className="text-destructive">
                                                    {detailNode.runtime.last_error}
                                                </span>
                                            ) : (
                                                '—'
                                            ),
                                        },
                                    ]}
                                />
                            </TabsContent>

                            <TabsContent
                                value="logs"
                                className="flex min-h-0 flex-col gap-3 overflow-hidden p-4 sm:p-6"
                            >
                                <div className="flex shrink-0 flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
                                    <div>
                                        <p className="text-sm font-semibold">节点日志</p>
                                        <p className="text-xs text-muted-foreground">
                                            {nodeLogs.data?.list.length ?? 0} 条 · 实时流
                                        </p>
                                    </div>
                                    <div className="flex flex-wrap items-center gap-2">
                                        <fieldset className="flex gap-1 border-0 p-0">
                                            <legend className="sr-only">日志数量</legend>
                                            {LOG_TAIL_LIMIT_OPTIONS.map((option) => (
                                                <Button
                                                    key={option.value}
                                                    variant={
                                                        logLimit === option.value
                                                            ? 'default'
                                                            : 'outline'
                                                    }
                                                    size="sm"
                                                    aria-pressed={logLimit === option.value}
                                                    onClick={() => setLogLimit(option.value)}
                                                >
                                                    {option.label}
                                                </Button>
                                            ))}
                                        </fieldset>
                                        <Button
                                            variant="outline"
                                            size="sm"
                                            disabled={nodeLogs.isFetching}
                                            aria-label="刷新节点日志"
                                            onClick={() => nodeLogs.refetch()}
                                        >
                                            <RefreshCw
                                                className={cn(
                                                    nodeLogs.isFetching && 'animate-spin'
                                                )}
                                            />
                                            刷新
                                        </Button>
                                    </div>
                                </div>
                                <div className="min-h-0 flex-1">
                                    <VirtualNodeLogTable
                                        logs={nodeLogs.data?.list ?? []}
                                        loading={nodeLogs.isLoading}
                                    />
                                </div>
                            </TabsContent>
                        </Tabs>
                    ) : (
                        <EmptyState title="节点不存在" description="刷新列表后重新打开节点详情" />
                    )}
                </SheetContent>
            </Sheet>

            <Sheet open={open} onOpenChange={(nextOpen) => !nextOpen && requestCloseForm()}>
                <SheetContent side="right" className="w-full gap-0 p-0 sm:max-w-xl">
                    <SheetHeader className="border-b px-4 py-4 pr-12 sm:px-6 sm:pr-14">
                        <SheetTitle>
                            {credentials && !editing
                                ? '节点接入'
                                : editing
                                  ? '节点设置'
                                  : '新建节点'}
                        </SheetTitle>
                        <SheetDescription>
                            {credentials && !editing
                                ? '在目标服务器执行安装命令，完成节点注册与接入。'
                                : '配置节点所属集群、服务 IP 和对应 DNS 线路。'}
                        </SheetDescription>
                    </SheetHeader>

                    <div className="min-h-0 flex-1 overflow-y-auto p-4 sm:p-6">
                        {credentials && !editing ? (
                            <NodeInstallScriptPanel
                                credentials={credentials}
                                description="节点已创建。复制这一条脚本到目标服务器执行即可完成接入。"
                                onCopy={copyInstallScript}
                            />
                        ) : (
                            <form
                                id="node-form"
                                noValidate
                                className="space-y-6"
                                onSubmit={submitNodeForm}
                            >
                                <section className="space-y-4">
                                    <div>
                                        <h3 className="text-sm font-semibold">节点信息</h3>
                                        <p className="mt-1 text-xs text-muted-foreground">
                                            节点名称用于控制面识别，所属集群决定配置发布边界。
                                        </p>
                                    </div>
                                    <div className="grid gap-4 sm:grid-cols-2">
                                        <Field data-invalid={Boolean(form.formState.errors.name)}>
                                            <FieldLabel htmlFor="node-name">节点名称</FieldLabel>
                                            <Input
                                                id="node-name"
                                                maxLength={NODE_NAME_MAX_LENGTH}
                                                placeholder="hz-edge-03"
                                                aria-invalid={Boolean(form.formState.errors.name)}
                                                {...form.register('name')}
                                            />
                                            <FieldError>
                                                {form.formState.errors.name?.message}
                                            </FieldError>
                                        </Field>

                                        <Controller
                                            control={form.control}
                                            name="cluster_id"
                                            render={({ field, fieldState }) => (
                                                <Field data-invalid={fieldState.invalid}>
                                                    <FieldLabel htmlFor="node-cluster">
                                                        所属集群
                                                    </FieldLabel>
                                                    <ClusterSelect
                                                        id="node-cluster"
                                                        value={field.value}
                                                        onChange={field.onChange}
                                                        enabled
                                                        requireEnabled
                                                        seedOptions={[
                                                            cluster,
                                                            selectedClusterOption,
                                                        ]}
                                                        invalid={fieldState.invalid}
                                                        onClusterChange={(nextCluster) => {
                                                            if (nextCluster)
                                                                setSelectedClusterOption(
                                                                    nextCluster
                                                                );
                                                            form.setValue(
                                                                'endpoints',
                                                                form
                                                                    .getValues('endpoints')
                                                                    .map((endpoint) => ({
                                                                        ...endpoint,
                                                                        line_code: 'default',
                                                                    })),
                                                                {
                                                                    shouldDirty: true,
                                                                    shouldValidate: true,
                                                                }
                                                            );
                                                        }}
                                                    />
                                                    <FieldError>
                                                        {fieldState.error?.message}
                                                    </FieldError>
                                                </Field>
                                            )}
                                        />
                                    </div>
                                </section>

                                <section className="space-y-3 border-t pt-5">
                                    <div className="flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
                                        <div>
                                            <h3 className="text-sm font-semibold">
                                                IP 与 DNS 线路
                                            </h3>
                                            <p className="mt-1 text-xs text-muted-foreground">
                                                每个 IP 选择一条 DNS 解析线路，最多{' '}
                                                {NODE_IP_MAX_COUNT} 个。
                                            </p>
                                        </div>
                                        <Button
                                            type="button"
                                            variant="outline"
                                            size="sm"
                                            disabled={!canRefreshDnsLines || refreshLines.isPending}
                                            onClick={() => {
                                                if (selectedClusterOption?.dns_zone_id) {
                                                    refreshLines.mutate(
                                                        selectedClusterOption.dns_zone_id
                                                    );
                                                }
                                            }}
                                        >
                                            <RefreshCw
                                                className={cn(
                                                    refreshLines.isPending && 'animate-spin'
                                                )}
                                            />
                                            刷新线路
                                        </Button>
                                    </div>

                                    <div className="space-y-3">
                                        {endpointFields.fields.map((endpointField, index) => {
                                            const ipError =
                                                form.formState.errors.endpoints?.[index]
                                                    ?.ip_address;
                                            const lineError =
                                                form.formState.errors.endpoints?.[index]?.line_code;
                                            return (
                                                <div
                                                    key={endpointField.fieldKey}
                                                    className="grid gap-3 rounded-xl border bg-muted/20 p-3 sm:grid-cols-[minmax(0,0.9fr)_minmax(0,1.1fr)_auto]"
                                                >
                                                    <Field data-invalid={Boolean(ipError)}>
                                                        <FieldLabel
                                                            htmlFor={`node-ip-${endpointField.fieldKey}`}
                                                        >
                                                            IP 地址 {index + 1}
                                                        </FieldLabel>
                                                        <Input
                                                            id={`node-ip-${endpointField.fieldKey}`}
                                                            maxLength={IP_ADDRESS_MAX_LENGTH}
                                                            placeholder="203.0.113.10"
                                                            aria-invalid={Boolean(ipError)}
                                                            {...form.register(
                                                                `endpoints.${index}.ip_address`
                                                            )}
                                                        />
                                                        <FieldError>{ipError?.message}</FieldError>
                                                    </Field>

                                                    <Controller
                                                        control={form.control}
                                                        name={`endpoints.${index}.line_code`}
                                                        render={({ field, fieldState }) => (
                                                            <Field
                                                                data-invalid={fieldState.invalid}
                                                            >
                                                                <FieldLabel
                                                                    htmlFor={`node-line-${endpointField.fieldKey}`}
                                                                >
                                                                    DNS 线路
                                                                </FieldLabel>
                                                                <DnsLineTreeSelect
                                                                    id={`node-line-${endpointField.fieldKey}`}
                                                                    value={field.value}
                                                                    loading={formDnsLines.isLoading}
                                                                    lines={formDnsLines.data ?? []}
                                                                    onChange={(lineCode) =>
                                                                        field.onChange(
                                                                            lineCode ?? ''
                                                                        )
                                                                    }
                                                                    invalid={fieldState.invalid}
                                                                />
                                                                <FieldError>
                                                                    {lineError?.message}
                                                                </FieldError>
                                                            </Field>
                                                        )}
                                                    />

                                                    <Button
                                                        type="button"
                                                        variant="ghost"
                                                        size="icon-sm"
                                                        className="text-destructive sm:mt-6"
                                                        disabled={endpointFields.fields.length <= 1}
                                                        aria-label={`移除 IP ${index + 1}`}
                                                        onClick={() => endpointFields.remove(index)}
                                                    >
                                                        <Trash2 />
                                                    </Button>
                                                </div>
                                            );
                                        })}
                                    </div>

                                    <Button
                                        type="button"
                                        variant="outline"
                                        size="sm"
                                        disabled={endpointFields.fields.length >= NODE_IP_MAX_COUNT}
                                        onClick={() =>
                                            endpointFields.append(createDefaultEndpoint())
                                        }
                                    >
                                        <Plus />
                                        再加一个 IP
                                    </Button>
                                </section>

                                <section className="border-t pt-5">
                                    <Controller
                                        control={form.control}
                                        name="status"
                                        render={({ field, fieldState }) => (
                                            <Field data-invalid={fieldState.invalid}>
                                                <FieldLabel>节点状态</FieldLabel>
                                                <Select
                                                    value={field.value}
                                                    onValueChange={field.onChange}
                                                >
                                                    <SelectTrigger
                                                        className="w-full"
                                                        aria-invalid={fieldState.invalid}
                                                    >
                                                        <SelectValue placeholder="选择节点状态" />
                                                    </SelectTrigger>
                                                    <SelectContent>
                                                        <SelectItem value="enabled">
                                                            启用
                                                        </SelectItem>
                                                        <SelectItem value="disabled">
                                                            停用
                                                        </SelectItem>
                                                    </SelectContent>
                                                </Select>
                                                <FieldDescription>
                                                    停用节点后，控制面将其视为离线并停止正常交付。
                                                </FieldDescription>
                                                <FieldError>{fieldState.error?.message}</FieldError>
                                            </Field>
                                        )}
                                    />
                                </section>

                                {editing && (
                                    <section className="space-y-4 border-t pt-5">
                                        <div>
                                            <h3 className="text-sm font-semibold">节点接入</h3>
                                            <p className="mt-1 text-xs text-muted-foreground">
                                                查看当前安装命令，或重置持久化节点凭据。
                                            </p>
                                        </div>
                                        {storedCredentials.isLoading && (
                                            <div className="flex items-center gap-2 text-sm text-muted-foreground">
                                                <Spinner />
                                                正在加载一键安装命令
                                            </div>
                                        )}
                                        {activeCredentials && (
                                            <NodeInstallScriptPanel
                                                credentials={activeCredentials}
                                                description="复制这一条脚本到目标服务器执行即可安装或重新接入。"
                                                onCopy={copyInstallScript}
                                            />
                                        )}
                                        {storedCredentials.isError && !activeCredentials && (
                                            <Alert className="border-amber-200 bg-amber-50 text-amber-950">
                                                <TriangleAlert className="text-amber-600" />
                                                <AlertTitle>当前节点凭据不可用</AlertTitle>
                                                <AlertDescription className="text-amber-800">
                                                    {storedCredentials.error instanceof Error
                                                        ? storedCredentials.error.message
                                                        : '请重置持久化 nodeId 与 secret。'}
                                                </AlertDescription>
                                            </Alert>
                                        )}
                                        <div className="flex flex-col gap-3 rounded-xl border border-amber-200 bg-amber-50/60 p-4 sm:flex-row sm:items-center sm:justify-between">
                                            <div>
                                                <p className="text-sm font-medium text-amber-950">
                                                    重置节点凭据
                                                </p>
                                                <p className="mt-1 text-xs leading-relaxed text-amber-800">
                                                    生成新的持久化 nodeId 与
                                                    secret，旧脚本立即失效。
                                                </p>
                                            </div>
                                            <Button
                                                type="button"
                                                variant="outline"
                                                disabled={
                                                    resetCredentials.isPending &&
                                                    resetCredentials.variables?.id === editing.id
                                                }
                                                className="border-amber-300 text-amber-900 hover:bg-amber-100"
                                                onClick={() =>
                                                    setConfirmation({
                                                        title: `重置「${editing.name}」的节点凭据？`,
                                                        content:
                                                            '会生成新的持久化 nodeId 与 secret，旧脚本立即失效。目标节点必须重新执行一键安装脚本。',
                                                        confirmText: '重置凭据',
                                                        onConfirm: async () => {
                                                            const result =
                                                                await resetCredentials.mutateAsync({
                                                                    id: editing.id,
                                                                    revision: editing.revision,
                                                                });
                                                            setCredentials(result);
                                                            setEditing((current) =>
                                                                current?.id === editing.id
                                                                    ? {
                                                                          ...current,
                                                                          revision: result.revision,
                                                                          registration_status:
                                                                              'pending',
                                                                          connection_status:
                                                                              'unregistered',
                                                                          last_heartbeat_at:
                                                                              undefined,
                                                                          applied_node_spec_revision: 0,
                                                                          active_release_id:
                                                                              undefined,
                                                                          active_manifest_digest:
                                                                              undefined,
                                                                      }
                                                                    : current
                                                            );
                                                        },
                                                    })
                                                }
                                            >
                                                {resetCredentials.isPending ? (
                                                    <LoaderCircle className="animate-spin" />
                                                ) : (
                                                    <KeyRound />
                                                )}
                                                重置凭据
                                            </Button>
                                        </div>
                                    </section>
                                )}

                                {editing && (
                                    <section className="space-y-4 border-t pt-5">
                                        <div>
                                            <h3 className="text-sm font-semibold text-destructive">
                                                危险操作
                                            </h3>
                                            <p className="mt-1 text-xs text-muted-foreground">
                                                删除操作不可撤销，并会使现有节点凭据失效。
                                            </p>
                                        </div>
                                        <div className="flex flex-col gap-3 rounded-xl border border-destructive/25 bg-destructive/5 p-4 sm:flex-row sm:items-center sm:justify-between">
                                            <div>
                                                <p className="text-sm font-medium text-destructive">
                                                    删除节点
                                                </p>
                                                <p className="mt-1 text-xs leading-relaxed text-muted-foreground">
                                                    删除后会从集群发布中移除该节点，节点凭据也会失效。
                                                </p>
                                            </div>
                                            <Button
                                                type="button"
                                                variant="destructive"
                                                disabled={removeNode.isPending}
                                                onClick={() =>
                                                    setConfirmation({
                                                        title: `删除节点「${editing.name}」？`,
                                                        danger: true,
                                                        confirmText: '删除',
                                                        onConfirm: async () => {
                                                            await removeNode.mutateAsync({
                                                                id: editing.id,
                                                                revision: editing.revision,
                                                            });
                                                            closeForm();
                                                        },
                                                    })
                                                }
                                            >
                                                {removeNode.isPending ? (
                                                    <LoaderCircle className="animate-spin" />
                                                ) : (
                                                    <Trash2 />
                                                )}
                                                删除节点
                                            </Button>
                                        </div>
                                    </section>
                                )}
                            </form>
                        )}
                    </div>

                    <SheetFooter className="shrink-0 flex-row justify-end border-t px-4 py-4 sm:px-6">
                        <Button
                            variant="outline"
                            disabled={saveNodeMutation.isPending}
                            onClick={requestCloseForm}
                        >
                            <X />
                            {credentials && !editing ? '关闭' : '取消'}
                        </Button>
                        {(!credentials || editing) && (
                            <Button disabled={saveNodeMutation.isPending} onClick={submitNodeForm}>
                                {saveNodeMutation.isPending ? (
                                    <LoaderCircle className="animate-spin" />
                                ) : (
                                    <Save />
                                )}
                                保存
                            </Button>
                        )}
                    </SheetFooter>
                </SheetContent>
            </Sheet>

            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </div>
    );
}
