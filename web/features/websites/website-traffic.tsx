import { ArrowRotateLeft } from '@gravity-ui/icons'
import { ChartTooltip } from '@heroui-pro/react'
import { AreaChart } from '@heroui-pro/react/area-chart'
import { BarChart } from '@heroui-pro/react/bar-chart'
import { KPI } from '@heroui-pro/react/kpi'
import { PieChart } from '@heroui-pro/react/pie-chart'
import { Alert, Button, Skeleton } from '@heroui/react'
import {
  formatBitsPerSecond,
  formatBytes,
  formatDate,
  formatNumber,
  formatPercent,
  ratioOf,
} from '@/lib/format'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { EmptyState } from '@/components/page'
import { Section } from '@/components/section'
import type {
  WebsiteDashboard,
  WebsiteDashboardClientIpRanking,
  WebsiteDashboardRanking,
} from './dashboard-schema'
import { useWebsiteDashboard } from './use-website-dashboard'
import { useWebsite } from './website-context'

const statusClassColors: Record<string, string> = {
  '1': 'var(--chart-1)',
  '2': 'var(--success)',
  '3': 'var(--accent)',
  '4': 'var(--warning)',
  '5': 'var(--danger)',
}

/** `/websites/$websiteId?tab=traffic`: access-log based statistics. Subscribes to
 * the dashboard stream only while this tab is mounted. */
export function WebsiteTraffic() {
  const website = useWebsite()
  const dashboard = useWebsiteDashboard(website.id)
  const data = dashboard.data
  const logging = website.config.access_log_enabled

  return (
    <div className='flex flex-col gap-6'>
      {!logging && (
        <Alert status='warning'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>访问日志未启用</Alert.Title>
            <Alert.Description>
              流量统计基于访问日志，启用后新的请求才会计入。
            </Alert.Description>
          </Alert.Content>
          <LinkButton
            variant='tertiary'
            href={`/websites/${website.id}?tab=settings&section=logs`}
            className='shrink-0 self-center'
          >
            启用访问日志
          </LinkButton>
        </Alert>
      )}
      {dashboard.error && (
        <Alert status='danger'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>统计连接异常</Alert.Title>
            <Alert.Description>{dashboard.error}</Alert.Description>
          </Alert.Content>
          <Button
            size='sm'
            variant='tertiary'
            className='shrink-0 self-center'
            onPress={dashboard.reconnect}
          >
            <ArrowRotateLeft />
            重新连接
          </Button>
        </Alert>
      )}
      {data ? <TrafficContent data={data} /> : <TrafficSkeleton />}
    </div>
  )
}

function TrafficSkeleton() {
  return (
    <div
      role='status'
      aria-label='正在加载流量统计'
      className='flex flex-col gap-6'
    >
      <div className='grid gap-4 sm:grid-cols-2 lg:grid-cols-3 2xl:grid-cols-6'>
        {Array.from({ length: 6 }, (_, index) => (
          <Skeleton key={index} className='h-24 rounded-2xl' />
        ))}
      </div>
      <div className='grid gap-6 xl:grid-cols-2'>
        <Skeleton className='h-72 rounded-2xl' />
        <Skeleton className='h-72 rounded-2xl' />
      </div>
    </div>
  )
}

