import { useQuery } from '@tanstack/react-query'
import { ArrowRotateLeft, ChevronRight, CircleCheck } from '@gravity-ui/icons'
import { ListView, Widget, type DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, Chip, Link, Skeleton } from '@heroui/react'
import { formatBitsPerSecond, formatNumber, ratioOf } from '@/lib/format'
import { DataTable, TruncatedText } from '@/components/data-table'
import { KpiQuantity } from '@/components/kpi-quantity'
import { LinkButton } from '@/components/link-button'
import { MetricMeter } from '@/components/metric-meter'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { NodeConnectionChip } from '@/features/nodes/node-connection-chip'
import type { Node } from '@/features/nodes/types'
import {
  taskHref,
  taskResourceTarget,
  taskStatusLabels,
  taskStatuses,
  taskStatusTones,
  taskTitle,
} from '@/features/tasks/task-display'
import {
  attentionItems,
  nodeHealth,
  overviewNodePageSize,
  overviewNodesQuery,
  overviewQuery,
  overviewTrafficQuery,
  type Overview,
  type OverviewMarker,
  type OverviewTraffic,
} from './data'
import {
  BandwidthPanel,
  StatusClassPanel,
  TopWebsitesPanel,
} from './traffic-panels'

/** Overview dashboard. Holds one live stream while mounted, the combined
 * `/api/overview/stream` (events `snapshot`, `traffic`, `nodes`,
 * `node-runtime`); it is released when the page unmounts. */
export function OverviewPage() {
  const overview = useQuery(overviewQuery)
  const traffic = useQuery(overviewTrafficQuery)
  const nodes = useQuery(overviewNodesQuery)
  const health = nodes.data ? nodeHealth(nodes.data.list) : undefined
  const refreshing =
    overview.isFetching || traffic.isFetching || nodes.isFetching
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
          // All three share one connection: the first refetch reconnects it
          // and the others await the fresh frames of that connection.
          onPress={() => {
            void overview.refetch()
            void traffic.refetch()
            void nodes.refetch()
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
          isLoading={overview.isPending}
        />
      </div>
      <div className='grid gap-6 lg:grid-cols-12'>
        <TopWebsitesPanel query={traffic} />
        <StatusClassPanel query={traffic} />
      </div>
      <NodeHealthPanel query={nodes} nodes={health?.ordered} />
      <RecentTasksPanel query={overview} />
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
  const valueSkeleton = <Skeleton className='h-8 w-24 rounded-md' />
  return (
    <div className='grid grid-cols-2 gap-3 md:grid-cols-3 2xl:grid-cols-6'>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>当前带宽</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          {egressBps === undefined ? (
            valueSkeleton
          ) : (
            <KpiQuantity value={egressBps} unit='bps' />
          )}
        </KPI.Content>
        {resources && resources.node_count > overviewNodePageSize && (
          <KPI.Footer className='truncate text-xs text-muted'>
            按前 {overviewNodePageSize} 个节点统计
          </KPI.Footer>
        )}
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
        {summary && (
          <KPI.Footer className='truncate text-xs text-muted'>
            独立 IP {formatNumber(summary.unique_client_ips)}
          </KPI.Footer>
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
        {summary && (
          <KPI.Footer className='truncate text-xs text-muted'>
            峰值带宽 {formatBitsPerSecond(summary.peak_bandwidth_bps)}
          </KPI.Footer>
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
        {summary && (
          <KPI.Footer className='truncate text-xs text-muted'>
            {formatNumber(summary.status_5xx_count)} 次 5xx 响应
          </KPI.Footer>
        )}
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
        {resources && (
          <KPI.Footer className='truncate text-xs text-muted'>
            {resources.node_count === 0
              ? '还没有节点'
              : offline === 0
                ? '全部在线'
                : `${offline} 个未在线`}
          </KPI.Footer>
        )}
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
        {resources && (
          <KPI.Footer className='truncate text-xs text-muted'>
            {resources.cluster_count} 个集群 · {resources.certificate_count}{' '}
            张证书
          </KPI.Footer>
        )}
      </KPI>
    </div>
  )
}

function AttentionPanel({
  overview,
  isLoading,
}: {
  overview?: Overview
  isLoading: boolean
}) {
  const items = overview ? attentionItems(overview) : undefined
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
                description='没有同步异常、到期证书、待重试任务或未在线节点。'
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
      <div className='flex min-w-0 flex-col'>
        <Link href={`/nodes/${node.id}`} className='truncate font-medium'>
          {node.name}
        </Link>
        <span className='truncate text-xs text-muted'>
          {node.cluster_name || '—'}
        </span>
      </div>
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
        <LinkButton variant='ghost' href='/nodes'>
          全部节点
        </LinkButton>
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

const markerColumns: DataGridColumn<OverviewMarker>[] = [
  {
    id: 'task',
    header: '任务',
    isRowHeader: true,
    minWidth: 140,
    cell: (marker) => (
      <Link href={taskHref(marker)} className='font-medium'>
        {taskTitle(marker)}
      </Link>
    ),
  },
  {
    id: 'resource',
    header: '关联资源',
    minWidth: 180,
    cell: (marker) => {
      const target = taskResourceTarget({
        resource_type: marker.resource_type,
        resource_id: marker.resource_id,
        name: marker.resource_name,
      })
      return (
        <Link
          href={target.href}
          routerOptions={target.state && { state: target.state }}
          className='truncate'
        >
          {marker.resource_name}
        </Link>
      )
    },
  },
  {
    id: 'status',
    header: '状态',
    minWidth: 100,
    cell: (marker) => (
      <StatusChip
        status={marker.status}
        label={taskStatuses[marker.status as keyof typeof taskStatuses]}
        tone={taskStatusTones[marker.status as keyof typeof taskStatusTones]}
        reserve={taskStatusLabels}
      />
    ),
  },
  {
    id: 'error',
    header: '最近错误',
    minWidth: 200,
    cell: (marker) => (
      <TruncatedText
        text={marker.last_error}
        className='max-w-72 text-xs text-muted'
      />
    ),
  },
  {
    id: 'updated_at',
    header: '更新时间',
    cell: (marker) => (
      <RelativeTime value={marker.updated_at} className='text-muted' />
    ),
  },
]

function RecentTasksPanel({
  query,
}: {
  query: {
    data?: { recent_markers: OverviewMarker[] }
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
        <LinkButton variant='ghost' href='/tasks'>
          任务中心
        </LinkButton>
      </Widget.Header>
      <Widget.Content className='p-0'>
        <DataTable
          label='最近任务'
          columns={markerColumns}
          data={query.data?.recent_markers.slice(0, 6)}
          isLoading={query.isPending}
          error={query.isError ? query.error : undefined}
          onRetry={query.refetch}
          minWidth={760}
          empty={{
            title: '暂无任务',
            description: '创建资源或提交配置后，可在这里查看执行情况。',
          }}
        />
      </Widget.Content>
    </Widget>
  )
}
