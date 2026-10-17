import { keepPreviousData, useQueries, useQuery } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Tooltip } from '@heroui/react'
import { formatBitsPerSecond, formatBytes, formatNumber } from '@/lib/format'
import { hasListFilters, type ListSearchState } from '@/lib/list-search'
import { describeStatus, statusOptions } from '@/lib/status'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { MetricMeter } from '@/components/metric-meter'
import { Mono } from '@/components/mono'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import {
  clusterOptionsQuery,
  clusterSnapshotQuery,
} from '@/features/clusters/data'
import { dnsZoneLinesQuery } from '@/features/dns-zones/data'
import { dnsLineName } from '@/features/dns-zones/dns-lines'
import type { DnsLine } from '@/features/dns-zones/types'
import { nodesQuery } from './data'
import { NodeConnectionChip } from './node-connection-chip'
import { NodeCreateSheet } from './node-create-sheet'
import {
  connectionStatuses,
  liveMetrics,
  nodeStatusReserve,
  nodeStatuses,
} from './node-display'
import { nodeFilterKeys, type NodeListSearch } from './node-search'
import type { Node } from './types'

const connectionOptions = statusOptions(connectionStatuses)
const nodeStatusOptions = statusOptions(nodeStatuses)

/** Keep the endpoint order and line height aligned with the IP column.
 * Queries are shared by zone and never open another live stream. */
function NodeDnsLines({ node, lines }: { node: Node; lines: DnsLine[] }) {
  const [primary, ...others] = node.config.endpoints
  if (!primary) return <span className='text-muted'>—</span>
  const names = node.config.endpoints.map((endpoint) =>
    dnsLineName(lines, endpoint.line_code)
  )
  const allNames = names.join('\n')
  return (
    <div className='flex min-w-0 items-center gap-1.5'>
      <span
        title={names[0]}
        className='max-w-40 min-w-0 truncate text-[13px] text-muted'
      >
        {names[0]}
      </span>
      {others.length > 0 && (
        <Tooltip delay={300}>
          <Tooltip.Trigger
            aria-label={allNames}
            className='shrink-0 text-xs text-muted tabular-nums'
          >
            +{others.length}
          </Tooltip.Trigger>
          <Tooltip.Content className='whitespace-pre-line'>
            {allNames}
          </Tooltip.Content>
        </Tooltip>
      )}
    </div>
  )
}

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
  const clusterSnapshot = useQuery({
    ...clusterSnapshotQuery(clusterId ?? ''),
    enabled: clusterId !== undefined,
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
  const clusterZoneById = new Map(
    (clusters.data ?? []).map((cluster) => [cluster.id, cluster.dns_zone_id])
  )
  if (clusterSnapshot.data)
    clusterZoneById.set(
      clusterSnapshot.data.id,
      clusterSnapshot.data.dns_zone_id
    )
  const zoneIds = [
    ...new Set(
      (query.data?.list ?? [])
        .map((node) => clusterZoneById.get(node.cluster_id))
        .filter((zoneId): zoneId is string => !!zoneId)
    ),
  ]
  const zoneLineResults = useQueries({
    queries: zoneIds.map((zoneId) => dnsZoneLinesQuery(zoneId)),
  })
  const linesByZoneId = new Map(
    zoneIds.map((zoneId, index) => [zoneId, zoneLineResults[index]?.data ?? []])
  )
  const filterKeys = nodeFilterKeys.filter(
    (key) => clusterId === undefined || key !== 'cluster_id'
  )
  const filtered = hasListFilters(search, filterKeys)
  const columns: DataGridColumn<Node>[] = [
    ...(clusterId === undefined
      ? [
          {
            id: 'cluster',
            header: '集群',
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
      id: 'name',
      header: '节点',
      isRowHeader: true,
      cell: (node) => (
        <span className='block truncate font-medium'>{node.name}</span>
      ),
    },
    {
      id: 'ip',
      header: 'IP',
      cell: (node) => {
        const [primary, ...others] = node.config.endpoints
        if (!primary) return <span className='text-muted'>—</span>
        const allIps = node.config.endpoints
          .map((endpoint) => endpoint.ip_address)
          .join('\n')
        return (
          <div className='flex min-w-0 items-center gap-1.5'>
            <Mono truncate className='min-w-0'>
              {primary.ip_address}
            </Mono>
            {others.length > 0 && (
              <Tooltip delay={300}>
                <Tooltip.Trigger
                  aria-label={allIps}
                  className='shrink-0 text-xs text-muted tabular-nums'
                >
                  +{others.length}
                </Tooltip.Trigger>
                <Tooltip.Content className='whitespace-pre-line'>
                  {allIps}
                </Tooltip.Content>
              </Tooltip>
            )}
          </div>
        )
      },
    },
    {
      id: 'dns-line',
      header: '线路',
      cell: (node) => {
        const zoneId = clusterZoneById.get(node.cluster_id)
        return (
          <NodeDnsLines
            node={node}
            lines={zoneId ? (linesByZoneId.get(zoneId) ?? []) : []}
          />
        )
      },
    },
    {
      id: 'cpu',
      header: 'CPU',
      cell: (node) => (
        <MetricMeter
          label={`${node.name} CPU`}
          value={liveMetrics(node.runtime).cpu_usage}
          capacity={
            node.runtime.cpu_count === undefined
              ? '—'
              : `${formatNumber(node.runtime.cpu_count)} 核`
          }
          className='w-24'
        />
      ),
    },
    {
      id: 'memory',
      header: '内存',
      cell: (node) => (
        <MetricMeter
          label={`${node.name} 内存`}
          value={liveMetrics(node.runtime).memory_usage}
          capacity={formatBytes(node.runtime.memory_total_bytes)}
          className='w-32'
        />
      ),
    },
    {
      id: 'disk',
      header: '磁盘',
      align: 'end',
      cell: (node) => (
        <span
          className='text-sm tabular-nums'
          title='节点工作目录所在文件系统的总容量'
        >
          {formatBytes(node.runtime.disk_total_bytes)}
        </span>
      ),
    },
    {
      id: 'traffic',
      header: '带宽',
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
      align: 'end',
      cell: (node) => (
        <span className='text-sm tabular-nums'>
          {formatNumber(liveMetrics(node.runtime).connection_count)}
        </span>
      ),
    },
    {
      id: 'connected-ips',
      header: 'IP数',
      align: 'end',
      cell: (node) => (
        <span className='text-sm tabular-nums'>
          {formatNumber(liveMetrics(node.runtime).connected_ip_count)}
        </span>
      ),
    },
    {
      id: 'connection',
      header: '在线状态',
      cell: (node) => <NodeConnectionChip runtime={node.runtime} />,
    },
    {
      id: 'status',
      header: '状态',
      cell: (node) => (
        <StatusChip
          {...describeStatus(nodeStatuses, node.status)}
          reserve={nodeStatusReserve}
        />
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
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
              options={nodeStatusOptions}
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
