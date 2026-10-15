import { ArrowRotateLeft } from '@gravity-ui/icons'
import { ChartTooltip, TrendChip, Widget } from '@heroui-pro/react'
import { AreaChart } from '@heroui-pro/react/area-chart'
import { BarChart } from '@heroui-pro/react/bar-chart'
import { KPI } from '@heroui-pro/react/kpi'
import { PieChart } from '@heroui-pro/react/pie-chart'
import { Alert, Button, Label, Meter, Skeleton } from '@heroui/react'
import {
  formatBitsPerSecond,
  formatBytes,
  formatDate,
  formatNumber,
  formatPercent,
  ratioOf,
} from '@/lib/format'
import { cn } from '@/lib/utils'
import { KpiQuantity } from '@/components/kpi-quantity'
import { EmptyState } from '@/components/page'
import type {
  WebsiteDashboard,
  WebsiteDashboardClientIpRanking,
  WebsiteDashboardRanking,
} from './dashboard-schema'
import { useWebsiteDashboard } from './use-website-dashboard'
import { useOpenWebsiteTab, useWebsite } from './website-context'

const statusClassColors: Record<string, string> = {
  '1': 'var(--chart-1)',
  '2': 'var(--success)',
  '3': 'var(--accent)',
  '4': 'var(--warning)',
  '5': 'var(--danger)',
}

/** Website detail “流量分析” tab: access-log based statistics. Subscribes to
 * the dashboard stream only while this tab is mounted. */
