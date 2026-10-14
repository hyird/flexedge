import { useQuery } from '@tanstack/react-query'
import {
  ArrowRotateLeft,
  ChevronRight,
  CircleCheck,
  Server,
} from '@gravity-ui/icons'
import { KPIGroup, type DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Button, Link, Skeleton } from '@heroui/react'
import {
  formatBitsPerSecond,
  formatBytes,
  formatNumber,
  formatPercent,
  ratioOf,
} from '@/lib/format'
import { cn } from '@/lib/utils'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { MetricMeter } from '@/components/metric-meter'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import type { Node } from '@/features/nodes/types'
import {
  taskHref,
  taskResourceHref,
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
  type NodeHealth,
  type Overview,
  type OverviewMarker,
} from './data'
import {
  BandwidthPanel,
  StatusClassPanel,
  TopWebsitesPanel,
} from './traffic-panels'

type Kpi = {
  id: string
  title: string
  value?: string
  detail?: string
}

/** Overview dashboard. Holds one live stream while mounted, the combined
 * `/api/overview/stream` (events `snapshot`, `traffic`, `nodes`,
 * `node-runtime`); it is released when the page unmounts. */
export function OverviewPage() {
  const overview = useQuery(overviewQuery)
  const traffic = useQuery(overviewTrafficQuery)
  const nodes = useQuery(overviewNodesQuery)
  const health = nodes.data ? nodeHealth(nodes.data.list) : undefined
  const resources = overview.data?.resources
  const summary = traffic.data?.summary
  const kpis: Kpi[] = [
    {
      id: 'egress',
      title: '当前出口带宽',
      value: health && formatBitsPerSecond(health.egressBps),
      detail:
        resources && resources.node_count > overviewNodePageSize
          ? `在线节点实时上报之和，按前 ${overviewNodePageSize} 个节点统计`
          : '在线节点实时上报之和',
    },
    {
      id: 'requests',
      title: '24 小时请求数',
      value: summary && formatNumber(summary.request_count, { compact: true }),
      detail: summary && `独立 IP ${formatNumber(summary.unique_client_ips)}`,
    },
    {
      id: 'bytes',
      title: '24 小时响应流量',
      value: summary && formatBytes(summary.response_bytes),
      detail:
        summary &&
        `峰值带宽 ${formatBitsPerSecond(summary.peak_bandwidth_bps)}`,
    },
    {
      id: 'errors',
      title: '5xx 比例',
      value:
        summary &&
        formatPercent(
          ratioOf(summary.status_5xx_count, summary.request_count),
          2
        ),
      detail:
        summary && `${formatNumber(summary.status_5xx_count)} 次 5xx 响应`,
    },
    {
      id: 'nodes',
      title: '在线节点',
      value:
        resources && `${resources.online_node_count} / ${resources.node_count}`,
      detail:
        resources &&
        (resources.node_count === 0
          ? '还没有节点'
          : resources.node_count === resources.online_node_count
            ? '全部在线'
            : `${resources.node_count - resources.online_node_count} 个未在线（离线或未接入）`),
    },
    {
      id: 'websites',
      title: '网站',
      value:
        overview.data && formatNumber(overview.data.resources.website_count),
      detail:
        overview.data &&
        `${overview.data.resources.cluster_count} 个集群 · ${overview.data.resources.certificate_count} 张证书`,
    },
  ]
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
      <KPIGroup
        aria-label='关键指标'
        className='grid grid-cols-2 gap-px bg-separator md:grid-cols-3 2xl:grid-cols-6'
      >
        {kpis.map((item) => (
          <KPI key={item.id} className='min-w-0 bg-surface'>
            <KPI.Header>
              <KPI.Title>{item.title}</KPI.Title>
            </KPI.Header>
            <KPI.Content>
              {item.value === undefined ? (
                <Skeleton className='h-8 w-24 rounded-md' />
              ) : (
                <dd className='truncate text-2xl font-semibold tracking-tight tabular-nums'>
                  {item.value}
                </dd>
              )}
            </KPI.Content>
            <KPI.Footer className='truncate text-xs text-muted'>
              {item.detail ?? ' '}
            </KPI.Footer>
          </KPI>
        ))}
      </KPIGroup>
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
        <div className='flex min-w-0 flex-col gap-6 lg:col-span-6'>
          <StatusClassPanel query={traffic} />
          <NodeHealthPanel query={nodes} health={health} />
        </div>
      </div>
      <RecentTasksPanel query={overview} />
    </Page>
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
    <Section
      title='需要关注'
      description='点击查看对应的筛选列表'
      className='lg:col-span-4'
    >
      {!items ? (
        isLoading && (
          <div className='flex flex-col gap-2'>
            {Array.from({ length: 4 }, (_, index) => (
              <Skeleton key={index} className='h-12 w-full rounded-lg' />
            ))}
          </div>
        )
      ) : items.length === 0 ? (
        <EmptyState
          size='sm'
          icon={<CircleCheck />}
          title='一切正常'
          description='没有同步异常、到期证书、待重试任务或未在线节点。'
        />
      ) : (
        <ul className='-mx-2 flex flex-col'>
          {items.map((item) => (
            <li key={item.id}>
              <Link
                href={item.href}
                className='flex w-full items-center gap-3 rounded-lg px-2 py-2.5 text-foreground no-underline hover:bg-default'
              >
                <span
                  aria-hidden
                  className={cn(
                    'size-2 shrink-0 rounded-full',
                    item.tone === 'danger' ? 'bg-danger' : 'bg-warning'
                  )}
                />
                <span className='flex min-w-0 flex-1 flex-col'>
                  <span className='text-sm font-medium'>{item.label}</span>
                  <span className='truncate text-xs text-muted'>
                    {item.description}
                  </span>
                </span>
                <span
                  className={cn(
                    'text-base font-semibold tabular-nums',
                    item.tone === 'danger' ? 'text-danger' : 'text-warning'
                  )}
                >
                  {formatNumber(item.count)}
                </span>
                <ChevronRight className='size-4 shrink-0 text-muted' />
              </Link>
            </li>
          ))}
        </ul>
      )}
    </Section>
  )
}

