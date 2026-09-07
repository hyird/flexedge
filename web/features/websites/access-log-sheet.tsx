import { useDeferredValue, useEffect, useMemo, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { getData, type ApiEnvelope } from '@/lib/api'
import { formatBytes, formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import type { PageData } from '@/lib/types'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  Collapsible,
  CollapsibleContent,
  CollapsibleTrigger,
} from '@/components/ui/collapsible'
import { Input } from '@/components/ui/input'
import { ScrollArea } from '@/components/ui/scroll-area'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { Skeleton } from '@/components/ui/skeleton'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import type { Website } from './types'

type AccessLog = {
  id: string
  occurred_at: string
  node_id: string
  node_name: string
  client_ip?: string
  client_ip_location?: string
  protocol: string
  method: string
  host: string
  target: string
  status_code: number
  request_bytes: number
  response_bytes: number
  duration_ms: number
  user_agent?: string
  referer?: string
  request_headers?: string
  request_body?: string
  request_body_truncated: boolean
  response_headers?: string
  query_string?: string
  cookies?: string
  tls_fingerprint?: string
}

function accessLogStatusLabel(status: number) {
  if (status >= 500) return '服务器错误'
  if (status >= 400) return '客户端错误'
  if (status >= 300) return '重定向'
  if (status >= 200) return '成功'
  return '信息响应'
}

function accessLogStatusClass(status: number) {
  if (status >= 500)
    return 'border-destructive/30 bg-destructive/10 text-destructive'
  if (status >= 400)
    return 'border-amber-500/30 bg-amber-500/10 text-amber-700 dark:text-amber-400'
  if (status >= 300)
    return 'border-sky-500/30 bg-sky-500/10 text-sky-700 dark:text-sky-400'
  return 'border-emerald-500/30 bg-emerald-500/10 text-emerald-700 dark:text-emerald-400'
}

function formatAccessLogDuration(value?: number) {
  if (value === undefined || !Number.isFinite(value)) return '—'
  const milliseconds = Math.max(0, value)
  if (milliseconds < 1000) return `${milliseconds.toFixed(0)} ms`
  return `${(milliseconds / 1000).toFixed(1)} s`
}

function accessLogUrlScheme(protocol: string) {
  const normalized = protocol.trim().toLowerCase()
  return normalized === 'https' || normalized === 'h2' || normalized === 'h3'
    ? 'https'
    : 'http'
}

function accessLogHttpVersion(protocol: string) {
  const normalized = protocol.trim().toLowerCase()
  if (normalized === 'h2') return 'HTTP/2'
  if (normalized === 'h3') return 'HTTP/3'
  if (
    normalized === 'http' ||
    normalized === 'https' ||
    normalized === 'http/1.1' ||
    normalized === 'https/1.1'
  ) {
    return 'HTTP/1.1'
  }
  if (normalized === 'http/1.0' || normalized === 'https/1.0') return 'HTTP/1.0'
  return normalized.startsWith('http/') ? normalized.toUpperCase() : '未知协议'
}

const accessLogMethods = [
  'all',
  'GET',
  'POST',
  'PUT',
  'PATCH',
  'DELETE',
  'HEAD',
  'OPTIONS',
] as const
const accessLogStatusClasses = [
  'all',
  '1xx',
  '2xx',
  '3xx',
  '4xx',
  '5xx',
] as const

type AccessLogMethod = (typeof accessLogMethods)[number]
type AccessLogStatusClass = (typeof accessLogStatusClasses)[number]
type AccessLogView = 'live' | 'history'

function accessLogMatches(
  log: AccessLog,
  keyword: string,
  method: AccessLogMethod,
  statusClass: AccessLogStatusClass
) {
  if (method !== 'all' && log.method !== method) return false
  if (
    statusClass !== 'all' &&
    !String(log.status_code).startsWith(statusClass[0])
  ) {
    return false
  }
  const normalizedKeyword = keyword.trim().toLocaleLowerCase()
  if (!normalizedKeyword) return true
  return [
    log.method,
    log.host,
    log.target,
    log.node_name,
    log.client_ip,
    log.client_ip_location,
    log.protocol,
    String(log.status_code),
  ]
    .filter(Boolean)
    .join(' ')
    .toLocaleLowerCase()
    .includes(normalizedKeyword)
}

function AccessLogFilters({
  keyword,
  method,
  statusClass,
  onKeywordChange,
  onMethodChange,
  onStatusClassChange,
}: {
  keyword: string
  method: AccessLogMethod
  statusClass: AccessLogStatusClass
  onKeywordChange: (value: string) => void
  onMethodChange: (value: AccessLogMethod) => void
  onStatusClassChange: (value: AccessLogStatusClass) => void
}) {
  return (
    <div className='flex flex-col gap-2 px-4 sm:flex-row'>
      <Input
        value={keyword}
        onChange={(event) => onKeywordChange(event.target.value)}
        placeholder='搜索域名、路径、IP、节点或状态码'
        aria-label='搜索访问日志'
        className='h-9 sm:max-w-sm'
      />
      <div className='grid grid-cols-2 gap-2 sm:flex'>
        <Select
          value={method}
          onValueChange={(value) => onMethodChange(value as AccessLogMethod)}
        >
          <SelectTrigger
            className='h-9 w-full sm:w-28'
            aria-label='按请求方法筛选'
          >
            <SelectValue placeholder='请求方法' />
          </SelectTrigger>
          <SelectContent>
            {accessLogMethods.map((item) => (
              <SelectItem key={item} value={item}>
                {item === 'all' ? '全部方法' : item}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
        <Select
          value={statusClass}
          onValueChange={(value) =>
            onStatusClassChange(value as AccessLogStatusClass)
          }
        >
          <SelectTrigger
            className='h-9 w-full sm:w-28'
            aria-label='按状态码筛选'
          >
            <SelectValue placeholder='状态码' />
          </SelectTrigger>
          <SelectContent>
            {accessLogStatusClasses.map((item) => (
              <SelectItem key={item} value={item}>
                {item === 'all' ? '全部状态' : item}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>
      </div>
    </div>
  )
}

function AccessLogList({
  logs,
  emptyMessage,
  label,
}: {
  logs: AccessLog[]
  emptyMessage: string
  label: string
}) {
  return (
    <div className='bg-card font-mono text-xs' role='region' aria-label={label}>
      {logs.map((log) => {
        const request = `${accessLogUrlScheme(log.protocol)}://${log.host}${log.target}`
        const statusLabel = accessLogStatusLabel(log.status_code)
        return (
          <Collapsible
            key={log.id}
            className='grid grid-cols-[minmax(0,1fr)_auto_auto] gap-x-3 gap-y-0 border-b px-3 py-2 last:border-b-0 hover:bg-muted/50'
          >
            <div className='flex min-w-0 items-center gap-2'>
              <Badge
                variant='outline'
                className={accessLogStatusClass(log.status_code)}
                aria-label={`响应状态 ${log.status_code}，${statusLabel}`}
                title={statusLabel}
              >
                {log.status_code}
              </Badge>
              <span className='font-semibold text-foreground'>
                {log.method}
              </span>
              <code
                title={request}
                className='min-w-0 truncate text-foreground'
              >
                {request}
              </code>
            </div>
            <div className='row-span-2 flex flex-col items-end gap-0 self-center text-end text-[11px] leading-4 text-muted-foreground tabular-nums'>
              <span title={`${log.duration_ms} 毫秒`} className='font-medium'>
                {formatAccessLogDuration(log.duration_ms)}
              </span>
              <span title={`请求体大小：${formatBytes(log.request_bytes)}`}>
                请求体 {formatBytes(log.request_bytes)}
              </span>
              <span title={`响应大小：${formatBytes(log.response_bytes)}`}>
                响应 {formatBytes(log.response_bytes)}
              </span>
            </div>
            <div className='col-start-1 min-w-0 overflow-x-auto text-muted-foreground'>
              <div className='flex min-w-max items-center gap-x-3 whitespace-nowrap'>
                <time dateTime={log.occurred_at}>
                  {formatDate(log.occurred_at)}
                </time>
                <span>[{log.node_name || '未知节点'}]</span>
                <span title='用户 IP'>{log.client_ip || '未知 IP'}</span>
                {log.client_ip_location && (
                  <span title={`IP 归属地：${log.client_ip_location}`}>
                    {log.client_ip_location.replaceAll(' · ', '')}
                  </span>
                )}
                <span>{accessLogHttpVersion(log.protocol)}</span>
              </div>
            </div>
            <CollapsibleTrigger asChild>
              <Button
                variant='ghost'
                size='sm'
                className='col-start-3 row-span-2 row-start-1 self-center'
                aria-label={`查看请求详情 ${log.method} ${log.target}`}
              >
                请求详情
              </Button>
            </CollapsibleTrigger>
            <CollapsibleContent className='col-span-3 mt-2 min-w-0 space-y-3 rounded-md border bg-muted/20 p-3'>
              <p className='font-sans text-muted-foreground'>
                仅展示已采集内容；未返回的字段可能未启用采集或为空。内容可能包含敏感信息，请勿直接分享。
              </p>
              {log.request_body_truncated && (
                <p className='font-sans text-destructive'>
                  请求体已截断，以下不是完整请求体。
                </p>
              )}
              {(
                [
                  ['请求头', log.request_headers],
                  ['请求体', log.request_body],
                  ['响应头', log.response_headers],
                  ['查询参数', log.query_string],
                  ['Cookie', log.cookies],
                  ['Referer', log.referer],
                  ['User-Agent', log.user_agent],
                  ['TLS 指纹', log.tls_fingerprint],
                ] as const
              ).map(([label, value]) => (
                <div key={label} className='min-w-0 space-y-1'>
                  <h4 className='font-sans font-medium'>{label}</h4>
                  <pre className='max-h-64 overflow-auto rounded-md border bg-background p-2 break-all whitespace-pre-wrap'>
                    {value || '未采集或为空'}
                  </pre>
                </div>
              ))}
            </CollapsibleContent>
          </Collapsible>
        )
      })}
      {!logs.length && (
        <p className='py-16 text-center text-muted-foreground'>
          {emptyMessage}
        </p>
      )}
    </div>
  )
}

export function AccessLogSheet({
  website,
  onOpenChange,
}: {
  website: Website
  onOpenChange: (open: boolean) => void
}) {
  const [logs, setLogs] = useState<AccessLog[]>([])
  const [connected, setConnected] = useState(false)
  const [view, setView] = useState<AccessLogView>('live')
  const [keyword, setKeyword] = useState('')
  const [method, setMethod] = useState<AccessLogMethod>('all')
  const [statusClass, setStatusClass] = useState<AccessLogStatusClass>('all')
  const [historyPage, setHistoryPage] = useState(1)
  const deferredKeyword = useDeferredValue(keyword.trim())

  useEffect(() => {
    if (view !== 'live') return

    const source = new EventSource(
      `/api/websites/${website.id}/access-logs/stream?limit=100`,
      { withCredentials: true }
    )
    source.addEventListener('ready', () => setConnected(true))
    source.addEventListener('logs', (event) => {
      const payload = JSON.parse(
        (event as MessageEvent<string>).data
      ) as ApiEnvelope<{
        list: AccessLog[]
      }>
      setLogs((current) => {
        const merged = [...payload.data.list, ...current]
        return Array.from(
          new Map(merged.map((item) => [item.id, item])).values()
        )
          .sort((a, b) => b.occurred_at.localeCompare(a.occurred_at))
          .slice(0, 1000)
      })
    })
    source.onerror = () => setConnected(false)
    return () => source.close()
  }, [view, website.id])

  const liveLogs = useMemo(
    () =>
      logs.filter((log) => accessLogMatches(log, keyword, method, statusClass)),
    [keyword, logs, method, statusClass]
  )
  const historyQuery = useQuery({
    queryKey: [
      ...queryKeys.websites,
      website.id,
      'access-logs',
      historyPage,
      deferredKeyword,
      method,
      statusClass,
    ],
    queryFn: () =>
      getData<PageData<AccessLog>>(`/websites/${website.id}/access-logs`, {
        page: historyPage,
        page_size: 50,
        keyword: deferredKeyword || undefined,
        method: method === 'all' ? undefined : method,
        status_class: statusClass === 'all' ? undefined : statusClass,
      }),
    enabled: view === 'history',
  })
  const history = historyQuery.data
  const changeFilters = (next: {
    keyword?: string
    method?: AccessLogMethod
    statusClass?: AccessLogStatusClass
  }) => {
    if (next.keyword !== undefined) setKeyword(next.keyword)
    if (next.method !== undefined) setMethod(next.method)
    if (next.statusClass !== undefined) setStatusClass(next.statusClass)
    setHistoryPage(1)
  }

  return (
    <Sheet open onOpenChange={onOpenChange}>
      <SheetContent className='flex w-full flex-col sm:max-w-5xl'>
        <SheetHeader className='text-start'>
          <div className='flex items-center gap-2'>
            <SheetTitle>{website.config.name} · 访问日志</SheetTitle>
            {view === 'live' && (
              <Badge variant='outline' role='status'>
                {connected ? '已连接' : '正在重连'}
              </Badge>
            )}
          </div>
          <SheetDescription>
            打开时载入最近 100 条请求，实时窗口最多保留 1000
            条；更早记录请在“历史日志”中检索。展开“请求详情”查看已采集的请求头和请求体等内容。
          </SheetDescription>
        </SheetHeader>
        <Tabs
          value={view}
          onValueChange={(value) => {
            setConnected(false)
            setView(value as AccessLogView)
          }}
          className='min-h-0 flex-1 gap-3'
        >
          <div className='flex flex-wrap items-center justify-between gap-2 px-4'>
            <TabsList>
              <TabsTrigger value='live'>实时日志</TabsTrigger>
              <TabsTrigger value='history'>历史日志</TabsTrigger>
            </TabsList>
            {view === 'live' && (
              <span className='text-xs text-muted-foreground'>
                显示 {liveLogs.length} / {logs.length} 条
              </span>
            )}
            {view === 'history' && history && (
              <span className='text-xs text-muted-foreground'>
                共 {history.total} 条
              </span>
            )}
          </div>
          <AccessLogFilters
            keyword={keyword}
            method={method}
            statusClass={statusClass}
            onKeywordChange={(value) => changeFilters({ keyword: value })}
            onMethodChange={(value) => changeFilters({ method: value })}
            onStatusClassChange={(value) =>
              changeFilters({ statusClass: value })
            }
          />
          <TabsContent
            value='live'
            className='mt-0 flex min-h-0 flex-1 flex-col'
          >
            <ScrollArea className='min-h-0 flex-1 px-4'>
              <AccessLogList
                logs={liveLogs}
                emptyMessage={
                  logs.length ? '没有符合筛选条件的实时日志' : '等待访问日志…'
                }
                label='实时访问日志列表'
              />
            </ScrollArea>
          </TabsContent>
          <TabsContent
            value='history'
            className='mt-0 flex min-h-0 flex-1 flex-col'
          >
            {historyQuery.isLoading ? (
              <div className='space-y-1 px-4' aria-label='正在加载历史访问日志'>
                <Skeleton className='h-16 w-full' />
                <Skeleton className='h-16 w-full' />
                <Skeleton className='h-16 w-full' />
              </div>
            ) : historyQuery.isError ? (
              <div className='flex flex-1 flex-col items-center justify-center gap-3 px-4 text-sm text-muted-foreground'>
                <p>历史访问日志加载失败。</p>
                <Button
                  variant='outline'
                  size='sm'
                  onClick={() => void historyQuery.refetch()}
                >
                  重试
                </Button>
              </div>
            ) : (
              <>
                <ScrollArea className='min-h-0 flex-1 px-4'>
                  <AccessLogList
                    logs={history?.list ?? []}
                    emptyMessage='没有符合筛选条件的历史日志'
                    label='历史访问日志列表'
                  />
                </ScrollArea>
                <div className='flex items-center justify-end gap-2 border-t px-4 py-2 text-xs text-muted-foreground'>
                  <Button
                    variant='outline'
                    size='sm'
                    disabled={
                      (history?.page ?? 1) <= 1 || historyQuery.isFetching
                    }
                    onClick={() =>
                      setHistoryPage((page) => Math.max(1, page - 1))
                    }
                  >
                    上一页
                  </Button>
                  <span>
                    {history?.page ?? 1} /{' '}
                    {Math.max(1, history?.total_pages ?? 1)}
                  </span>
                  <Button
                    variant='outline'
                    size='sm'
                    disabled={
                      !history ||
                      history.page >= history.total_pages ||
                      historyQuery.isFetching
                    }
                    onClick={() => setHistoryPage((page) => page + 1)}
                  >
                    下一页
                  </Button>
                </div>
              </>
            )}
          </TabsContent>
        </Tabs>
      </SheetContent>
    </Sheet>
  )
}
