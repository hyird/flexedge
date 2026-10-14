import { ChartAreaStacked, CircleInfo, Globe } from '@gravity-ui/icons'
import { type DataGridColumn, ChartTooltip } from '@heroui-pro/react'
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
import { LinkButton } from '@/components/link-button'
import { EmptyState, QueryNotice } from '@/components/page'
import { Section } from '@/components/section'
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

/** One-line provenance note shared by every access-log based panel. */
function coverageNote(traffic?: OverviewTraffic) {
  if (!traffic) return '基于访问日志'
  return `基于访问日志 · 覆盖 ${traffic.coverage.logging_website_count} / ${traffic.coverage.website_count} 个网站`
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
        action={<LinkButton href='/websites/new'>创建网站</LinkButton>}
      />
    )
  if (state === 'no_logging')
    return (
      <EmptyState
        size={size}
        icon={<CircleInfo />}
        title='没有网站启用访问日志'
        description='流量统计来自访问日志。在网站配置的「日志」分组中启用后开始统计。'
        action={
          <LinkButton variant='tertiary' href='/websites'>
            前往网站
          </LinkButton>
        }
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
  const points = traffic ? trafficSeries(traffic.hourly) : []
  return (
    <Section
      title='24 小时带宽'
      description={coverageNote(traffic)}
      actions={
        traffic && (
          <span className='text-xs text-muted tabular-nums'>
            峰值 {formatBitsPerSecond(traffic.summary.peak_bandwidth_bps)}
          </span>
        )
      }
      className='lg:col-span-8'
    >
      <QueryNotice query={query} onRetry={query.refetch} />
      {!traffic ? (
        query.isPending && <Skeleton className='h-[260px] w-full rounded-xl' />
      ) : coverageState(traffic) !== 'ok' ? (
        <TrafficEmpty traffic={traffic} />
      ) : (
        <AreaChart data={points} height={260} aria-label='过去 24 小时出口带宽'>
          <defs>
            <linearGradient id='overview-bandwidth' x1='0' x2='0' y1='0' y2='1'>
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
            width={84}
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
    </Section>
  )
}

export function StatusClassPanel({ query }: { query: TrafficQuery }) {
  const traffic = query.data
  const distribution = traffic
    ? statusDistribution(traffic.status_classes)
    : undefined
  return (
    <Section title='状态码分布' description={coverageNote(traffic)}>
      <QueryNotice query={query} onRetry={query.refetch} />
      {!traffic || !distribution ? (
        query.isPending && (
          <div className='flex items-center gap-6'>
            <Skeleton className='size-36 shrink-0 rounded-full' />
            <div className='flex flex-1 flex-col gap-3'>
              {Array.from({ length: 5 }, (_, index) => (
                <Skeleton key={index} className='h-4 w-full rounded-md' />
              ))}
            </div>
          </div>
        )
      ) : distribution.total === 0 ? (
        <TrafficEmpty traffic={traffic} size='sm' />
      ) : (
        <div className='flex flex-col items-center gap-6 sm:flex-row'>
          <div className='relative shrink-0'>
            <PieChart height={160} width={160} aria-label='状态码分布'>
              <PieChart.Pie
                data={distribution.items.filter((item) => item.request_count)}
                dataKey='request_count'
                nameKey='label'
                cx='50%'
                cy='50%'
                innerRadius='68%'
                strokeWidth={0}
                paddingAngle={1}
              >
                {distribution.items
                  .filter((item) => item.request_count)
                  .map((item) => (
                    <PieChart.Cell key={item.label} fill={item.color} />
                  ))}
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
            <div className='pointer-events-none absolute inset-0 flex flex-col items-center justify-center'>
              <span className='text-lg font-semibold tabular-nums'>
                {formatNumber(distribution.total, { compact: true })}
              </span>
              <span className='text-xs text-muted'>请求</span>
            </div>
          </div>
          <ul className='flex w-full min-w-0 flex-1 flex-col gap-2'>
            {distribution.items.map((item) => (
              <li key={item.label} className='flex items-center gap-3 text-sm'>
                <span
                  aria-hidden
                  className='size-2.5 shrink-0 rounded-full'
                  style={{ backgroundColor: item.color }}
                />
                <span className='w-10 font-mono text-[13px]'>{item.label}</span>
                <span className='flex-1 text-right tabular-nums'>
                  {formatNumber(item.request_count)}
                </span>
                <span className='w-14 text-right text-xs text-muted tabular-nums'>
                  {formatPercent(item.share)}
                </span>
              </li>
            ))}
          </ul>
        </div>
      )}
    </Section>
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
        size='sm'
        className='flex w-32 flex-row items-center gap-2'
      >
        <Meter.Track className='flex-1'>
          <Meter.Fill />
        </Meter.Track>
        <span className='w-12 shrink-0 text-right text-xs text-muted tabular-nums'>
          {formatPercent(site.share)}
        </span>
      </Meter>
    ),
  },
]

export function TopWebsitesPanel({ query }: { query: TrafficQuery }) {
  const traffic = query.data
  const state = traffic ? coverageState(traffic) : undefined
  return (
    <Section
      title='流量 Top 网站'
      description={`${coverageNote(traffic)} · 按响应流量排序`}
      actions={
        <LinkButton variant='ghost' href='/websites'>
          全部网站
        </LinkButton>
      }
      className='lg:col-span-6'
    >
      <DataTable
        label='流量 Top 网站'
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
                action: <LinkButton href='/websites/new'>创建网站</LinkButton>,
              }
            : state === 'no_logging'
              ? {
                  title: '没有网站启用访问日志',
                  description:
                    '在网站配置的「日志」分组中启用访问日志后开始统计。',
                }
              : {
                  title: '过去 24 小时没有访问记录',
                  description: '启用访问日志的网站产生请求后，这里会显示排行。',
                }
        }
      />
    </Section>
  )
}