function NodeHealthPanel({
  query,
  health,
}: {
  query: {
    isPending: boolean
    isError: boolean
    error: unknown
    refetch: () => unknown
  }
  health?: NodeHealth
}) {
  const shown = health?.ordered.slice(0, 6)
  return (
    <Section
      title='节点健康'
      description='离线优先，其次按 CPU 负载'
      actions={
        <LinkButton variant='ghost' href='/nodes'>
          全部节点
        </LinkButton>
      }
    >
      <QueryNotice query={query} onRetry={query.refetch} />
      {!shown ? (
        query.isPending && (
          <div className='flex flex-col gap-2'>
            {Array.from({ length: 4 }, (_, index) => (
              <Skeleton key={index} className='h-11 w-full rounded-lg' />
            ))}
          </div>
        )
      ) : shown.length === 0 ? (
        <EmptyState
          size='sm'
          icon={<Server />}
          title='还没有节点'
          description='添加节点并完成接入后，这里会显示运行状况。'
          action={<LinkButton href='/nodes?create=1'>添加节点</LinkButton>}
        />
      ) : (
        <ul className='flex flex-col divide-y divide-separator'>
          {shown.map((node) => (
            <NodeHealthRow key={node.id} node={node} />
          ))}
        </ul>
      )}
    </Section>
  )
}

function NodeHealthRow({ node }: { node: Node }) {
  const online = node.runtime.connection_status === 'online'
  return (
    <li className='flex flex-wrap items-center gap-x-4 gap-y-2 py-2.5'>
      <div className='flex min-w-0 flex-1 basis-40 flex-col'>
        <Link
          href={`/nodes/${node.id}`}
          className='truncate text-sm font-medium'
        >
          {node.name}
        </Link>
        <span className='truncate text-xs text-muted'>{node.cluster_name}</span>
      </div>
      <StatusChip status={node.runtime.connection_status} />
      {online ? (
        <div className='flex items-center gap-3 text-xs text-muted'>
          <span className='flex items-center gap-1.5'>
            CPU
            <MetricMeter
              label={`${node.name} CPU`}
              value={node.runtime.cpu_usage}
              className='w-24'
            />
          </span>
          <span className='flex items-center gap-1.5'>
            内存
            <MetricMeter
              label={`${node.name} 内存`}
              value={node.runtime.memory_usage}
              className='w-24'
            />
          </span>
        </div>
      ) : node.runtime.last_heartbeat_at ? (
        <span className='text-xs text-muted'>
          最后心跳 <RelativeTime value={node.runtime.last_heartbeat_at} />
        </span>
      ) : (
        <span className='text-xs text-muted'>尚未接入</span>
      )}
    </li>
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
    cell: (marker) => (
      <Link
        href={taskResourceHref({
          resource_type: marker.resource_type,
          resource_id: marker.resource_id,
          name: marker.resource_name,
        })}
        className='truncate'
      >
        {marker.resource_name}
      </Link>
    ),
  },
  {
    id: 'status',
    header: '状态',
    cell: (marker) => (
      <StatusChip
        status={marker.status}
        label={taskStatuses[marker.status as keyof typeof taskStatuses]}
        tone={taskStatusTones[marker.status as keyof typeof taskStatusTones]}
      />
    ),
  },
  {
    id: 'error',
    header: '最近错误',
    minWidth: 200,
    cell: (marker) => (
      <span
        className='line-clamp-1 text-xs text-muted'
        title={marker.last_error}
      >
        {marker.last_error || '—'}
      </span>
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
    <Section
      title='最近任务'
      actions={
        <LinkButton variant='ghost' href='/tasks'>
          任务中心
        </LinkButton>
      }
    >
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
    </Section>
  )
}
