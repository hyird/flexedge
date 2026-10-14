import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { Link, useNavigate } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { formatBitsPerSecond, formatNumber } from '@/lib/format'
import { hasListFilters, type ListSearchState } from '@/lib/list-search'
import { DataTable } from '@/components/data-table'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { MetricMeter } from '@/components/metric-meter'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { nodesQuery } from './data'
import { NodeActionMenu } from './node-action-menu'
import { NodeCreateSheet } from './node-create-sheet'
import { nodeFilterKeys, type NodesSearch } from './node-search'
import type { Node } from './types'

const connectionOptions = [
  { id: 'online', label: '在线' },
  { id: 'offline', label: '离线' },
  { id: 'unregistered', label: '未接入' },
]
const statusOptions = [
  { id: 'enabled', label: '已启用' },
  { id: 'disabled', label: '已停用' },
]

/** Node list with toolbar, pagination and the add-node flow (`?create=1`).
 * With `clusterId` the list is fixed to that cluster: no cluster filter or
 * column, and new nodes are created in it. */
export function NodesTable({
  list,
  clusterId,
}: {
  list: ListSearchState<NodesSearch>
  clusterId?: string
}) {
  const navigate = useNavigate()
  const { search } = list
  const clusters = useQuery({
    ...clusterOptionsQuery,
    enabled: clusterId === undefined,
  })
  const query = useQuery({
    ...nodesQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
      cluster_id: clusterId ?? search.cluster_id,
      status: search.status,
      connection_status: search.connection_status,
    }),
    placeholderData: keepPreviousData,
  })
  const filterKeys = nodeFilterKeys.filter(
    (key) => clusterId === undefined || key !== 'cluster_id'
  )
  const filtered = hasListFilters(search, filterKeys)
  const columns: DataGridColumn<Node>[] = [
    {
      id: 'name',
      header: '节点',
      isRowHeader: true,
      minWidth: 200,
      cell: (node) => (
        <div className='flex min-w-0 flex-col gap-0.5'>
          <Link
            to='/nodes/$nodeId'
            params={{ nodeId: node.id }}
            className='link truncate font-medium'
          >
            {node.name}
          </Link>
          <span className='flex min-w-0 items-center gap-1.5 text-xs text-muted'>
            {node.config.endpoints[0] && (
              <Mono className='text-xs'>
                {node.config.endpoints[0].ip_address}
              </Mono>
            )}
            {node.config.endpoints.length > 1 && (
              <span>+{node.config.endpoints.length - 1}</span>
            )}
          </span>
        </div>
      ),
    },
    ...(clusterId === undefined
      ? [
          {
            id: 'cluster',
            header: '集群',
            minWidth: 140,
            cell: (node: Node) => (
              <Link
                to='/clusters/$clusterId'
                params={{ clusterId: node.cluster_id }}
                className='link text-sm'
              >
                {node.cluster_name}
              </Link>
            ),
          },
        ]
      : []),
    {
      id: 'connection',
      header: '连接',
      minWidth: 110,
      cell: (node) => <StatusChip status={node.runtime.connection_status} />,
    },
    {
      id: 'cpu',
      header: 'CPU',
      minWidth: 140,
      cell: (node) => (
        <MetricMeter
          label={`${node.name} CPU`}
          value={node.runtime.cpu_usage}
        />
      ),
    },
    {
      id: 'memory',
      header: '内存',
      minWidth: 140,
      cell: (node) => (
        <MetricMeter
          label={`${node.name} 内存`}
          value={node.runtime.memory_usage}
        />
      ),
    },
    {
      id: 'traffic',
      header: '出口带宽',
      minWidth: 110,
      align: 'end',
      cell: (node) => (
        <span className='text-sm tabular-nums'>
          {formatBitsPerSecond(node.runtime.traffic_out_bps)}
        </span>
      ),
    },
    {
      id: 'connections',
      header: '连接数',
      minWidth: 90,
      align: 'end',
      cell: (node) => (
        <span className='text-sm tabular-nums'>
          {formatNumber(node.runtime.connection_count)}
        </span>
      ),
    },
    {
      id: 'heartbeat',
      header: '最近心跳',
      minWidth: 110,
      cell: (node) => (
        <RelativeTime
          value={node.runtime.last_heartbeat_at}
          className='text-[13px] text-muted'
        />
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 90,
      cell: (node) => <StatusChip status={node.status} />,
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      minWidth: 56,
      align: 'end',
      cell: (node) => <NodeActionMenu node={node} context='list' />,
    },
  ]
  const setCreate = (create: 1 | undefined) =>
    navigate({
      to: '.',
      search: (previous: Record<string, unknown>) => ({ ...previous, create }),
      replace: create === undefined,
    })
  const createButton = (
    <Button size='sm' onPress={() => void setCreate(1)}>
      <Plus className='size-4' />
      添加节点
    </Button>
  )
  return (
    <div className='flex flex-col gap-4'>
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索节点名称',
        }}
        filters={
          <>
            {clusterId === undefined && (
              <FilterSelect
                label='集群'
                value={search.cluster_id ?? ''}
                onChange={(value) =>
                  void list.setSearch({ cluster_id: value || undefined })
                }
                options={(clusters.data ?? []).map((cluster) => ({
                  id: cluster.id,
                  label: cluster.name,
                }))}
              />
            )}
            <FilterSelect
              label='连接'
              value={search.connection_status ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  connection_status: (value ||
                    undefined) as NodesSearch['connection_status'],
                })
              }
              options={connectionOptions}
            />
            <FilterSelect
              label='状态'
              value={search.status ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  status: (value || undefined) as NodesSearch['status'],
                })
              }
              options={statusOptions}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters(filterKeys)}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isFetching && !query.isPending}
      />
      <DataTable
        label='节点列表'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => void query.refetch()}
        minWidth={clusterId === undefined ? 1180 : 1040}
        empty={
          filtered
            ? {
                title: '没有匹配的节点',
                description: '调整搜索或筛选条件后重试。',
              }
            : {
                title: '还没有边缘节点',
                description: '添加节点后，用接入凭据在服务器上启动节点程序。',
                action: createButton,
              }
        }
        footer={
          <TablePagination
            page={list.page}
            pageSize={list.pageSize}
            total={query.data?.total ?? 0}
            onPageChange={list.setPage}
            onPageSizeChange={list.setPageSize}
          />
        }
      />
      <NodeCreateSheet
        isOpen={search.create === 1}
        onClose={() => void setCreate(undefined)}
        clusterId={clusterId}
      />
    </div>
  )
}
