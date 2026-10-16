import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { formatBitsPerSecond, formatNumber } from '@/lib/format'
import { hasListFilters, type ListSearchState } from '@/lib/list-search'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { MetricMeter } from '@/components/metric-meter'
import { Mono } from '@/components/mono'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { clusterOptionsQuery } from '@/features/clusters/data'
import { nodesQuery } from './data'
import { NodeConnectionChip } from './node-connection-chip'
import { NodeCreateSheet } from './node-create-sheet'
import { nodeFilterKeys, type NodeListSearch } from './node-search'
import { liveMetrics, nodeStatusLabels } from './node-status'
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

/** Node list with toolbar, pagination and the add-node sheet, whose open
 * state belongs to the page (`/nodes?create=1`, or component state on the
 * cluster detail). With `clusterId` the list is fixed to that cluster: no
 * cluster filter or column, and new nodes are created in it. */
export function NodesTable({
  list,
  clusterId,
  isCreating,
  onCreatingChange,
}: {
  list: ListSearchState<NodeListSearch>
  clusterId?: string
  isCreating: boolean
  onCreatingChange: (creating: boolean) => void
}) {
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
      minWidth: 130,
      cell: (node) => (
        <span className='block truncate font-medium'>{node.name}</span>
      ),
    },
    {
      id: 'ip',
      header: 'IP',
      minWidth: 136,
      cell: (node) =>
        node.config.endpoints[0] ? (
          <span className='flex min-w-0 items-center gap-1.5'>
            <Mono truncate className='min-w-0'>
              {node.config.endpoints[0].ip_address}
            </Mono>
            {node.config.endpoints.length > 1 && (
              <span className='shrink-0 text-muted'>
                +{node.config.endpoints.length - 1}
              </span>
            )}
          </span>
        ) : (
          <span className='text-muted'>—</span>
        ),
    },
    ...(clusterId === undefined
      ? [
          {
            id: 'cluster',
            header: '集群',
            minWidth: 110,
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
      header: '在线状态',
      minWidth: 100,
      cell: (node) => <NodeConnectionChip runtime={node.runtime} />,
    },
    {
      id: 'cpu',
      header: 'CPU',
      minWidth: 96,
      cell: (node) => (
        <MetricMeter
          label={`${node.name} CPU`}
          value={liveMetrics(node.runtime).cpu_usage}
          className='w-16'
        />
      ),
    },
    {
      id: 'memory',
      header: '内存',
      minWidth: 96,
      cell: (node) => (
        <MetricMeter
          label={`${node.name} 内存`}
          value={liveMetrics(node.runtime).memory_usage}
          className='w-16'
        />
      ),
    },
    {
      id: 'traffic',
      header: '带宽',
      minWidth: 100,
      align: 'end',
      cell: (node) => (
        <span className='text-sm tabular-nums'>
          {formatBitsPerSecond(liveMetrics(node.runtime).traffic_out_bps)}
        </span>
      ),
    },
    {
      id: 'connections',
      header: '连接数',
      minWidth: 80,
      align: 'end',
      cell: (node) => (
        <span className='text-sm tabular-nums'>
          {formatNumber(liveMetrics(node.runtime).connection_count)}
        </span>
      ),
    },
    {
      id: 'connected-ips',
      header: '连接 IP',
      minWidth: 80,
      align: 'end',
      cell: (node) => (
        <span className='text-sm tabular-nums'>
          {formatNumber(liveMetrics(node.runtime).connected_ip_count)}
        </span>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 90,
      cell: (node) => (
        <StatusChip status={node.status} reserve={nodeStatusLabels} />
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      width: 72,
      cell: (node) => (
        <LinkButton
          href={`/nodes/${node.id}`}
          className='px-2'
          aria-label={`节点 ${node.name} 的详情`}
        >
          详情
        </LinkButton>
      ),
    },
  ]
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
              label='在线状态'
              value={search.connection_status ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  connection_status: (value ||
                    undefined) as NodeListSearch['connection_status'],
                })
              }
              options={connectionOptions}
            />
            <FilterSelect
              label='状态'
              value={search.status ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  status: (value || undefined) as NodeListSearch['status'],
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
        actions={
          <Button size='sm' onPress={() => onCreatingChange(true)}>
            <Plus className='size-4' />
            添加节点
          </Button>
        }
      />
      <DataTable
        label='节点列表'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => void query.refetch()}
        minWidth={clusterId === undefined ? 1080 : 960}
        empty={
          filtered
            ? {
                title: '没有匹配的节点',
                description: '调整搜索或筛选条件后重试。',
              }
            : {
                title: '还没有边缘节点',
                description: '添加节点后，用接入凭据在服务器上启动节点程序。',
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
        isOpen={isCreating}
        onClose={() => onCreatingChange(false)}
        clusterId={clusterId}
      />
    </div>
  )
}