function TrafficContent({ data }: { data: WebsiteDashboard }) {
  const { summary } = data
  const metrics = [
    ['当前带宽', summary.current_bandwidth_bps, formatBitsPerSecond],
    ['今日峰值带宽', summary.today_peak_bps, formatBitsPerSecond],
    ['今日响应流量', summary.today_response_bytes, formatBytes],
    ['今日独立 IP', summary.today_unique_ips, formatNumber],
    ['本月峰值带宽', summary.current_month_peak_bps, formatBitsPerSecond],
    ['上月峰值带宽', summary.previous_month_peak_bps, formatBitsPerSecond],
  ] as const
  const hourly = data.hourly.map((point) => ({
    label: formatDate(point.timestamp).slice(11, 16),
    time: formatDate(point.timestamp).slice(0, 16),
    bandwidth: point.bandwidth_bps,
    requests: point.request_count,
  }))
  const daily = data.daily.map((point) => ({
    label: formatDate(point.timestamp).slice(5, 10),
    requests: point.request_count,
    bytes: point.response_bytes,
  }))
  const totalStatus = data.status_codes.reduce(
    (sum, item) => sum + item.request_count,
    0
  )
  return (
    <>
      <div className='grid gap-4 sm:grid-cols-2 lg:grid-cols-3 2xl:grid-cols-6'>
        {metrics.map(([label, value, format]) => (
          <KPI key={label}>
            <KPI.Header>
              <KPI.Title>{label}</KPI.Title>
            </KPI.Header>
            <KPI.Content>
              <KPI.Value value={value}>{() => format(value)}</KPI.Value>
            </KPI.Content>
          </KPI>
        ))}
      </div>
      <p className='-mt-3 text-xs text-muted'>
        统计基于访问日志，按控制台时区（UTC+8）汇总。
      </p>

      <div className='grid gap-6 xl:grid-cols-2'>
        <Section
          title='24 小时带宽'
          description='每小时平均出口带宽（bit/s）。'
        >
          {hourly.length ? (
            <AreaChart data={hourly} height={240}>
              <defs>
                <linearGradient
                  id='website-bandwidth'
                  x1='0'
                  x2='0'
                  y1='0'
                  y2='1'
                >
                  <stop
                    offset='0%'
                    stopColor='var(--chart-3)'
                    stopOpacity={0.25}
                  />
                  <stop
                    offset='100%'
                    stopColor='var(--chart-3)'
                    stopOpacity={0.02}
                  />
                </linearGradient>
              </defs>
              <AreaChart.Grid vertical={false} />
              <AreaChart.XAxis dataKey='label' tickMargin={8} minTickGap={24} />
              <AreaChart.YAxis
                width={72}
                tickFormatter={(value: number) => formatBitsPerSecond(value)}
              />
              <AreaChart.Area
                dataKey='bandwidth'
                name='带宽'
                type='monotone'
                dot={false}
                stroke='var(--chart-3)'
                strokeWidth={2}
                fill='url(#website-bandwidth)'
              />
              <AreaChart.Tooltip
                content={({ active, payload }) => {
                  const point = payload?.[0]?.payload as
                    (typeof hourly)[number] | undefined
                  if (!active || !point) return null
                  return (
                    <ChartTooltip>
                      <ChartTooltip.Header>{point.time}</ChartTooltip.Header>
                      <ChartTooltip.Item>
                        <ChartTooltip.Indicator color='var(--chart-3)' />
                        <ChartTooltip.Label>带宽</ChartTooltip.Label>
                        <ChartTooltip.Value>
                          {formatBitsPerSecond(point.bandwidth)}
                        </ChartTooltip.Value>
                      </ChartTooltip.Item>
                      <ChartTooltip.Item>
                        <ChartTooltip.Indicator color='var(--chart-1)' />
                        <ChartTooltip.Label>请求</ChartTooltip.Label>
                        <ChartTooltip.Value>
                          {formatNumber(point.requests)}
                        </ChartTooltip.Value>
                      </ChartTooltip.Item>
                    </ChartTooltip>
                  )
                }}
              />
            </AreaChart>
          ) : (
            <EmptyState
              size='sm'
              title='暂无带宽数据'
              description='收到访问记录后显示趋势。'
            />
          )}
        </Section>
        <Section title='近 15 天请求量' description='每日请求数。'>
          {daily.length ? (
            <BarChart data={daily} height={240}>
              <BarChart.Grid vertical={false} />
              <BarChart.XAxis dataKey='label' tickMargin={8} minTickGap={12} />
              <BarChart.YAxis
                width={56}
                tickFormatter={(value: number) =>
                  formatNumber(value, { compact: true })
                }
              />
              <BarChart.Bar
                dataKey='requests'
                name='请求'
                fill='var(--chart-3)'
                radius={[4, 4, 0, 0]}
                maxBarSize={28}
              />
              <BarChart.Tooltip
                content={({ active, label, payload }) => {
                  const point = payload?.[0]?.payload as
                    (typeof daily)[number] | undefined
                  if (!active || !point) return null
                  return (
                    <ChartTooltip>
                      <ChartTooltip.Header>{label}</ChartTooltip.Header>
                      <ChartTooltip.Item>
                        <ChartTooltip.Indicator color='var(--chart-3)' />
                        <ChartTooltip.Label>请求</ChartTooltip.Label>
                        <ChartTooltip.Value>
                          {formatNumber(point.requests)}
                        </ChartTooltip.Value>
                      </ChartTooltip.Item>
                      <ChartTooltip.Item>
                        <ChartTooltip.Indicator color='var(--chart-1)' />
                        <ChartTooltip.Label>流量</ChartTooltip.Label>
                        <ChartTooltip.Value>
                          {formatBytes(point.bytes)}
                        </ChartTooltip.Value>
                      </ChartTooltip.Item>
                    </ChartTooltip>
                  )
                }}
              />
            </BarChart>
          ) : (
            <EmptyState
              size='sm'
              title='暂无请求数据'
              description='收到访问记录后显示趋势。'
            />
          )}
        </Section>
      </div>

      <div className='grid gap-6 md:grid-cols-2 xl:grid-cols-3'>
        <Section title='状态码分布' description='今日请求按状态码。'>
          {totalStatus ? (
            <div className='flex flex-col items-center gap-4 sm:flex-row xl:flex-col 2xl:flex-row'>
              <div className='relative shrink-0'>
                <PieChart height={168} width={168}>
                  <PieChart.Pie
                    data={data.status_codes}
                    dataKey='request_count'
                    nameKey='label'
                    innerRadius='70%'
                    outerRadius='96%'
                    strokeWidth={0}
                    paddingAngle={2}
                  >
                    {data.status_codes.map((item) => (
                      <PieChart.Cell
                        key={item.label}
                        fill={
                          statusClassColors[item.label[0]] ?? 'var(--chart-1)'
                        }
                      />
                    ))}
                  </PieChart.Pie>
                  <PieChart.Tooltip
                    content={
                      <PieChart.TooltipContent
                        hideHeader
                        valueFormatter={(value) => formatNumber(Number(value))}
                      />
                    }
                  />
                </PieChart>
                <div className='pointer-events-none absolute inset-0 flex flex-col items-center justify-center'>
                  <span className='text-lg font-semibold tabular-nums'>
                    {formatNumber(totalStatus, { compact: true })}
                  </span>
                  <span className='text-xs text-muted'>请求</span>
                </div>
              </div>
              <ul className='flex w-full min-w-0 flex-col gap-2'>
                {data.status_codes.map((item) => (
                  <li
                    key={item.label}
                    className='flex items-center gap-2 text-sm'
                  >
                    <span
                      aria-hidden
                      className='size-2.5 shrink-0 rounded-full'
                      style={{
                        backgroundColor:
                          statusClassColors[item.label[0]] ?? 'var(--chart-1)',
                      }}
                    />
                    <Mono>{item.label}</Mono>
                    <span className='ml-auto tabular-nums'>
                      {formatNumber(item.request_count)}
                    </span>
                    <span className='w-14 text-right text-xs text-muted tabular-nums'>
                      {formatPercent(ratioOf(item.request_count, totalStatus))}
                    </span>
                  </li>
                ))}
              </ul>
            </div>
          ) : (
            <EmptyState size='sm' title='暂无请求' />
          )}
        </Section>
        <Ranking title='国家 / 地区' data={data.countries} />
        <Ranking title='域名' data={data.hosts} mono />
        <Ranking title='访问路径' data={data.paths} mono />
        <Ranking title='来源页面' data={data.referers} mono />
        <Ranking title='请求方法' data={data.methods} mono />
        <Ranking
          title='客户端 IP · 按流量'
          data={data.client_ips_by_bytes}
          metric='bytes'
          mono
        />
        <Ranking
          title='客户端 IP · 按请求'
          data={data.client_ips_by_requests}
          mono
        />
      </div>
    </>
  )
}

