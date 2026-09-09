import { type ReactNode } from 'react'
import { useMutation, useQuery } from '@tanstack/react-query'
import {
  Activity,
  Globe2,
  Network,
  RefreshCw,
  Server,
  ShieldCheck,
  type LucideIcon,
} from 'lucide-react'
import { toast } from 'sonner'
import { apiErrorMessage } from '@/lib/api'
import { formatBytes, formatBytesPerSecond } from '@/lib/format'
import { useDelayedLoading } from '@/hooks/use-delayed-loading'
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from '@/components/ui/card'
import { ScrollArea } from '@/components/ui/scroll-area'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'
import { Skeleton } from '@/components/ui/skeleton'
import {
  Tooltip,
  TooltipContent,
  TooltipTrigger,
} from '@/components/ui/tooltip'
import { StatusBadge } from '@/components/status-badge'
import type {
  WebsiteDashboardClientIpRanking,
  WebsiteDashboardRanking,
  WebsiteDashboardSeriesPoint,
} from './dashboard-schema'
import { websiteDetailQuery, probeWebsiteDns } from './data'
import type { Website } from './types'
import { useWebsiteDashboard } from './use-website-dashboard'
import { originGroupLabel } from './website-display'

function DashboardTooltip({
  children,
  content,
}: {
  children: ReactNode
  content: ReactNode
}) {
  return (
    <Tooltip>
      <TooltipTrigger asChild>{children}</TooltipTrigger>
      <TooltipContent
        sideOffset={6}
        className='max-w-[min(24rem,calc(100vw-2rem))]'
      >
        {content}
      </TooltipContent>
    </Tooltip>
  )
}

function WebsiteDashboardMetric({
  title,
  value,
  description,
  icon: Icon,
}: {
  title: string
  value: ReactNode
  description: string
  icon: LucideIcon
}) {
  return (
    <DashboardTooltip
      content={
        <div className='space-y-1.5'>
          <p className='font-medium'>{title}</p>
          <p className='text-primary-foreground/80'>{description}</p>
          <p>当前值：{value}</p>
        </div>
      }
    >
      <div className='min-w-0 cursor-default bg-card p-3'>
        <div className='flex items-center justify-between gap-2 text-xs text-muted-foreground'>
          <span className='truncate'>{title}</span>
          <Icon className='size-3.5 shrink-0' aria-hidden='true' />
        </div>
        <div className='mt-2 min-h-5 text-base font-semibold tabular-nums'>
          {value}
        </div>
        <p className='mt-0.5 truncate text-xs text-muted-foreground'>
          {description}
        </p>
      </div>
    </DashboardTooltip>
  )
}

function WebsiteDashboardTrend({
  title,
  description,
  data,
  value,
  format,
}: {
  title: string
  description: string
  data: WebsiteDashboardSeriesPoint[]
  value: (item: WebsiteDashboardSeriesPoint) => number
  format: (value: number) => string
}) {
  const maximum = Math.max(1, ...data.map(value))
  const hasData = data.some((item) => value(item) > 0)

  return (
    <Card className='min-w-0 gap-3 py-3'>
      <CardHeader className='border-b px-3 [.border-b]:pb-3'>
        <CardTitle className='text-sm'>{title}</CardTitle>
        <CardDescription>{description}</CardDescription>
      </CardHeader>
      <CardContent className='px-3'>
        {hasData ? (
          <div className='grid gap-2'>
            <div className='flex h-28 items-end gap-1' aria-label={title}>
              {data.map((item) => {
                const amount = value(item)
                const label = item.timestamp.slice(5, 16).replace('T', ' ')
                return (
                  <DashboardTooltip
                    key={item.timestamp}
                    content={
                      <div className='space-y-1'>
                        <p>{label}</p>
                        <p className='font-medium'>{format(amount)}</p>
                      </div>
                    }
                  >
                    <div
                      className='group flex h-full min-w-0 flex-1 cursor-default items-end focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none'
                      tabIndex={0}
                    >
                      <div
                        className='w-full rounded-t-sm bg-primary/80 transition-colors group-hover:bg-primary group-focus:bg-primary'
                        style={{
                          height: `${Math.max(3, (amount / maximum) * 100)}%`,
                        }}
                      />
                      <span className='sr-only'>
                        {label}：{format(amount)}
                      </span>
                    </div>
                  </DashboardTooltip>
                )
              })}
            </div>
            <div className='flex justify-between text-xs text-muted-foreground'>
              <span>{data[0]?.timestamp.slice(5, 10) ?? '—'}</span>
              <span>{data.at(-1)?.timestamp.slice(5, 10) ?? '—'}</span>
            </div>
          </div>
        ) : (
          <p className='py-14 text-center text-sm text-muted-foreground'>
            当前时段暂无访问数据
          </p>
        )}
      </CardContent>
    </Card>
  )
}

