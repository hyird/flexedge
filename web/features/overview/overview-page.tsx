import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import {
  ArrowRotateLeft,
  ChevronRight,
  CircleCheck,
  CircleExclamation,
  Globe,
  Server,
} from '@gravity-ui/icons'
import {
  ChartTooltip,
  ListView,
  Widget,
  type DataGridColumn,
} from '@heroui-pro/react'
import { AreaChart } from '@heroui-pro/react/area-chart'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, Chip, Link, Skeleton } from '@heroui/react'
import {
  formatBitsPerSecond,
  formatBytes,
  formatDate,
  formatNumber,
  ratioOf,
} from '@/lib/format'
import { describeStatus } from '@/lib/status'
import { DataTable } from '@/components/data-table'
import { KpiQuantity } from '@/components/kpi-quantity'
import { MetricMeter } from '@/components/metric-meter'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { NodeConnectionChip } from '@/features/nodes/node-connection-chip'
import type { Node } from '@/features/nodes/types'
import { recentTasksQuery } from '@/features/tasks/data'
import {
  recentTaskOrder,
  taskHref,
  taskKey,
  taskStatuses,
  taskStatusReserve,
  taskTitle,
} from '@/features/tasks/task-display'
import { taskRefOf, type Task } from '@/features/tasks/types'
import {
  appendBandwidthPoint,
  attentionItems,
  nodeHealth,
  overviewNodePageSize,
  overviewNodesQuery,
  overviewQuery,
  overviewTrafficQuery,
  type BandwidthPoint,
  type Overview,
  type OverviewTraffic,
} from './data'
import {
  BandwidthPanel,
  StatusClassPanel,
  TopWebsitesPanel,
} from './traffic-panels'

/** Overview dashboard. Holds one live stream while mounted, the combined
 * `/api/overview/stream` (events `snapshot`, `traffic`, `nodes`,
 * `node-runtime`); it is released when the page unmounts. Recent tasks are
 * the six most recently updated, read over GET (`/api/tasks`), active ones
 * first. */
export function OverviewPage() {
  const overview = useQuery(overviewQuery)
  const traffic = useQuery(overviewTrafficQuery)
  const nodes = useQuery(overviewNodesQuery)
  const tasks = useQuery({
    ...recentTasksQuery(6),
    meta: { inlineError: true },
  })
  const health = nodes.data ? nodeHealth(nodes.data.list) : undefined
  const [bandwidthHistory, setBandwidthHistory] = useState<{
    updatedAt: number
    egressBps?: number
    points: BandwidthPoint[]
  }>({ updatedAt: 0, points: [] })
  if (
    nodes.data &&
    (bandwidthHistory.updatedAt !== nodes.dataUpdatedAt ||
      bandwidthHistory.egressBps !== health?.egressBps)
  ) {
    setBandwidthHistory({
      updatedAt: nodes.dataUpdatedAt,
      egressBps: health?.egressBps,
      points:
        health?.egressBps === undefined
          ? bandwidthHistory.points
          : appendBandwidthPoint(bandwidthHistory.points, {
              timestamp: nodes.dataUpdatedAt,
              bandwidth_bps: health.egressBps,
            }),
    })
  }
  const refreshing =
    overview.isFetching ||
    traffic.isFetching ||
    nodes.isFetching ||
    tasks.isFetching
  return (
    <Page
      title='概览'
      description='边缘网络的流量、健康状况与待处理事项。'
      meta={
        <span>
          流量统计基于访问日志，只计入启用访问日志的网站
          {traffic.data &&
            `（${traffic.data.coverage.logging_website_count} / ${traffic.data.coverage.website_count}）`}
        </span>
      }
      actions={
        <Button
          size='sm'
          variant='tertiary'
          isPending={refreshing}
          // The first three share one connection: the first refetch
          // reconnects it and the others await its fresh frames; tasks take
          // a new one-shot snapshot.
          onPress={() => {
            void overview.refetch()
            void traffic.refetch()
            void nodes.refetch()
            void tasks.refetch()
          }}
        >
          <ArrowRotateLeft className='size-4' />
          刷新
        </Button>
      }
    >
      <OverviewKpis
        overview={overview.data}
        traffic={traffic.data}
        egressBps={health?.egressBps}
        bandwidthHistory={bandwidthHistory.points}
        isBandwidthPending={nodes.isPending}
        isOverviewPending={overview.isPending}
        isTrafficPending={traffic.isPending}
      />
      <QueryNotice
        query={overview}
        onRetry={overview.refetch}
        title='概览数据加载失败'
      />
      <div className='grid gap-6 lg:grid-cols-12'>
        <BandwidthPanel query={traffic} />
        <AttentionPanel
          overview={overview.data}
          failedTasks={tasks.isPending ? undefined : (tasks.data?.failed ?? 0)}
          isLoading={overview.isPending || tasks.isPending}
        />
      </div>
      <div className='grid gap-6 lg:grid-cols-12'>
        <TopWebsitesPanel query={traffic} />
        <StatusClassPanel query={traffic} />
      </div>
      <NodeHealthPanel query={nodes} nodes={health?.ordered} />
      <RecentTasksPanel query={tasks} />
    </Page>
  )
}