export function WebsiteTraffic() {
  const website = useWebsite()
  const openTab = useOpenWebsiteTab()
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
          <Button
            size='sm'
            variant='tertiary'
            onPress={() => openTab('settings', { section: 'logs' })}
            className='shrink-0 self-center'
          >
            启用访问日志
          </Button>
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
      <div className='grid grid-cols-2 gap-3 lg:grid-cols-3 2xl:grid-cols-6'>
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
  const monthChange = summary.previous_month_peak_bps
    ? summary.current_month_peak_bps / summary.previous_month_peak_bps - 1
    : undefined
  return (
    <>
      <div className='grid grid-cols-2 gap-3 lg:grid-cols-3 2xl:grid-cols-6'>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>当前带宽</KPI.Title>
          </KPI.Header>
          <KPI.Content className='content-start'>
            <KpiQuantity value={summary.current_bandwidth_bps} unit='bps' />
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>今日峰值带宽</KPI.Title>
          </KPI.Header>
          <KPI.Content className='content-start'>
            <KpiQuantity value={summary.today_peak_bps} unit='bps' />
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>今日响应流量</KPI.Title>
          </KPI.Header>
          <KPI.Content className='content-start'>
            <KpiQuantity value={summary.today_response_bytes} unit='bytes' />
          </KPI.Content>
          {daily.length > 1 && (
            <KPI.Chart
              data={daily}
              dataKey='bytes'
              color='var(--color-accent)'
              height={40}
              strokeWidth={1.5}
            />
          )}
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>今日独立 IP</KPI.Title>
          </KPI.Header>
          <KPI.Content className='content-start'>
            <KPI.Value value={summary.today_unique_ips} />
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>本月峰值带宽</KPI.Title>
          </KPI.Header>
          <KPI.Content className='grid-cols-1 content-start justify-items-start gap-y-1'>
            <KpiQuantity value={summary.current_month_peak_bps} unit='bps' />
            {monthChange !== undefined && (
              <KPI.Trend
                trend={
                  monthChange > 0 ? 'up' : monthChange < 0 ? 'down' : 'neutral'
                }
              >
                {formatPercent(Math.abs(monthChange))}
                <TrendChip.Suffix>较上月</TrendChip.Suffix>
              </KPI.Trend>
            )}
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>上月峰值带宽</KPI.Title>
          </KPI.Header>
          <KPI.Content className='content-start'>
            <KpiQuantity value={summary.previous_month_peak_bps} unit='bps' />
          </KPI.Content>
        </KPI>
      </div>
      <p className='-mt-3 text-xs text-muted'>
        统计基于访问日志，按控制台时区（UTC+8）汇总。
      </p>

      <div className='grid gap-6 xl:grid-cols-2'>
        <Widget className='min-w-0'>
          <Widget.Header>
            <Widget.Title>24 小时带宽</Widget.Title>
          </Widget.Header>
          <Widget.Content>
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
                <AreaChart.XAxis
                  dataKey='label'
                  tickMargin={8}
                  minTickGap={24}
                />
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
          </Widget.Content>
        </Widget>
        <Widget className='min-w-0'>
          <Widget.Header>
            <Widget.Title>近 15 天请求量</Widget.Title>
          </Widget.Header>
          <Widget.Content>
            {daily.length ? (
              <BarChart data={daily} height={240}>
                <BarChart.Grid vertical={false} />
                <BarChart.XAxis
                  dataKey='label'
                  tickMargin={8}
                  minTickGap={12}
                />
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
          </Widget.Content>
        </Widget>
      </div>

      <div className='grid gap-6 md:grid-cols-2 xl:grid-cols-3'>
        <Widget className='min-w-0'>
          <Widget.Header>
            <Widget.Title>今日状态码</Widget.Title>
          </Widget.Header>
          <Widget.Content className='flex flex-col justify-center'>
            {totalStatus ? (
              <div className='flex flex-col items-center gap-4 sm:flex-row xl:flex-col 2xl:flex-row'>
                <PieChart
                  height={168}
                  width={168}
                  aria-label='今日状态码分布'
                  className='w-42 shrink-0'
                >
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
                    <PieChart.Label
                      position='center'
                      value={formatNumber(totalStatus, { compact: true })}
                      className='fill-foreground text-lg font-semibold tabular-nums'
                    />
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
                <Widget.Legend className='w-full min-w-0 flex-col items-stretch gap-2'>
                  {data.status_codes.map((item) => (
                    <div
                      key={item.label}
                      className='flex items-center gap-2 text-sm tabular-nums'
                    >
                      <Widget.LegendItem
                        color={
                          statusClassColors[item.label[0]] ?? 'var(--chart-1)'
                        }
                        className='font-mono'
                      >
                        {item.label}
                      </Widget.LegendItem>
                      <span className='ml-auto'>
                        {formatNumber(item.request_count)}
                      </span>
                      <span className='w-14 text-right text-xs text-muted'>
                        {formatPercent(
                          ratioOf(item.request_count, totalStatus)
                        )}
                      </span>
                    </div>
                  ))}
                </Widget.Legend>
              </div>
            ) : (
              <EmptyState size='sm' title='暂无请求' />
            )}
          </Widget.Content>
        </Widget>
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

/** Top 8 of one dimension: each row is a HeroUI Meter scaled to the leader,
 * labelled with the item and showing the ranked metric and the other one. */
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
    <Widget className='min-w-0'>
      <Widget.Header>
        <Widget.Title>{title}</Widget.Title>
      </Widget.Header>
      <Widget.Content>
        {data.length ? (
          <ol className='flex flex-col gap-3'>
            {data.slice(0, 8).map((item) => (
              <li key={item.label}>
                <Meter
                  value={value(item)}
                  maxValue={leader || 1}
                  valueLabel={
                    metric === 'bytes'
                      ? `${formatBytes(item.response_bytes)} · ${formatNumber(item.request_count, { compact: true })} 次`
                      : `${formatNumber(item.request_count)} · ${formatBytes(item.response_bytes)}`
                  }
                  size='sm'
                  className='grid-cols-[minmax(0,1fr)_auto] gap-x-3'
                >
                  <Label
                    title={item.label}
                    className={cn(
                      'max-w-full min-w-0 truncate font-normal',
                      mono && 'font-mono text-[13px]'
                    )}
                  >
                    {item.label || '—'}
                    {item.asn && (
                      <span className='ml-2 font-sans text-xs text-muted'>
                        {item.asn} {item.as_name}
                      </span>
                    )}
                  </Label>
                  <Meter.Output className='text-xs font-normal text-muted' />
                  <Meter.Track>
                    <Meter.Fill />
                  </Meter.Track>
                </Meter>
              </li>
            ))}
          </ol>
        ) : (
          <EmptyState size='sm' title='暂无数据' />
        )}
      </Widget.Content>
    </Widget>
  )
}