function clientIpTooltipDetails(item: WebsiteDashboardClientIpRanking) {
  return (
    <div className='space-y-1 break-words'>
      <p>ASN：{item.asn || '未知'}</p>
      <p>AS 名称：{item.as_name || '暂无数据'}</p>
    </div>
  )
}

function WebsiteDashboardRankingList<T extends WebsiteDashboardRanking>({
  title,
  description,
  data,
  value,
  format,
  tooltipDetails,
}: {
  title: string
  description: string
  data: T[]
  value: (item: T) => number
  format: (value: number) => string
  tooltipDetails?: (item: T) => ReactNode
}) {
  const maximum = Math.max(1, ...data.map(value))

  return (
    <Card className='h-full min-w-0 gap-3 py-3'>
      <CardHeader className='border-b px-3 [.border-b]:pb-3'>
        <CardTitle className='text-sm'>{title}</CardTitle>
        <CardDescription>{description}</CardDescription>
      </CardHeader>
      <CardContent className='space-y-2 px-3'>
        {data.map((item) => {
          const amount = value(item)
          return (
            <div key={item.label} className='grid gap-1'>
              <div className='flex items-center justify-between gap-3 text-sm'>
                <DashboardTooltip
                  content={
                    <div className='space-y-1 break-all'>
                      <p>{item.label}</p>
                      {tooltipDetails?.(item)}
                      <p className='font-medium'>{format(amount)}</p>
                    </div>
                  }
                >
                  <span
                    className='min-w-0 cursor-default truncate font-mono'
                    tabIndex={0}
                  >
                    {item.label}
                  </span>
                </DashboardTooltip>
                <span className='shrink-0 text-muted-foreground tabular-nums'>
                  {format(amount)}
                </span>
              </div>
              <div className='h-1.5 overflow-hidden rounded-full bg-muted'>
                <div
                  className='h-full rounded-full bg-primary'
                  style={{ width: `${(amount / maximum) * 100}%` }}
                />
              </div>
            </div>
          )
        })}
        {!data.length && (
          <p className='py-5 text-center text-sm text-muted-foreground'>
            当前时段暂无访问数据
          </p>
        )}
      </CardContent>
    </Card>
  )
}

