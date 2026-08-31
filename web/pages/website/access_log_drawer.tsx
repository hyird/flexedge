import { RefreshCw } from 'lucide-react';
import { useEffect, useMemo, useState } from 'react';
import { DescriptionList } from '@/components/description_list';
import { EmptyState } from '@/components/empty_state';
import { StatusBadge } from '@/components/status_badge';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import {
    Dialog,
    DialogContent,
    DialogDescription,
    DialogHeader,
    DialogTitle,
} from '@/components/ui/dialog';
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
    SheetHeader,
    SheetTitle,
} from '@/components/ui/sheet';
import { Skeleton } from '@/components/ui/skeleton';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { formatDateTime } from '@/utils/date';
import { LOG_TAIL_LIMIT_OPTIONS } from '@/utils/log_tail';
import { useWebsiteAccessLogs, useWebsiteDetail } from './website.service';
import type { WebsiteAccessLog, WebsiteAccessLogLimit } from './website.types';

interface WebsiteAccessLogDrawerProps {
    websiteId?: string;
    onClose: () => void;
}

const LOG_ROW_HEIGHT = 44;
const LOG_HEADER_HEIGHT = 36;
const LOG_OVERSCAN = 12;
const detailSkeletonKeys = ['detail-a', 'detail-b', 'detail-c', 'detail-d', 'detail-e', 'detail-f'];
const logSkeletonKeys = [
    'log-a',
    'log-b',
    'log-c',
    'log-d',
    'log-e',
    'log-f',
    'log-g',
    'log-h',
    'log-i',
    'log-j',
];

function formatBytes(value: number) {
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
    let amount = value;
    let unitIndex = 0;
    while (amount >= 1024 && unitIndex < units.length - 1) {
        amount /= 1024;
        unitIndex += 1;
    }
    return `${amount.toFixed(amount >= 100 || Number.isInteger(amount) ? 0 : 1)}${units[unitIndex]}`;
}

function accessLogUrl(log: WebsiteAccessLog) {
    const protocol = log.protocol === 'h2' ? 'https' : log.protocol || 'http';
    const target = log.target.startsWith('/') ? log.target : `/${log.target}`;
    return `${protocol}://${log.host}${target}`;
}

function accessLogProtocolText(log: WebsiteAccessLog) {
    return log.protocol === 'h2' ? 'HTTP/2.0' : 'HTTP/1.1';
}

function accessLogDuration(value: number) {
    if (!Number.isFinite(value)) return `${value}ms`;
    return `${Math.max(0, Math.round(value))}ms`;
}

function accessLogResponseBytes(value: number) {
    return value > 0 ? formatBytes(value) : '—';
}

function accessLogLine(log: WebsiteAccessLog) {
    const nodeName = log.node_name || log.node_id || '未知节点';
    const clientIp = log.client_ip || '—';
    return `[${nodeName}] ${clientIp} ${accessLogProtocolText(log)} ${log.method} ${log.status_code} ${accessLogResponseBytes(log.response_bytes)} ${accessLogDuration(log.duration_ms)} ${accessLogUrl(log)}`;
}

function accessLogStatusText(statusCode: number) {
    if (statusCode >= 200 && statusCode < 300) return `${statusCode} OK`;
    return String(statusCode);
}

function accessLogCookie(log: WebsiteAccessLog) {
    if (log.cookies) return log.cookies;
    const match = log.request_headers?.match(/^cookie:\s*(.+)$/im);
    return match?.[1];
}

function accessLogLimit(value: string) {
    return LOG_TAIL_LIMIT_OPTIONS.find((option) => String(option.value) === value)?.value;
}

function LogText({ value }: { value?: string }) {
    return (
        <pre className="max-h-72 overflow-auto whitespace-pre-wrap break-all rounded-lg bg-muted/40 p-3 font-mono text-xs leading-relaxed">
            {value || '—'}
        </pre>
    );
}

