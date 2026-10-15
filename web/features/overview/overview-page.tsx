import { useQuery } from '@tanstack/react-query'
import { ArrowRotateLeft, ChevronRight, CircleCheck } from '@gravity-ui/icons'
import { ListView, Widget, type DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, Chip, Link, Skeleton } from '@heroui/react'
import { formatNumber, ratioOf } from '@/lib/format'
import { DataTable } from '@/components/data-table'
import { KpiQuantity } from '@/components/kpi-quantity'
import { LinkButton } from '@/components/link-button'
import { MetricMeter } from '@/components/metric-meter'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { NodeConnectionChip } from '@/features/nodes/node-connection-chip'
import type { Node } from '@/features/nodes/types'
import { recentOperationsQuery } from '@/features/tasks/data'
import {
  actorLabel,
  describeOperationStatus,
  operationHref,
  operationStatusLabels,
  operationTitle,
} from '@/features/tasks/task-display'
import type { Operation } from '@/features/tasks/types'
import {
  attentionItems,
  nodeHealth,
  overviewNodePageSize,
  overviewNodesQuery,
  overviewQuery,
  overviewTrafficQuery,
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
 * `node-runtime`); it is released when the page unmounts. Recent tasks are a
 * one-shot read of the six newest (`/api/operations/stream`, closed after
 * its first frame). */