/** Primary metrics follow the chart-bottom KPI layout; secondary metrics
 * use a compact horizontal layout without inventing comparison trends. */
function OverviewKpis({
  overview,
  traffic,
  egressBps,
  bandwidthHistory,
  isBandwidthPending,
  isOverviewPending,
  isTrafficPending,
}: {
  overview?: Overview
  traffic?: OverviewTraffic
  egressBps?: number
  bandwidthHistory: BandwidthPoint[]
  isBandwidthPending: boolean
  isOverviewPending: boolean
  isTrafficPending: boolean
}) {
  const resources = overview?.resources
  const summary = traffic?.summary
  const offline = resources
    ? resources.node_count - resources.online_node_count
    : 0
  const online = resources
    ? ratioOf(resources.online_node_count, resources.node_count)
    : 0
  const valueSkeleton = <Skeleton className='h-8 w-24 rounded-md' />
  const compactSkeleton = <Skeleton className='h-7 w-16 rounded-md' />
  const missingValue = <KPI.Value value={0}>{() => '—'}</KPI.Value>
  return (
    <div className='flex flex-col gap-3'>
      <div className='grid grid-cols-1 gap-3 lg:grid-cols-3'>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title className='truncate'>
              当前带宽
              {resources &&
                resources.node_count > overviewNodePageSize &&
                `（前 ${overviewNodePageSize} 个节点）`}
            </KPI.Title>
            <span className='ml-auto shrink-0 text-xs text-muted'>实时</span>
          </KPI.Header>
          <KPI.Content className='min-h-8'>
            {isBandwidthPending ? (
              valueSkeleton
            ) : (
              <KpiQuantity value={egressBps} unit='bps' />
            )}
          </KPI.Content>
          <KPI.Chart
            data={bandwidthHistory}
            dataKey='bandwidth_bps'
            color='var(--color-accent)'
            height={64}
            strokeWidth={1.5}
            areaProps={{ dot: bandwidthHistory.length === 1 }}
            aria-label='当前出口带宽近期趋势'
            title='本次打开概览后收到的最近 60 次节点上报'
            tooltip={
              <AreaChart.Tooltip
                content={({ active, payload }) => {
                  const point = payload?.[0]?.payload as
                    BandwidthPoint | undefined
                  if (!active || !point) return null
                  return (
                    <ChartTooltip>
                      <ChartTooltip.Header>
                        {formatDate(new Date(point.timestamp).toISOString())}
                      </ChartTooltip.Header>
                      <ChartTooltip.Item>
                        <ChartTooltip.Indicator color='var(--color-accent)' />
                        <ChartTooltip.Label>实时出口带宽</ChartTooltip.Label>
                        <ChartTooltip.Value>
                          {formatBitsPerSecond(point.bandwidth_bps)}
                        </ChartTooltip.Value>
                      </ChartTooltip.Item>
                    </ChartTooltip>
                  )
                }}
              />
            }
          />
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>24 小时请求数</KPI.Title>
          </KPI.Header>
          <KPI.Content className='min-h-8'>
            {summary ? (
              <KPI.Value
                value={summary.request_count}
                notation='compact'
                maximumFractionDigits={1}
                className='whitespace-nowrap'
              />
            ) : isTrafficPending ? (
              valueSkeleton
            ) : (
              missingValue
            )}
          </KPI.Content>
          <KPI.Chart
            data={traffic?.hourly ?? []}
            dataKey='request_count'
            color='var(--color-accent)'
            height={64}
            strokeWidth={1.5}
            aria-label='过去 24 小时每小时请求数趋势'
            tooltip={
              <AreaChart.Tooltip
                content={({ active, payload }) => {
                  const point = payload?.[0]?.payload as
                    OverviewTraffic['hourly'][number] | undefined
                  if (!active || !point) return null
                  return (
                    <ChartTooltip>
                      <ChartTooltip.Header>
                        {formatDate(point.timestamp).slice(0, 16)}
                      </ChartTooltip.Header>
                      <ChartTooltip.Item>
                        <ChartTooltip.Indicator color='var(--color-accent)' />
                        <ChartTooltip.Label>请求数</ChartTooltip.Label>
                        <ChartTooltip.Value>
                          {formatNumber(point.request_count)} 次
                        </ChartTooltip.Value>
                      </ChartTooltip.Item>
                    </ChartTooltip>
                  )
                }}
              />
            }
          />
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>24 小时响应流量</KPI.Title>
          </KPI.Header>
          <KPI.Content className='min-h-8'>
            {summary ? (
              <KpiQuantity value={summary.response_bytes} unit='bytes' />
            ) : isTrafficPending ? (
              valueSkeleton
            ) : (
              missingValue
            )}
          </KPI.Content>
          <KPI.Chart
            data={traffic?.hourly ?? []}
            dataKey='response_bytes'
            color='var(--color-accent)'
            height={64}
            strokeWidth={1.5}
            aria-label='过去 24 小时每小时响应流量趋势'
            tooltip={
              <AreaChart.Tooltip
                content={({ active, payload }) => {
                  const point = payload?.[0]?.payload as
                    OverviewTraffic['hourly'][number] | undefined
                  if (!active || !point) return null
                  return (
                    <ChartTooltip>
                      <ChartTooltip.Header>
                        {formatDate(point.timestamp).slice(0, 16)}
                      </ChartTooltip.Header>
                      <ChartTooltip.Item>
                        <ChartTooltip.Indicator color='var(--color-accent)' />
                        <ChartTooltip.Label>响应流量</ChartTooltip.Label>
                        <ChartTooltip.Value>
                          {formatBytes(point.response_bytes)}
                        </ChartTooltip.Value>
                      </ChartTooltip.Item>
                    </ChartTooltip>
                  )
                }}
              />
            }
          />
        </KPI>
      </div>
      <div className='grid grid-cols-1 gap-3 sm:grid-cols-3'>
        <KPI className='min-w-0 flex-row items-center gap-3'>
          <KPI.Header className='mb-0 min-w-0 flex-1'>
            <KPI.Icon
              status={
                summary
                  ? summary.status_5xx_count > 0
                    ? 'danger'
                    : 'success'
                  : undefined
              }
            >
              <CircleExclamation />
            </KPI.Icon>
            <KPI.Title className='truncate'>5xx比例</KPI.Title>
          </KPI.Header>
          <KPI.Content className='min-h-8 flex-none [&_.kpi__value]:text-xl'>
            {summary ? (
              <KPI.Value
                value={ratioOf(summary.status_5xx_count, summary.request_count)}
                style='percent'
                maximumFractionDigits={2}
                className='whitespace-nowrap'
              />
            ) : isTrafficPending ? (
              compactSkeleton
            ) : (
              missingValue
            )}
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0 flex-row items-center gap-3'>
          <KPI.Header className='mb-0 min-w-0 flex-1'>
            <KPI.Icon
              status={
                resources
                  ? offline === 0
                    ? 'success'
                    : online === 0
                      ? 'danger'
                      : 'warning'
                  : undefined
              }
            >
              <Server />
            </KPI.Icon>
            <KPI.Title className='truncate'>在线节点</KPI.Title>
          </KPI.Header>
          <KPI.Content className='min-h-8 flex-none [&_.kpi__value]:text-xl'>
            {resources ? (
              <KPI.Value
                value={resources.online_node_count}
                className='whitespace-nowrap'
              >
                {(formatted) => (
                  <>
                    {formatted}
                    <span className='ms-1 text-sm font-normal text-muted'>
                      / {formatNumber(resources.node_count)}
                    </span>
                  </>
                )}
              </KPI.Value>
            ) : isOverviewPending ? (
              compactSkeleton
            ) : (
              missingValue
            )}
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0 flex-row items-center gap-3'>
          <KPI.Header className='mb-0 min-w-0 flex-1'>
            <KPI.Icon className='text-muted'>
              <Globe />
            </KPI.Icon>
            <KPI.Title className='truncate'>网站</KPI.Title>
          </KPI.Header>
          <KPI.Content className='min-h-8 flex-none [&_.kpi__value]:text-xl'>
            {resources ? (
              <KPI.Value value={resources.website_count} />
            ) : isOverviewPending ? (
              compactSkeleton
            ) : (
              missingValue
            )}
          </KPI.Content>
        </KPI>
      </div>
    </div>
  )
}

