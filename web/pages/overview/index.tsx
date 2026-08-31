import type { LucideIcon } from 'lucide-react';
import {
    Activity,
    CheckCircle2,
    Cloud,
    Globe2,
    RefreshCw,
    Server,
    ShieldCheck,
    TriangleAlert,
} from 'lucide-react';
import { useNavigate } from 'react-router-dom';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { PageHeader } from '@/components/page_header';
import { StatusBadge } from '@/components/status_badge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Skeleton } from '@/components/ui/skeleton';
import { cn } from '@/lib/utils';
import { formatDateTime } from '@/utils/date';
import { useOverview } from './overview.service';
import type { OverviewTask } from './overview.types';

type StatusTone = 'success' | 'warning' | 'destructive' | 'info' | 'neutral';

interface ResourceCardProps {
    title: string;
    description: string;
    value?: number;
    icon: LucideIcon;
    iconClassName: string;
    loading: boolean;
    onClick: () => void;
}

function ResourceCard({
    title,
    description,
    value,
    icon: Icon,
    iconClassName,
    loading,
    onClick,
}: ResourceCardProps) {
    return (
        <Card className="gap-0 overflow-hidden transition-shadow hover:shadow-md">
            <button
                type="button"
                className="flex w-full items-center justify-between gap-4 rounded-xl p-5 text-left outline-none focus-visible:ring-[3px] focus-visible:ring-ring/50"
                onClick={onClick}
            >
                <div className="min-w-0">
                    <p className="text-sm font-medium text-muted-foreground">{title}</p>
                    {loading ? (
                        <Skeleton className="mt-2 h-8 w-20" />
                    ) : (
                        <p className="mt-1 text-3xl font-semibold tracking-tight tabular-nums">
                            {value ?? 0}
                        </p>
                    )}
                    <p className="mt-1 truncate text-xs text-muted-foreground/80">{description}</p>
                </div>
                <span
                    className={cn(
                        'flex size-11 shrink-0 items-center justify-center rounded-xl',
                        iconClassName
                    )}
                >
                    <Icon className="size-5" />
                </span>
            </button>
        </Card>
    );
}

const taskStatusMeta: Record<
    OverviewTask['status'],
    { tone: StatusTone; label: string; pulse?: boolean }
> = {
    pending: { tone: 'info', label: '待同步', pulse: true },
    running: { tone: 'info', label: '同步中', pulse: true },
    retry: { tone: 'warning', label: '等待重试', pulse: true },
    completed: { tone: 'success', label: '已完成' },
};

function taskOperationLabel(task: OverviewTask) {
    if (task.kind === 'provider') return '检测供应商';
    if (task.kind === 'dns') return task.operation.startsWith('sync') ? '同步 DNS' : '删除 DNS';
    if (task.kind === 'certificate') return task.operation === 'issue' ? '签发证书' : '续签证书';
    return task.operation === 'apply' ? '下发网站配置' : '删除网站配置';
}

function resourcePath(task: OverviewTask) {
    if (task.resource_type === 'website') return '/websites';
    if (task.resource_type === 'certificate') return '/certificates';
    if (task.resource_type === 'dns_zone') return '/dns-zones';
    return undefined;
}

interface IssueRowProps {
    message: string;
    onClick?: () => void;
}

function IssueRow({ message, onClick }: IssueRowProps) {
    return (
        <div className="flex items-center gap-3 rounded-lg border border-destructive/20 bg-destructive/5 p-3">
            <TriangleAlert className="size-4 shrink-0 text-destructive" />
            <p className="min-w-0 flex-1 text-sm">{message}</p>
            {onClick && (
                <Button variant="outline" size="xs" onClick={onClick}>
                    查看
                </Button>
            )}
        </div>
    );
}

