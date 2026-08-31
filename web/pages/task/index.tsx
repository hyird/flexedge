import { ListTodo, RefreshCw, TriangleAlert } from 'lucide-react';
import { useEffect, useState } from 'react';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { PaginationBar } from '@/components/pagination_bar';
import { StatusBadge } from '@/components/status_badge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import {
    Sheet,
    SheetContent,
    SheetDescription,
    SheetHeader,
    SheetTitle,
} from '@/components/ui/sheet';
import { cn } from '@/lib/utils';
import { formatDateTime } from '@/utils/date';
import { useTaskList } from './task.service';
import type { TaskItem, TaskQuery, TaskStatus } from './task.types';

type StatusTone = 'success' | 'warning' | 'destructive' | 'info' | 'neutral';

const statusMeta: Record<TaskStatus, { tone: StatusTone; label: string; pulse?: boolean }> = {
    pending: { tone: 'info', label: '待同步', pulse: true },
    running: { tone: 'info', label: '同步中', pulse: true },
    retry: { tone: 'warning', label: '等待重试', pulse: true },
    completed: { tone: 'success', label: '已完成' },
};

function operationLabel(item: TaskItem) {
    if (item.kind === 'provider') return '检测供应商';
    if (item.kind === 'dns')
        return item.operation.startsWith('sync') ? '同步 DNS' : '删除 DNS 配置';
    if (item.kind === 'certificate') return item.operation === 'issue' ? '申请证书' : '重新签发';
    return item.operation === 'apply' ? '下发网站配置' : '删除网站配置';
}

function taskResult(item: TaskItem) {
    if (item.status === 'completed') return <span className="text-emerald-700">已完成</span>;
    if (item.error)
        return (
            <span
                className="block max-w-96 whitespace-normal break-words text-destructive"
                title={item.error}
            >
                {item.error}
            </span>
        );
    if (item.status === 'running') return '正在同步';
    if (item.status === 'retry') return `计划重试：${formatDateTime(item.next_attempt_at)}`;
    return '等待同步';
}

interface TaskTableProps {
    items: TaskItem[];
    loading: boolean;
}

function TaskTable({ items, loading }: TaskTableProps) {
    const columns: DataTableColumn<TaskItem>[] = [
        {
            key: 'sequence',
            header: '标记序号',
            className: 'tabular-nums',
            cell: (item) => item.sequence,
        },
        {
            key: 'resource',
            header: '资源',
            cell: (item) => (
                <span className="block max-w-56 truncate font-medium" title={item.resource_name}>
                    {item.resource_name}
                </span>
            ),
        },
        { key: 'operation', header: '操作', cell: operationLabel },
        {
            key: 'status',
            header: '状态',
            cell: (item) => {
                const meta = statusMeta[item.status];
                return (
                    <StatusBadge tone={meta.tone} pulse={meta.pulse}>
                        {meta.label}
                    </StatusBadge>
                );
            },
        },
        {
            key: 'updated',
            header: '最后更新',
            className: 'tabular-nums',
            cell: (item) => formatDateTime(item.updated_at),
        },
        { key: 'result', header: '结果', cell: taskResult },
    ];
    return (
        <DataTable
            columns={columns}
            data={items}
            getRowKey={(item) => item.id}
            loading={loading}
            emptyTitle="暂无同步标记"
            emptyDescription="资源发生变更或同步失败后会显示在这里"
            className="min-h-0 flex-1 rounded-none border-0"
            tableClassName="min-w-[980px]"
        />
    );
}