function AttentionPanel({
  overview,
  failedTasks,
  isLoading,
}: {
  overview?: Overview
  failedTasks: number | undefined
  isLoading: boolean
}) {
  const items =
    overview && failedTasks !== undefined
      ? attentionItems(overview, failedTasks)
      : undefined
  return (
    <Widget className='min-w-0 lg:col-span-4'>
      <Widget.Header>
        <Widget.Title>需要关注</Widget.Title>
      </Widget.Header>
      <Widget.Content className='flex min-h-[292px] flex-col justify-center p-1.5'>
        {!items ? (
          isLoading && (
            <div className='flex flex-col gap-2 p-2.5'>
              {Array.from({ length: 4 }, (_, index) => (
                <Skeleton key={index} className='h-12 w-full rounded-lg' />
              ))}
            </div>
          )
        ) : (
          <ListView
            aria-label='需要关注'
            variant='secondary'
            items={items}
            renderEmptyState={() => (
              <EmptyState
                size='sm'
                icon={<CircleCheck />}
                title='一切正常'
                description='没有同步异常、到期证书、需关注任务或未在线节点。'
              />
            )}
          >
            {(item) => (
              <ListView.Item
                id={item.id}
                href={item.href}
                textValue={item.label}
                className='rounded-lg'
              >
                <ListView.ItemContent>
                  <div className='flex min-w-0 flex-col'>
                    <ListView.Title>{item.label}</ListView.Title>
                    <ListView.Description>
                      {item.description}
                    </ListView.Description>
                  </div>
                </ListView.ItemContent>
                <ListView.ItemAction className='flex items-center gap-2'>
                  <Chip
                    size='sm'
                    variant='soft'
                    color={item.tone}
                    className='tabular-nums'
                  >
                    <Chip.Label>{formatNumber(item.count)}</Chip.Label>
                  </Chip>
                  <ChevronRight className='size-4 text-muted' />
                </ListView.ItemAction>
              </ListView.Item>
            )}
          </ListView>
        )}
      </Widget.Content>
    </Widget>
  )
}