export default function OverviewPage() {
    const navigate = useNavigate();
    const overview = useOverview();
    const resources = overview.data?.resources;
    const issues = overview.data?.issues;
    const issueCount =
        (issues?.dns_zone_issue_count ?? 0) +
        (issues?.certificate_expiring_count ?? 0) +
        (issues?.certificate_failed_count ?? 0) +
        (issues?.failed_task_count ?? 0);
    const taskColumns: DataTableColumn<OverviewTask>[] = [
        {
            key: 'resource',
            header: '同步对象',
            cell: (task) => {
                const path = resourcePath(task);
                return path ? (
                    <Button
                        variant="link"
                        size="sm"
                        className="h-auto max-w-52 justify-start p-0"
                        onClick={() => navigate(path)}
                    >
                        <span className="truncate" title={task.resource_name}>
                            {task.resource_name}
                        </span>
                    </Button>
                ) : (
                    <span className="block max-w-52 truncate" title={task.resource_name}>
                        {task.resource_name}
                    </span>
                );
            },
        },
        {
            key: 'operation',
            header: '操作',
            cell: (task) => taskOperationLabel(task),
        },
        {
            key: 'status',
            header: '状态',
            cell: (task) => {
                const meta = taskStatusMeta[task.status];
                return (
                    <StatusBadge tone={meta.tone} pulse={meta.pulse}>
                        {meta.label}
                    </StatusBadge>
                );
            },
        },
        {
            key: 'error',
            header: '错误信息',
            cell: (task) => (
                <span
                    className={cn(
                        'block max-w-96 whitespace-normal break-words',
                        task.last_error && 'text-destructive'
                    )}
                    title={task.last_error}
                >
                    {task.last_error || '—'}
                </span>
            ),
        },
        {
            key: 'updated',
            header: '最后更新',
            cell: (task) => (
                <span className="whitespace-nowrap tabular-nums">
                    {formatDateTime(task.updated_at)}
                </span>
            ),
        },
    ];

    return (
        <div className="mx-auto flex h-full w-full max-w-[1680px] flex-col gap-6 overflow-auto p-4 sm:p-6">
            <PageHeader
                eyebrow="Dashboard"
                title="首页概览"
                description="查看控制面资源、异常状态和待处理同步"
                actions={
                    <Button
                        variant="outline"
                        disabled={overview.isFetching}
                        onClick={() => overview.refetch()}
                    >
                        <RefreshCw className={cn(overview.isFetching && 'animate-spin')} />
                        刷新
                    </Button>
                }
            />

            {overview.isError && (
                <Alert variant="destructive">
                    <TriangleAlert />
                    <AlertTitle>概览数据加载失败</AlertTitle>
                    <AlertDescription>请刷新页面重试。</AlertDescription>
                </Alert>
            )}

            <div className="grid grid-cols-1 gap-4 sm:grid-cols-2 xl:grid-cols-4">
                <ResourceCard
                    title="网站"
                    description="已接入的加速站点"
                    value={resources?.website_count}
                    icon={Cloud}
                    iconClassName="bg-blue-500/10 text-blue-600"
                    loading={overview.isLoading}
                    onClick={() => navigate('/websites')}
                />
                <ResourceCard
                    title="绑定域名"
                    description="当前服务域名总数"
                    value={resources?.domain_count}
                    icon={Globe2}
                    iconClassName="bg-cyan-500/10 text-cyan-600"
                    loading={overview.isLoading}
                    onClick={() => navigate('/websites')}
                />
                <ResourceCard
                    title="证书"
                    description="托管的 TLS 证书"
                    value={resources?.certificate_count}
                    icon={ShieldCheck}
                    iconClassName="bg-emerald-500/10 text-emerald-600"
                    loading={overview.isLoading}
                    onClick={() => navigate('/certificates')}
                />
                <ResourceCard
                    title="集群"
                    description="边缘交付集群"
                    value={resources?.cluster_count}
                    icon={Server}
                    iconClassName="bg-violet-500/10 text-violet-600"
                    loading={overview.isLoading}
                    onClick={() => navigate('/clusters')}
                />
            </div>

            <div className="grid grid-cols-1 gap-4 xl:grid-cols-[minmax(320px,0.8fr)_minmax(0,2fr)]">
                <Card>
                    <CardHeader>
                        <CardTitle>状态检查</CardTitle>
                        <CardDescription>需要管理员关注的控制面状态</CardDescription>
                    </CardHeader>
                    <CardContent className="space-y-3">
                        {overview.isLoading ? (
                            <div className="space-y-3">
                                <Skeleton className="h-16 w-full" />
                                <Skeleton className="h-12 w-full" />
                            </div>
                        ) : issueCount === 0 ? (
                            <div className="flex gap-3 rounded-lg border border-emerald-200 bg-emerald-50 p-3 text-emerald-800">
                                <CheckCircle2 className="mt-0.5 size-4 shrink-0" />
                                <div>
                                    <p className="text-sm font-medium">控制面状态正常</p>
                                    <p className="mt-0.5 text-xs text-emerald-700">
                                        没有发现 DNS、证书或同步异常
                                    </p>
                                </div>
                            </div>
                        ) : (
                            <div className="space-y-2">
                                {Boolean(issues?.dns_zone_issue_count) && (
                                    <IssueRow
                                        message={`${issues?.dns_zone_issue_count} 个 DNS 托管域名同步异常`}
                                        onClick={() => navigate('/dns-zones')}
                                    />
                                )}
                                {Boolean(issues?.certificate_expiring_count) && (
                                    <IssueRow
                                        message={`${issues?.certificate_expiring_count} 张证书将在 30 天内到期`}
                                        onClick={() => navigate('/certificates')}
                                    />
                                )}
                                {Boolean(issues?.certificate_failed_count) && (
                                    <IssueRow
                                        message={`${issues?.certificate_failed_count} 张证书签发、续签失败或已过期`}
                                        onClick={() => navigate('/certificates')}
                                    />
                                )}
                                {Boolean(issues?.failed_task_count) && (
                                    <IssueRow
                                        message={`${issues?.failed_task_count} 个同步标记执行失败`}
                                    />
                                )}
                            </div>
                        )}
                        <div className="flex items-center justify-between border-t pt-3">
                            <div className="flex items-center gap-2 text-sm text-muted-foreground">
                                <Activity
                                    className={cn(
                                        'size-4',
                                        Boolean(issues?.active_task_count) &&
                                            'animate-pulse text-primary'
                                    )}
                                />
                                正在处理的同步
                            </div>
                            <span className="text-sm font-semibold tabular-nums">
                                {issues?.active_task_count ?? 0}
                            </span>
                        </div>
                    </CardContent>
                </Card>

                <Card className="min-w-0 overflow-hidden">
                    <CardHeader>
                        <CardTitle>待处理同步</CardTitle>
                        <CardDescription>最近需要执行或关注的资源同步</CardDescription>
                    </CardHeader>
                    <CardContent className="p-0">
                        <DataTable
                            columns={taskColumns}
                            data={overview.data?.recent_tasks}
                            getRowKey={(task) => task.id}
                            loading={overview.isLoading}
                            emptyTitle="暂无待处理同步"
                            emptyDescription="资源发生变更后会显示在这里"
                            className="rounded-none border-x-0 border-b-0"
                        />
                    </CardContent>
                </Card>
            </div>
        </div>
    );
}