function LogDetailDialog({ log, onClose }: { log?: WebsiteAccessLog; onClose: () => void }) {
    const cookie = log ? accessLogCookie(log) : undefined;

    return (
        <Dialog open={Boolean(log)} onOpenChange={(open) => !open && onClose()}>
            <DialogContent className="max-h-[90vh] overflow-hidden p-0 sm:max-w-4xl">
                <DialogHeader className="border-b px-5 py-4 pr-12">
                    <DialogTitle>访问日志详情</DialogTitle>
                    <DialogDescription className="truncate font-mono">
                        {log ? accessLogUrl(log) : '请求详情'}
                    </DialogDescription>
                </DialogHeader>
                {log && (
                    <Tabs defaultValue="summary" className="min-h-0 gap-0 overflow-hidden">
                        <div className="overflow-x-auto border-b px-4">
                            <TabsList variant="line" className="h-11 min-w-max">
                                <TabsTrigger value="summary">综合信息</TabsTrigger>
                                <TabsTrigger value="request">请求数据</TabsTrigger>
                                <TabsTrigger value="response">响应数据</TabsTrigger>
                                <TabsTrigger value="cookie">Cookie</TabsTrigger>
                                <TabsTrigger value="client">终端信息</TabsTrigger>
                            </TabsList>
                        </div>
                        <div className="max-h-[calc(90vh-9rem)] overflow-y-auto p-4 sm:p-5">
                            <TabsContent value="summary">
                                <DescriptionList
                                    items={[
                                        { label: '请求 ID', value: log.id },
                                        {
                                            label: '本地时间 (TimeLocal)',
                                            value: formatDateTime(log.occurred_at),
                                        },
                                        {
                                            label: '节点',
                                            value: log.node_name || log.node_id || '—',
                                        },
                                    ]}
                                />
                            </TabsContent>
                            <TabsContent value="request" className="space-y-4">
                                <DescriptionList
                                    items={[
                                        { label: '请求方法 (RequestMethod)', value: log.method },
                                        {
                                            label: '请求 URI (RequestURI)',
                                            value: log.target || '/',
                                        },
                                        {
                                            label: '参数列表 (QueryString)',
                                            value: log.query_string || '—',
                                        },
                                        { label: '主机地址 (Host)', value: log.host || '—' },
                                        {
                                            label: '请求来源 (Referer)',
                                            value: log.referer || '—',
                                        },
                                    ]}
                                />
                                <Card className="gap-3">
                                    <CardHeader>
                                        <CardTitle className="text-sm">请求头</CardTitle>
                                    </CardHeader>
                                    <CardContent>
                                        <LogText value={log.request_headers} />
                                    </CardContent>
                                </Card>
                                <Card className="gap-3">
                                    <CardHeader>
                                        <CardTitle className="text-sm">
                                            请求 Body
                                            {log.request_body_truncated ? '（已截断）' : ''}
                                        </CardTitle>
                                    </CardHeader>
                                    <CardContent>
                                        <LogText value={log.request_body} />
                                    </CardContent>
                                </Card>
                            </TabsContent>
                            <TabsContent value="response" className="space-y-4">
                                <DescriptionList
                                    items={[
                                        {
                                            label: '协议 (Protocol)',
                                            value: accessLogProtocolText(log),
                                        },
                                        {
                                            label: '状态 (StatusMessage)',
                                            value: accessLogStatusText(log.status_code),
                                        },
                                        {
                                            label: '下行流量 (BytesSent)',
                                            value: accessLogResponseBytes(log.response_bytes),
                                        },
                                        {
                                            label: '耗时 (Duration)',
                                            value: accessLogDuration(log.duration_ms),
                                        },
                                    ]}
                                />
                                <Card className="gap-3">
                                    <CardHeader>
                                        <CardTitle className="text-sm">响应头</CardTitle>
                                    </CardHeader>
                                    <CardContent>
                                        <LogText value={log.response_headers} />
                                    </CardContent>
                                </Card>
                            </TabsContent>
                            <TabsContent value="cookie">
                                <LogText value={cookie} />
                            </TabsContent>
                            <TabsContent value="client">
                                <DescriptionList
                                    items={[
                                        {
                                            label: '终端地址 (RemoteAddr)',
                                            value: log.client_ip || '—',
                                        },
                                        {
                                            label: '终端信息 (UserAgent)',
                                            value: log.user_agent || '—',
                                        },
                                        {
                                            label: 'TLS 指纹',
                                            value: log.tls_fingerprint || '—',
                                        },
                                    ]}
                                />
                            </TabsContent>
                        </div>
                    </Tabs>
                )}
            </DialogContent>
        </Dialog>
    );
}