function MountedWebsiteDetailSheet({
  website,
  onOpenChange,
}: {
  website: Website
  onOpenChange: (open: boolean) => void
}) {
  const websiteId = website?.id
  const { data: dashboard, error: dashboardError } =
    useWebsiteDashboard(websiteId)
  const dashboardLoading = useDelayedLoading(!dashboard && !dashboardError)

  const detailQuery = useQuery(websiteDetailQuery(websiteId))
  const dnsProbeMutation = useMutation({
    mutationFn: () => probeWebsiteDns(websiteId),
    onSuccess: (response) => {
      toast.success(response.message)
    },
    onError: (error) => toast.error(apiErrorMessage(error)),
  })

  const detail = detailQuery.data ?? website
  const summary = dashboard?.summary
  const domainStates = detail.runtime.domain_states
  const originStates = detail.runtime.origin_states

  return (
    <Sheet open onOpenChange={onOpenChange}>
      <SheetContent className='flex w-full flex-col overflow-hidden p-0 sm:w-[80vw] sm:max-w-none'>
        <SheetHeader className='border-b px-6 py-5 text-start'>
          <div className='flex flex-wrap items-center gap-2'>
            <SheetTitle>{detail.config.name || '网站'} · 详情看板</SheetTitle>
            <StatusBadge status={detail.status} />
            <StatusBadge status={detail.runtime.deploy_status} />
          </div>
          <SheetDescription>
            {detail.cluster_name} · {detail.access_domain}
          </SheetDescription>
        </SheetHeader>
        <ScrollArea className='min-h-0 flex-1'>
          <div className='space-y-4 p-4'>
            {dashboardError && (
              <Alert variant='destructive'>
                <AlertTitle>网站统计暂未更新</AlertTitle>
                <AlertDescription>
                  {dashboardError}
                  {dashboard ? ' 当前展示上次有效数据。' : ''}
                </AlertDescription>
              </Alert>
            )}
            {dashboardLoading.pending && (
              <div
                role='status'
                aria-label='正在加载网站统计'
                className='space-y-4'
              >
                <p className='text-sm text-muted-foreground'>
                  正在加载网站统计…
                </p>
                <Skeleton
                  className={
                    dashboardLoading.showSkeleton
                      ? 'h-24 w-full'
                      : 'invisible h-24 w-full'
                  }
                />
                <Skeleton
                  className={
                    dashboardLoading.showSkeleton
                      ? 'h-56 w-full'
                      : 'invisible h-56 w-full'
                  }
                />
              </div>
            )}
            {!dashboardLoading.pending && dashboard && (
              <>
                <section
                  className='grid grid-cols-2 gap-px overflow-hidden rounded-lg border bg-border sm:grid-cols-3 2xl:grid-cols-6'
                  aria-label='网站核心运行指标'
                >
                  <WebsiteDashboardMetric
                    title='上月峰值带宽'
                    value={formatBytesPerSecond(
                      summary?.previous_month_peak_bps
                    )}
                    description='按分钟响应流量聚合'
                    icon={Activity}
                  />
                  <WebsiteDashboardMetric
                    title='当月峰值带宽'
                    value={formatBytesPerSecond(
                      summary?.current_month_peak_bps
                    )}
                    description='按分钟响应流量聚合'
                    icon={Network}
                  />
                  <WebsiteDashboardMetric
                    title='当天峰值带宽'
                    value={formatBytesPerSecond(summary?.today_peak_bps)}
                    description='按分钟响应流量聚合'
                    icon={Globe2}
                  />
                  <WebsiteDashboardMetric
                    title='当前带宽'
                    value={formatBytesPerSecond(summary?.current_bandwidth_bps)}
                    description='最近一分钟平均值'
                    icon={Server}
                  />
                  <WebsiteDashboardMetric
                    title='当天独立 IP'
                    value={summary?.today_unique_ips ?? '—'}
                    description='按客户端 IP 去重'
                    icon={Activity}
                  />
                  <WebsiteDashboardMetric
                    title='当天流量'
                    value={formatBytes(summary?.today_response_bytes)}
                    description='当天响应流量累计'
                    icon={ShieldCheck}
                  />
                </section>

                <div className='grid min-w-0 grid-cols-1 gap-4 xl:grid-cols-2'>
                  <WebsiteDashboardTrend
                    title='24 小时流量趋势'
                    description='按小时汇总响应流量。'
                    data={dashboard?.hourly ?? []}
                    value={(item) => item.response_bytes}
                    format={formatBytes}
                  />
                  <WebsiteDashboardTrend
                    title='15 天流量趋势'
                    description='按天汇总响应流量。'
                    data={dashboard?.daily ?? []}
                    value={(item) => item.response_bytes}
                    format={formatBytes}
                  />
                  <WebsiteDashboardTrend
                    title='24 小时访问量趋势'
                    description='按小时汇总请求次数。'
                    data={dashboard?.hourly ?? []}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                  <WebsiteDashboardTrend
                    title='15 天访问量趋势'
                    description='按天汇总请求次数。'
                    data={dashboard?.daily ?? []}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                </div>

                <div className='grid min-w-0 auto-rows-fr grid-cols-1 gap-4 lg:grid-cols-2'>
                  <WebsiteDashboardRankingList
                    title='状态码分布'
                    description='最近 24 小时的请求数。'
                    data={dashboard?.status_codes ?? []}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                  <WebsiteDashboardRankingList
                    title='请求方法分布'
                    description='最近 24 小时的请求数。'
                    data={dashboard?.methods ?? []}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                  <WebsiteDashboardRankingList
                    title='国家/地区排行'
                    description='最近 24 小时按请求数，由本地 XDB 库解析。'
                    data={dashboard?.countries ?? []}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                  <WebsiteDashboardRankingList
                    title='域名访问排行'
                    description='最近 24 小时按请求数。'
                    data={dashboard?.hosts ?? []}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                  <WebsiteDashboardRankingList
                    title='请求来源排行'
                    description='最近 24 小时按请求数。'
                    data={dashboard?.referers ?? []}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                  <WebsiteDashboardRankingList
                    title='请求路径排行'
                    description='最近 24 小时按请求数。'
                    data={dashboard?.paths ?? []}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                  <WebsiteDashboardRankingList
                    title='独立 IP 排行（下行流量）'
                    description='最近 24 小时按响应流量。'
                    data={dashboard?.client_ips_by_bytes ?? []}
                    tooltipDetails={clientIpTooltipDetails}
                    value={(item) => item.response_bytes}
                    format={formatBytes}
                  />
                  <WebsiteDashboardRankingList
                    title='独立 IP 排行（请求数）'
                    description='最近 24 小时按请求数。'
                    data={dashboard?.client_ips_by_requests ?? []}
                    tooltipDetails={clientIpTooltipDetails}
                    value={(item) => item.request_count}
                    format={(value) => `${value} 次`}
                  />
                </div>
              </>
            )}

            <div className='grid min-w-0 grid-cols-1 gap-4 xl:grid-cols-2'>
              <Card className='min-w-0 gap-3 py-3'>
                <CardHeader className='flex flex-row items-start justify-between gap-3 border-b px-3 [.border-b]:pb-3'>
                  <div className='space-y-1.5'>
                    <CardTitle>域名解析状态</CardTitle>
                    <CardDescription>
                      由公共 DNS 探测服务校验 CNAME 指向。
                    </CardDescription>
                  </div>
                  <Button
                    type='button'
                    variant='outline'
                    size='sm'
                    className='shrink-0'
                    onClick={() => dnsProbeMutation.mutate()}
                    disabled={dnsProbeMutation.isPending}
                  >
                    <RefreshCw
                      className={
                        dnsProbeMutation.isPending ? 'animate-spin' : undefined
                      }
                      aria-hidden='true'
                    />
                    立即检测
                  </Button>
                </CardHeader>
                <CardContent className='divide-y px-3'>
                  {detail.config.domains.map((domain) => {
                    const runtime = domainStates.find(
                      (item) => item.id === domain.id
                    )
                    return (
                      <div
                        key={domain.id}
                        className='flex flex-wrap items-center justify-between gap-3 py-2.5'
                      >
                        <div className='min-w-0'>
                          <p className='truncate font-mono text-sm font-medium'>
                            {domain.hostname}
                          </p>
                          <p className='mt-1 text-xs text-muted-foreground'>
                            {domain.dns_mode === 'managed'
                              ? '托管解析'
                              : '外部解析'}
                          </p>
                        </div>
                        <div className='max-w-full space-y-1 text-end'>
                          <DashboardTooltip
                            content={
                              <div className='space-y-1'>
                                <p>{domain.hostname}</p>
                                <p>
                                  解析状态：
                                  {runtime?.resolution_status ?? 'pending'}
                                </p>
                                {runtime?.last_verified_at && (
                                  <p>最近检测：{runtime.last_verified_at}</p>
                                )}
                                {runtime?.last_error && (
                                  <p className='break-all'>
                                    错误：{runtime.last_error}
                                  </p>
                                )}
                              </div>
                            }
                          >
                            <span
                              className='inline-flex cursor-default'
                              tabIndex={0}
                            >
                              <StatusBadge
                                status={runtime?.resolution_status ?? 'pending'}
                              />
                            </span>
                          </DashboardTooltip>
                          {runtime?.last_error && (
                            <p
                              className='max-w-56 truncate text-xs text-destructive'
                              title={runtime.last_error}
                            >
                              {runtime.last_error}
                            </p>
                          )}
                        </div>
                      </div>
                    )
                  })}
                </CardContent>
              </Card>

              <Card className='min-w-0 gap-3 py-3'>
                <CardHeader className='border-b px-3 [.border-b]:pb-3'>
                  <CardTitle>源站健康</CardTitle>
                  <CardDescription>各节点最新回源探测结果。</CardDescription>
                </CardHeader>
                <CardContent className='space-y-2 px-3'>
                  {detail.config.origins.map((origin) => {
                    const states = originStates.filter(
                      (state) => state.origin_id === origin.id
                    )
                    const state =
                      origin.status === 'disabled'
                        ? 'disabled'
                        : states.some((item) => item.status === 'unhealthy')
                          ? 'unhealthy'
                          : states.some((item) => item.status === 'healthy')
                            ? 'healthy'
                            : 'pending'
                    return (
                      <div key={origin.id} className='rounded-lg border p-2.5'>
                        <div className='flex flex-wrap items-start justify-between gap-3'>
                          <div className='min-w-0'>
                            <p className='truncate font-mono text-sm font-medium'>
                              {origin.protocol}://{origin.host}:{origin.port}
                            </p>
                            <p className='mt-1 text-xs text-muted-foreground'>
                              {originGroupLabel(origin.group)} ·{' '}
                              {origin.role === 'primary'
                                ? '主要源站'
                                : '备用源站'}{' '}
                              · 权重 {origin.weight}
                            </p>
                          </div>
                          <DashboardTooltip
                            content={
                              <div className='space-y-1'>
                                <p>
                                  {origin.protocol}://{origin.host}:
                                  {origin.port}
                                </p>
                                <p>整体状态：{state}</p>
                                {states.length > 0 ? (
                                  states.map((item) => (
                                    <p key={item.node_id} className='break-all'>
                                      {item.node_name || '未知节点'}：
                                      {item.status} · {item.latency_millis} ms
                                      {item.last_error
                                        ? ` · ${item.last_error}`
                                        : ''}
                                    </p>
                                  ))
                                ) : (
                                  <p>尚未收到节点探测结果。</p>
                                )}
                              </div>
                            }
                          >
                            <span
                              className='inline-flex cursor-default'
                              tabIndex={0}
                            >
                              <StatusBadge status={state} />
                            </span>
                          </DashboardTooltip>
                        </div>
                        {states.length > 0 && (
                          <div className='mt-2 flex flex-wrap gap-1.5'>
                            {states.map((item) => (
                              <Badge key={item.node_id} variant='outline'>
                                {item.node_name || '未知节点'} ·{' '}
                                {item.latency_millis} ms
                              </Badge>
                            ))}
                          </div>
                        )}
                      </div>
                    )
                  })}
                </CardContent>
              </Card>
            </div>
          </div>
        </ScrollArea>
      </SheetContent>
    </Sheet>
  )
}

export function WebsiteDetailSheet({
  website,
  onOpenChange,
}: {
  website: Website | null
  onOpenChange: (open: boolean) => void
}) {
  if (!website) return null
  return (
    <MountedWebsiteDetailSheet
      key={website.id}
      website={website}
      onOpenChange={onOpenChange}
    />
  )
}