export function OverviewPage() {
  const overview = useQuery(overviewQuery)
  const traffic = useQuery(overviewTrafficQuery)
  const nodes = useQuery(overviewNodesQuery)
  const tasks = useQuery({
    ...recentOperationsQuery(6),
    meta: { inlineError: true },
  })
  const health = nodes.data ? nodeHealth(nodes.data.list) : undefined
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
          attentionTasks={
            tasks.isPending ? undefined : (tasks.data?.attention ?? 0)
          }
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

/** Six KPI cards. A card shows a skeleton until its stream event arrived;
 * request and response figures carry their 24-hour sparkline. */
function OverviewKpis({
  overview,
  traffic,
  egressBps,
}: {
  overview?: Overview
  traffic?: OverviewTraffic
  egressBps?: number
}) {
  const resources = overview?.resources
  const summary = traffic?.summary
  const offline = resources
    ? resources.node_count - resources.online_node_count
    : 0
  const online = resources
    ? ratioOf(resources.online_node_count, resources.node_count)
    : 0
  const valueSkeleton = <Skeleton className='h-7 w-24 rounded-md' />
  return (
    <div className='grid grid-cols-2 gap-3 md:grid-cols-3 2xl:grid-cols-6'>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>
            当前带宽
            {resources &&
              resources.node_count > overviewNodePageSize &&
              `（前 ${overviewNodePageSize} 个节点）`}
          </KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          {egressBps === undefined ? (
            valueSkeleton
          ) : (
            <KpiQuantity value={egressBps} unit='bps' />
          )}
        </KPI.Content>
      </KPI>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>24 小时请求数</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          {summary ? (
            <KPI.Value
              value={summary.request_count}
              notation='compact'
              maximumFractionDigits={1}
              className='whitespace-nowrap'
            />
          ) : (
            valueSkeleton
          )}
        </KPI.Content>
        {traffic && (
          <KPI.Chart
            data={traffic.hourly}
            dataKey='request_count'
            color='var(--color-accent)'
            height={40}
            strokeWidth={1.5}
          />
        )}
      </KPI>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>24 小时响应流量</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          {summary ? (
            <KpiQuantity value={summary.response_bytes} unit='bytes' />
          ) : (
            valueSkeleton
          )}
        </KPI.Content>
        {traffic && (
          <KPI.Chart
            data={traffic.hourly}
            dataKey='response_bytes'
            color='var(--color-accent)'
            height={40}
            strokeWidth={1.5}
          />
        )}
      </KPI>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>5xx 比例</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          {summary ? (
            <KPI.Value
              value={ratioOf(summary.status_5xx_count, summary.request_count)}
              style='percent'
              maximumFractionDigits={2}
            />
          ) : (
            valueSkeleton
          )}
        </KPI.Content>
      </KPI>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>在线节点</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          {resources ? (
            <KPI.Value
              value={resources.online_node_count}
              className='whitespace-nowrap'
            >
              {(formatted) => `${formatted} / ${resources.node_count}`}
            </KPI.Value>
          ) : (
            valueSkeleton
          )}
          {resources && (
            <KPI.Progress
              aria-label='节点在线比例'
              value={online * 100}
              status={
                offline === 0 ? 'success' : online === 0 ? 'danger' : 'warning'
              }
            />
          )}
        </KPI.Content>
      </KPI>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>网站</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          {resources ? (
            <KPI.Value value={resources.website_count} />
          ) : (
            valueSkeleton
          )}
        </KPI.Content>
      </KPI>
    </div>
  )
}

function AttentionPanel({
  overview,
  attentionTasks,
  isLoading,
}: {
  overview?: Overview
  attentionTasks: number | undefined
  isLoading: boolean
}) {
  const items =
    overview && attentionTasks !== undefined
      ? attentionItems(overview, attentionTasks)
      : undefined
  return (
    <Widget className='min-w-0 lg:col-span-4'>
      <Widget.Header>
        <Widget.Title>需要关注</Widget.Title>
      </Widget.Header>
      <Widget.Content className='p-1.5'>
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
    minWidth: 180,
    cell: (node) => (
      <Link href={`/nodes/${node.id}`} className='truncate font-medium'>
        {node.name}
      </Link>
    ),
  },
  {
    id: 'cluster',
    header: '集群',
    minWidth: 140,
    cell: (node) => (
      <span className='truncate'>{node.cluster_name || '—'}</span>
    ),
  },
  {
    id: 'connection',
    header: '在线状态',
    minWidth: 100,
    cell: (node) => <NodeConnectionChip runtime={node.runtime} />,
  },
  // CPU and memory count only while the node is online; otherwise the
  // meters keep their shape with «—».
  {
    id: 'cpu',
    header: 'CPU',
    minWidth: 160,
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
    minWidth: 160,
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
        <LinkButton href='/nodes'>全部节点</LinkButton>
      </Widget.Header>
      <Widget.Content className='p-0'>
        <DataTable
          label='节点健康'
          columns={nodeHealthColumns}
          data={nodes?.slice(0, 6)}
          isLoading={query.isPending}
          error={query.isError ? query.error : undefined}
          onRetry={query.refetch}
          minWidth={640}
          empty={{
            title: '还没有节点',
            description: '添加节点并完成接入后，这里会显示运行状况。',
            action: <LinkButton href='/nodes?create=1'>添加节点</LinkButton>,
          }}
        />
      </Widget.Content>
    </Widget>
  )
}

const taskColumns: DataGridColumn<Operation>[] = [
  {
    id: 'operation',
    header: '任务',
    isRowHeader: true,
    minWidth: 240,
    cell: (operation) => (
      <Link
        href={operationHref(operation.id)}
        className='block truncate font-medium'
      >
        {operationTitle(operation)}
      </Link>
    ),
  },
  {
    id: 'actor',
    header: '发起人',
    minWidth: 100,
    cell: (operation) => (
      <span className='block truncate'>{actorLabel(operation) || '—'}</span>
    ),
  },
  {
    id: 'status',
    header: '状态',
    minWidth: 100,
    cell: (operation) => {
      const status = describeOperationStatus(operation.status)
      return (
        <StatusChip
          label={status.label}
          tone={status.tone}
          reserve={operationStatusLabels}
        />
      )
    },
  },
  {
    id: 'created_at',
    header: '时间',
    minWidth: 100,
    cell: (operation) => (
      <RelativeTime value={operation.created_at} className='text-muted' />
    ),
  },
]

function RecentTasksPanel({
  query,
}: {
  query: {
    data?: { list: Operation[] }
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
        <LinkButton href='/tasks'>任务中心</LinkButton>
      </Widget.Header>
      <Widget.Content className='p-0'>
        <DataTable
          label='最近任务'
          columns={taskColumns}
          data={query.data?.list}
          isLoading={query.isPending}
          error={query.isError ? query.error : undefined}
          onRetry={query.refetch}
          minWidth={640}
          empty={{ title: '暂无任务' }}
        />
      </Widget.Content>
    </Widget>
  )
}