const nodeHealthColumns: DataGridColumn<Node>[] = [
  {
    id: 'node',
    header: '节点',
    isRowHeader: true,
    cell: (node) => (
      <Link href={`/nodes/${node.id}`} className='truncate font-medium'>
        {node.name}
      </Link>
    ),
  },
  {
    id: 'cluster',
    header: '集群',
    cell: (node) => (
      <span className='truncate'>{node.cluster_name || '—'}</span>
    ),
  },
  {
    id: 'connection',
    header: '在线状态',
    cell: (node) => <NodeConnectionChip runtime={node.runtime} />,
  },
  // CPU and memory count only while the node is online; otherwise the
  // meters keep their shape with «—».
  {
    id: 'cpu',
    header: 'CPU',
    cell: (node) => (
      <MetricMeter
        label={`${node.name} CPU`}
        value={
          node.runtime.connection_status === 'online'
            ? node.runtime.cpu_usage
            : undefined
        }
        className='w-32'
      />
    ),
  },
  {
    id: 'memory',
    header: '内存',
    cell: (node) => (
      <MetricMeter
        label={`${node.name} 内存`}
        value={
          node.runtime.connection_status === 'online'
            ? node.runtime.memory_usage
            : undefined
        }
        className='w-32'
      />
    ),
  },
]

