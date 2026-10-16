import { ChartAreaStacked, CircleInfo, Globe } from '@gravity-ui/icons'
import { ChartTooltip, Widget, type DataGridColumn } from '@heroui-pro/react'
import { AreaChart } from '@heroui-pro/react/area-chart'
import { PieChart } from '@heroui-pro/react/pie-chart'
import { Link, Meter, Skeleton } from '@heroui/react'
import {
  formatBitsPerSecond,
  formatBytes,
  formatDate,
  formatNumber,
  formatPercent,
} from '@/lib/format'
import { DataTable } from '@/components/data-table'
import { EmptyState, QueryNotice } from '@/components/page'
import {
  coverageState,
  statusDistribution,
  topWebsiteRows,
  trafficSeries,
  type OverviewTraffic,
  type StatusClassItem,
  type TopWebsiteRow,
  type TrafficPoint,
} from './data'

type TrafficQuery = {
  data?: OverviewTraffic
  isPending: boolean
  isError: boolean
  error: unknown
  refetch: () => unknown
}

/** Explains an empty traffic panel; renders nothing when there is data. */
function TrafficEmpty({
  traffic,
  size = 'md',
}: {
  traffic: OverviewTraffic
  size?: 'sm' | 'md'
}) {
  const state = coverageState(traffic)
  if (state === 'ok') return null
  if (state === 'no_websites')
    return (
      <EmptyState
        size={size}
        icon={<Globe />}
        title='还没有网站'
        description='创建网站并启用访问日志后，这里会显示流量统计。'
      />
    )
  if (state === 'no_logging')
    return (
      <EmptyState
        size={size}
        icon={<CircleInfo />}
        title='没有网站启用访问日志'
        description='流量统计来自访问日志。在网站配置的「日志」分组中启用后开始统计。'
      />
    )
  return (
    <EmptyState
      size={size}
      icon={<ChartAreaStacked />}
      title='过去 24 小时没有访问记录'
      description='启用访问日志的网站产生请求后，这里会实时更新。'
    />
  )
}

export function BandwidthPanel({ query }: { query: TrafficQuery }) {
  const traffic = query.data
  return (
    <Widget className='min-w-0 lg:col-span-8'>
      <Widget.Header>
        <Widget.Title>24 小时带宽</Widget.Title>
        <Widget.Description className='whitespace-nowrap'>
          小时平均出口带宽
        </Widget.Description>
      </Widget.Header>
      <Widget.Content className='flex min-h-[292px] flex-col justify-center'>
        <QueryNotice query={query} onRetry={query.refetch} />
        {!traffic ? (
          query.isPending && (
            <Skeleton className='h-[260px] w-full rounded-xl' />
          )
        ) : coverageState(traffic) !== 'ok' ? (
          <TrafficEmpty traffic={traffic} />
        ) : (
          <AreaChart
            data={trafficSeries(traffic.hourly)}
            height={260}
            margin={{ top: 8, right: 0, bottom: 0, left: 0 }}
            aria-label='过去 24 小时平均出口带宽'
          >
            <defs>
              <linearGradient
                id='overview-bandwidth'
                x1='0'
                x2='0'
                y1='0'
                y2='1'
              >
                <stop
                  offset='0%'
                  stopColor='var(--color-accent)'
                  stopOpacity={0.24}
                />
                <stop
                  offset='100%'
                  stopColor='var(--color-accent)'
                  stopOpacity={0.02}
                />
              </linearGradient>
            </defs>
            <AreaChart.Grid vertical={false} />
            <AreaChart.XAxis
              dataKey='label'
              tickMargin={8}
              interval='preserveStartEnd'
              minTickGap={24}
            />
            <AreaChart.YAxis
              width='auto'
              tickFormatter={(value: number) => formatBitsPerSecond(value)}
            />
            <AreaChart.Area
              dataKey='bandwidth_bps'
              name='带宽'
              type='monotone'
              dot={false}
              stroke='var(--color-accent)'
              strokeWidth={2}
              fill='url(#overview-bandwidth)'
            />
            <AreaChart.Tooltip
              content={({ active, payload }) => {
                const point = payload?.[0]?.payload as TrafficPoint | undefined
                if (!active || !point) return null
                return (
                  <ChartTooltip>
                    <ChartTooltip.Header>
                      {formatDate(point.timestamp).slice(0, 16)}
                    </ChartTooltip.Header>
                    <ChartTooltip.Item>
                      <ChartTooltip.Indicator color='var(--color-accent)' />
                      <ChartTooltip.Label>平均带宽</ChartTooltip.Label>
                      <ChartTooltip.Value>
                        {formatBitsPerSecond(point.bandwidth_bps)}
                      </ChartTooltip.Value>
                    </ChartTooltip.Item>
                    <ChartTooltip.Item>
                      <ChartTooltip.Indicator color='var(--color-muted)' />
                      <ChartTooltip.Label>请求数</ChartTooltip.Label>
                      <ChartTooltip.Value>
                        {formatNumber(point.request_count)}
                      </ChartTooltip.Value>
                    </ChartTooltip.Item>
                  </ChartTooltip>
                )
              }}
            />
          </AreaChart>
        )}
      </Widget.Content>
    </Widget>
  )
}

/** Donut of the status classes with the total in its center (`PieChart.Label`)
 * and a Widget legend carrying each class's count and share. */