export default function TaskEntry() {
    const [open, setOpen] = useState(false);
    const [query, setQuery] = useState<TaskQuery>({ page: 1, pageSize: 10 });
    const tasks = useTaskList(query, true, open ? 2000 : 5000);
    const summary = tasks.data?.summary;
    const activeCount = (summary?.pending ?? 0) + (summary?.running ?? 0) + (summary?.retry ?? 0);
    const total = Object.values(summary ?? {}).reduce((sum, value) => sum + value, 0);
    const statusOptions: Array<{ label: string; count: number; value: 'all' | TaskStatus }> = [
        { label: '全部', count: total, value: 'all' },
        { label: '待同步', count: summary?.pending ?? 0, value: 'pending' },
        { label: '同步中', count: summary?.running ?? 0, value: 'running' },
        { label: '重试', count: summary?.retry ?? 0, value: 'retry' },
        { label: '完成', count: summary?.completed ?? 0, value: 'completed' },
    ];
    const selectedStatus = query.status ?? 'all';

    useEffect(() => {
        if (!tasks.data) return;
        const lastPage = Math.max(1, tasks.data.totalPages);
        if (Number(query.page) <= lastPage) return;
        setQuery((current) => ({ ...current, page: lastPage }));
    }, [query.page, tasks.data]);

    return (
        <>
            <div className="relative">
                <Button variant="ghost" size="sm" onClick={() => setOpen(true)}>
                    <ListTodo />
                    <span className="hidden sm:inline">同步状态</span>
                </Button>
                {activeCount > 0 && (
                    <span className="absolute -right-1 -top-1 flex min-w-4 items-center justify-center rounded-full bg-primary px-1 text-[10px] font-semibold leading-4 text-primary-foreground tabular-nums ring-2 ring-card">
                        {activeCount > 99 ? '99+' : activeCount}
                    </span>
                )}
            </div>

            <Sheet open={open} onOpenChange={setOpen}>
                <SheetContent
                    side="right"
                    className="w-full gap-0 p-0 sm:max-w-[min(74rem,calc(100vw-2rem))]"
                >
                    <SheetHeader className="border-b px-4 py-4 sm:px-6">
                        <SheetTitle className="flex items-center gap-2 text-lg">
                            <ListTodo className="size-5 text-primary" />
                            同步状态
                        </SheetTitle>
                        <SheetDescription>
                            查看资源当前版本的同步进度、重试状态和执行结果
                        </SheetDescription>
                    </SheetHeader>
                    <div className="flex min-h-0 flex-1 flex-col gap-3 p-4 sm:p-6">
                        <div className="flex shrink-0 flex-col gap-3 lg:flex-row lg:items-center lg:justify-between">
                            <fieldset className="flex min-w-0 gap-1 overflow-x-auto border-0 p-0 pb-1">
                                <legend className="sr-only">同步状态</legend>
                                {statusOptions.map((option) => {
                                    const selected = selectedStatus === option.value;
                                    return (
                                        <Button
                                            key={option.value}
                                            variant={selected ? 'default' : 'outline'}
                                            size="sm"
                                            className="shrink-0"
                                            aria-pressed={selected}
                                            onClick={() =>
                                                setQuery({
                                                    page: 1,
                                                    pageSize: query.pageSize,
                                                    status:
                                                        option.value === 'all'
                                                            ? undefined
                                                            : option.value,
                                                })
                                            }
                                        >
                                            {option.label}
                                            <span
                                                className={cn(
                                                    'rounded-full px-1.5 text-[11px] tabular-nums',
                                                    selected
                                                        ? 'bg-primary-foreground/15'
                                                        : 'bg-muted text-muted-foreground'
                                                )}
                                            >
                                                {option.count}
                                            </span>
                                        </Button>
                                    );
                                })}
                            </fieldset>
                            <Button
                                variant="outline"
                                size="sm"
                                disabled={tasks.isFetching}
                                onClick={() => tasks.refetch()}
                            >
                                <RefreshCw className={cn(tasks.isFetching && 'animate-spin')} />
                                刷新
                            </Button>
                        </div>

                        {tasks.isError && (
                            <Alert variant="destructive" className="shrink-0">
                                <TriangleAlert />
                                <AlertTitle>同步状态加载失败</AlertTitle>
                                <AlertDescription>请稍后重试。</AlertDescription>
                            </Alert>
                        )}

                        <div className="flex min-h-0 flex-1 flex-col overflow-hidden rounded-xl border bg-card">
                            <TaskTable items={tasks.data?.list ?? []} loading={tasks.isLoading} />
                            <PaginationBar
                                page={Number(query.page) || 1}
                                pageSize={Number(query.pageSize) || 10}
                                total={tasks.data?.total ?? 0}
                                onPageChange={(page) => setQuery({ ...query, page })}
                                onPageSizeChange={(pageSize) =>
                                    setQuery({ ...query, page: 1, pageSize })
                                }
                            />
                        </div>
                    </div>
                </SheetContent>
            </Sheet>
        </>
    );
}