type RankingRow = WebsiteDashboardRanking &
  Partial<Pick<WebsiteDashboardClientIpRanking, 'asn' | 'as_name'>>

/** Top-N card: label, share bar against the leader, requests and bytes. */
function Ranking({
  title,
  data,
  metric = 'requests',
  mono = false,
}: {
  title: string
  data: RankingRow[]
  metric?: 'requests' | 'bytes'
  mono?: boolean
}) {
  const value = (item: RankingRow) =>
    metric === 'bytes' ? item.response_bytes : item.request_count
  const leader = Math.max(0, ...data.map(value))
  return (
    <Section
      title={title}
      description={metric === 'bytes' ? '按响应流量排序' : '按请求数排序'}
    >
      {data.length ? (
        <ol className='flex flex-col gap-2.5'>
          {data.slice(0, 8).map((item) => (
            <li key={item.label} className='flex min-w-0 flex-col gap-1'>
              <div className='flex min-w-0 items-baseline gap-3 text-sm'>
                <span className='min-w-0 flex-1 truncate' title={item.label}>
                  {mono ? <Mono>{item.label || '—'}</Mono> : item.label || '—'}
                  {item.asn && (
                    <span className='ml-2 text-xs text-muted'>
                      {item.asn} {item.as_name}
                    </span>
                  )}
                </span>
                <span className='shrink-0 tabular-nums'>
                  {metric === 'bytes'
                    ? formatBytes(item.response_bytes)
                    : formatNumber(item.request_count)}
                </span>
                <span className='w-16 shrink-0 text-right text-xs text-muted tabular-nums'>
                  {metric === 'bytes'
                    ? `${formatNumber(item.request_count, { compact: true })} 次`
                    : formatBytes(item.response_bytes)}
                </span>
              </div>
              <div className='h-1 overflow-hidden rounded-full bg-default'>
                <div
                  className='h-full rounded-full bg-accent'
                  style={{ width: `${ratioOf(value(item), leader) * 100}%` }}
                />
              </div>
            </li>
          ))}
        </ol>
      ) : (
        <p className='py-6 text-center text-sm text-muted'>暂无数据</p>
      )}
    </Section>
  )
}