export function StatusClassPanel({ query }: { query: TrafficQuery }) {
  const traffic = query.data
  const distribution = traffic
    ? statusDistribution(traffic.status_classes)
    : undefined
  const slices = distribution?.items.filter((item) => item.request_count)
  return (
    <Widget className='@container min-w-0 lg:col-span-5'>
      <Widget.Header>
        <Widget.Title>状态码分布</Widget.Title>
      </Widget.Header>
      <Widget.Content className='flex min-h-[372px] flex-col justify-center @sm:min-h-[208px]'>
        <QueryNotice query={query} onRetry={query.refetch} />
        {!traffic || !distribution || !slices ? (
          query.isPending && (
            <div className='flex flex-col items-center gap-6 @sm:flex-row'>
              <Skeleton className='size-44 shrink-0 rounded-full' />
              <div className='flex w-full min-w-0 flex-1 flex-col gap-2.5'>
                {Array.from({ length: 5 }, (_, index) => (
                  <Skeleton key={index} className='h-5 w-full rounded-md' />
                ))}
              </div>
            </div>
          )
        ) : distribution.total === 0 ? (
          <TrafficEmpty traffic={traffic} size='sm' />
        ) : (
          <div className='flex flex-col items-center gap-6 @sm:flex-row'>
            <PieChart
              height={176}
              width={176}
              aria-label='状态码分布'
              className='w-44 shrink-0'
            >
              <PieChart.Pie
                data={slices}
                dataKey='request_count'
                nameKey='label'
                cx='50%'
                cy='50%'
                innerRadius='70%'
                strokeWidth={0}
                paddingAngle={1}
              >
                {slices.map((item) => (
                  <PieChart.Cell key={item.label} fill={item.color} />
                ))}
                <PieChart.Label
                  position='center'
                  value={formatNumber(distribution.total, { compact: true })}
                  className='fill-foreground text-lg font-semibold tabular-nums'
                />
              </PieChart.Pie>
              <PieChart.Tooltip
                content={({ active, payload }) => {
                  const entry = payload?.[0]
                  if (!active || !entry) return null
                  const item = entry.payload as StatusClassItem
                  return (
                    <ChartTooltip>
                      <ChartTooltip.Item>
                        <ChartTooltip.Indicator color={item.color} />
                        <ChartTooltip.Label>{item.label}</ChartTooltip.Label>
                        <ChartTooltip.Value>
                          {formatNumber(item.request_count)} 次 ·{' '}
                          {formatPercent(item.share)}
                        </ChartTooltip.Value>
                      </ChartTooltip.Item>
                    </ChartTooltip>
                  )
                }}
              />
            </PieChart>
            <Widget.Legend className='w-full min-w-0 flex-1 flex-col items-stretch gap-2.5'>
              {distribution.items.map((item) => (
                <div
                  key={item.label}
                  className='flex items-center gap-3 text-sm tabular-nums'
                >
                  <Widget.LegendItem
                    color={item.color}
                    className='w-14 font-mono'
                  >
                    {item.label}
                  </Widget.LegendItem>
                  <span className='flex-1 text-right'>
                    {formatNumber(item.request_count)}
                  </span>
                  <span className='w-14 text-right text-xs text-muted'>
                    {formatPercent(item.share)}
                  </span>
                </div>
              ))}
            </Widget.Legend>
          </div>
        )}
      </Widget.Content>
    </Widget>
  )
}

const topWebsiteColumns: DataGridColumn<TopWebsiteRow>[] = [
  {
    id: 'name',
    header: '网站',
    isRowHeader: true,
    minWidth: 160,
    cell: (site) => (
      <Link href={`/websites/${site.website_id}`} className='font-medium'>
        {site.name}
      </Link>
    ),
  },
  {
    id: 'request_count',
    header: '请求',
    align: 'end',
    cell: (site) => (
      <span className='tabular-nums'>
        {formatNumber(site.request_count, { compact: true })}
      </span>
    ),
  },
  {
    id: 'response_bytes',
    header: '流量',
    align: 'end',
    cell: (site) => (
      <span className='tabular-nums'>{formatBytes(site.response_bytes)}</span>
    ),
  },
  {
    id: 'share',
    header: '占比',
    minWidth: 140,
    cell: (site) => (
      <Meter
        aria-label={`${site.name} 流量占比`}
        value={site.share * 100}
        formatOptions={{ style: 'percent', maximumFractionDigits: 1 }}
        size='sm'
        className='w-32 gap-1'
      >
        <Meter.Output className='col-span-full col-start-1 justify-self-start text-xs font-normal text-muted' />
        <Meter.Track>
          <Meter.Fill />
        </Meter.Track>
      </Meter>
    ),
  },
]

export function TopWebsitesPanel({ query }: { query: TrafficQuery }) {
  const traffic = query.data
  const state = traffic ? coverageState(traffic) : undefined
  return (
    <Widget className='min-w-0 lg:col-span-7'>
      <Widget.Header>
        <Widget.Title>流量 Top 网站</Widget.Title>
      </Widget.Header>
      <Widget.Content className='p-0'>
        <DataTable
          label='流量 Top 网站'
          variant='secondary'
          columns={topWebsiteColumns}
          data={traffic ? topWebsiteRows(traffic) : undefined}
          isLoading={query.isPending}
          error={query.isError ? query.error : undefined}
          onRetry={query.refetch}
          getRowId={(site) => site.website_id}
          minWidth={520}
          empty={
            state === 'no_websites'
              ? {
                  title: '还没有网站',
                  description: '创建网站并启用访问日志后，这里会显示流量排行。',
                }
              : state === 'no_logging'
                ? {
                    title: '没有网站启用访问日志',
                    description:
                      '在网站配置的「日志」分组中启用访问日志后开始统计。',
                  }
                : {
                    title: '过去 24 小时没有访问记录',
                    description:
                      '启用访问日志的网站产生请求后，这里会显示排行。',
                  }
          }
        />
      </Widget.Content>
    </Widget>
  )
}