/** Six nodes, offline first, then by CPU load. */
function NodeHealthPanel({
  query,
  nodes,
}: {
  query: {
    isPending: boolean
    isError: boolean
    error: unknown
    refetch: () => unknown
  }
  nodes?: Node[]
}) {
  return (
    <Widget className='min-w-0'>
      <Widget.Header>
        <Widget.Title>节点健康</Widget.Title>
      </Widget.Header>
      <Widget.Content className='p-0'>
        <DataTable
          label='节点健康'
          variant='secondary'
          columns={nodeHealthColumns}
          data={nodes?.slice(0, 6)}
          isLoading={query.isPending}
          error={query.isError ? query.error : undefined}
          onRetry={query.refetch}
          empty={{
            title: '还没有节点',
            description: '添加节点并完成接入后，这里会显示运行状况。',
          }}
        />
      </Widget.Content>
    </Widget>
  )
}

const taskColumns: DataGridColumn<Task>[] = [
  {
    id: 'task',
    header: '任务',
    isRowHeader: true,
    cell: (task) => (
      <Link
        href={taskHref(taskRefOf(task))}
        className='block truncate font-medium'
      >
        {taskTitle(task)}
      </Link>
    ),
  },
  {
    id: 'status',
    header: '状态',
    cell: (task) => (
      <StatusChip
        {...describeStatus(taskStatuses, task.status)}
        reserve={taskStatusReserve}
      />
    ),
  },
  {
    id: 'updated_at',
    header: '最近执行',
    cell: (task) => (
      <RelativeTime value={task.updated_at} className='text-muted' />
    ),
  },
]

function RecentTasksPanel({
  query,
}: {
  query: {
    data?: { list: Task[] }
    isPending: boolean
    isError: boolean
    error: unknown
    refetch: () => unknown
  }
}) {
  return (
    <Widget className='min-w-0'>
      <Widget.Header>
        <Widget.Title>最近任务</Widget.Title>
      </Widget.Header>
      <Widget.Content className='p-0'>
        <DataTable
          label='最近任务'
          variant='secondary'
          columns={taskColumns}
          data={query.data && recentTaskOrder(query.data.list)}
          getRowId={(task) => taskKey(taskRefOf(task))}
          isLoading={query.isPending}
          error={query.isError ? query.error : undefined}
          onRetry={query.refetch}
          empty={{ title: '暂无任务' }}
        />
      </Widget.Content>
    </Widget>
  )
}