export default function WebsiteAccessLogDrawer({
    websiteId,
    onClose,
}: WebsiteAccessLogDrawerProps) {
    const [limit, setLimit] = useState<WebsiteAccessLogLimit>(100);
    const [viewportHeight, setViewportHeight] = useState(320);
    const [viewport, setViewport] = useState<HTMLDivElement | null>(null);
    const [scrollTop, setScrollTop] = useState(0);
    const [selectedLog, setSelectedLog] = useState<WebsiteAccessLog>();
    const detail = useWebsiteDetail(websiteId, Boolean(websiteId));
    const logs = useWebsiteAccessLogs(websiteId, limit, Boolean(websiteId));
    const website = detail.data;
    const rows = logs.data?.list ?? [];

    useEffect(() => {
        if (!viewport) return;
        const updateHeight = () =>
            setViewportHeight(Math.max(240, Math.floor(viewport.getBoundingClientRect().height)));
        updateHeight();
        const observer = new ResizeObserver(updateHeight);
        observer.observe(viewport);
        return () => observer.disconnect();
    }, [viewport]);

    const visibleWindow = useMemo(() => {
        const virtualScrollTop = Math.max(0, scrollTop - LOG_HEADER_HEIGHT);
        const rowViewportHeight = Math.max(LOG_ROW_HEIGHT, viewportHeight - LOG_HEADER_HEIGHT);
        const start = Math.max(0, Math.floor(virtualScrollTop / LOG_ROW_HEIGHT) - LOG_OVERSCAN);
        const count = Math.ceil(rowViewportHeight / LOG_ROW_HEIGHT) + LOG_OVERSCAN * 2;
        return {
            start,
            rows: rows.slice(start, Math.min(rows.length, start + count)),
        };
    }, [rows, scrollTop, viewportHeight]);

    const close = () => {
        setLimit(100);
        setScrollTop(0);
        setSelectedLog(undefined);
        onClose();
    };

    return (
        <>
            <Sheet open={websiteId !== undefined} onOpenChange={(open) => !open && close()}>
                <SheetContent className="w-full gap-0 p-0 sm:max-w-[min(1280px,94vw)]">
                    <SheetHeader className="border-b px-5 py-4 pr-12">
                        <SheetTitle>
                            {website ? `访问日志 · ${website.website_name}` : '访问日志'}
                        </SheetTitle>
                        <SheetDescription>
                            实时查看节点上报的请求记录，点击任意一行查看完整上下文
                        </SheetDescription>
                    </SheetHeader>
                    {detail.isLoading ? (
                        <div className="space-y-2 p-5">
                            {detailSkeletonKeys.map((key) => (
                                <Skeleton key={key} className="h-10 w-full" />
                            ))}
                        </div>
                    ) : website ? (
                        <div className="flex min-h-0 flex-1 flex-col">
                            <div className="flex flex-col gap-3 border-b bg-muted/20 px-4 py-3 sm:flex-row sm:items-center sm:justify-between">
                                <div className="flex items-center gap-3">
                                    <StatusBadge tone="info" pulse>
                                        实时日志
                                    </StatusBadge>
                                    <span className="text-xs text-muted-foreground tabular-nums">
                                        {rows.length} 条
                                    </span>
                                </div>
                                <div className="flex items-center gap-2">
                                    <Select
                                        value={String(limit)}
                                        onValueChange={(value) => {
                                            const nextLimit = accessLogLimit(value);
                                            if (!nextLimit) return;
                                            setLimit(nextLimit);
                                            setScrollTop(0);
                                            viewport?.scrollTo({ top: 0 });
                                        }}
                                    >
                                        <SelectTrigger size="sm" className="w-40 shrink-0">
                                            <SelectValue />
                                        </SelectTrigger>
                                        <SelectContent>
                                            {LOG_TAIL_LIMIT_OPTIONS.map((option) => (
                                                <SelectItem
                                                    key={option.value}
                                                    value={String(option.value)}
                                                >
                                                    最近 {option.label} 条
                                                </SelectItem>
                                            ))}
                                        </SelectContent>
                                    </Select>
                                    <Button
                                        variant="outline"
                                        size="icon-sm"
                                        disabled={logs.isFetching}
                                        title="刷新"
                                        aria-label="刷新"
                                        onClick={() => logs.refetch()}
                                    >
                                        <RefreshCw
                                            className={logs.isFetching ? 'animate-spin' : undefined}
                                        />
                                    </Button>
                                </div>
                            </div>
                            <div
                                ref={setViewport}
                                className="relative min-h-0 flex-1 overflow-auto"
                                onScroll={(event) => setScrollTop(event.currentTarget.scrollTop)}
                            >
                                <div className="min-w-[880px]">
                                    <div
                                        className="sticky top-0 z-10 grid grid-cols-[max-content_minmax(0,1fr)] items-center border-b bg-muted px-3 text-xs font-medium text-muted-foreground"
                                        style={{ height: LOG_HEADER_HEIGHT }}
                                    >
                                        <span>时间</span>
                                        <span>日志</span>
                                    </div>
                                    {logs.isLoading ? (
                                        <div className="space-y-1 p-3">
                                            {logSkeletonKeys.map((key) => (
                                                <Skeleton key={key} className="h-10 w-full" />
                                            ))}
                                        </div>
                                    ) : rows.length === 0 ? (
                                        <div
                                            style={{
                                                minHeight: Math.max(
                                                    0,
                                                    viewportHeight - LOG_HEADER_HEIGHT
                                                ),
                                            }}
                                        >
                                            <EmptyState
                                                title="暂无访问日志"
                                                description="新请求到达并由节点上报后会实时显示在这里"
                                                className="h-full"
                                            />
                                        </div>
                                    ) : (
                                        <div
                                            className="relative"
                                            style={{ height: rows.length * LOG_ROW_HEIGHT }}
                                        >
                                            <div
                                                className="absolute inset-x-0 top-0"
                                                style={{
                                                    transform: `translateY(${visibleWindow.start * LOG_ROW_HEIGHT}px)`,
                                                }}
                                            >
                                                {visibleWindow.rows.map((log) => {
                                                    const line = accessLogLine(log);
                                                    return (
                                                        <button
                                                            key={log.id}
                                                            type="button"
                                                            title={line}
                                                            className="grid w-full grid-cols-[max-content_minmax(0,1fr)] items-center border-b px-3 text-left text-xs hover:bg-accent focus-visible:bg-accent focus-visible:outline-none"
                                                            style={{ height: LOG_ROW_HEIGHT }}
                                                            onClick={() => setSelectedLog(log)}
                                                        >
                                                            <span className="text-muted-foreground tabular-nums">
                                                                {formatDateTime(log.occurred_at)}
                                                            </span>
                                                            <span className="truncate font-mono">
                                                                {line}
                                                            </span>
                                                        </button>
                                                    );
                                                })}
                                            </div>
                                        </div>
                                    )}
                                </div>
                            </div>
                        </div>
                    ) : (
                        <EmptyState title="网站不存在或已删除" className="flex-1" />
                    )}
                </SheetContent>
            </Sheet>
            <LogDetailDialog log={selectedLog} onClose={() => setSelectedLog(undefined)} />
        </>
    );
}
