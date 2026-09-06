import { useMemo, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { FileTerminal, KeyRound, Pencil, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { dnsLinePath } from '@/lib/dns-lines'
import { formatBytesPerSecond, formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import type { Cluster, DnsLine, DnsZone, Node, PageData } from '@/lib/types'
import { useResourceFilters } from '@/hooks/use-resource-filters'
import {
  DropdownMenuItem,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
import { DataTableCellContent } from '@/components/data-table/cell-content'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'
import { CredentialsDialog } from './credentials-dialog'
import { NodeDialog, type NodeCredentials } from './node-dialog'
import { NodeLogSheet } from './node-log-sheet'

export function NodesPanel({
  initialClusterId,
  createOpen = false,
  onCreateOpenChange,
  showClusterFilter = true,
}: {
  initialClusterId?: string
  createOpen?: boolean
  onCreateOpenChange?: (open: boolean) => void
  showClusterFilter?: boolean
}) {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const filter = useResourceFilters({
    keyword: '',
    clusterId: initialClusterId ?? 'all',
    status: 'all',
  })
  const { keyword, clusterId, status } = filter.filters
  const [dialog, setDialog] = useState<Node | 'new' | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Node | null>(null)
  const [credentials, setCredentials] = useState<NodeCredentials | null>(null)
  const [logNode, setLogNode] = useState<Node | null>(null)

  const clustersQuery = useQuery({
    queryKey: [...queryKeys.clusters, 'options'],
    queryFn: () =>
      getData<PageData<Cluster>>('/clusters/').then((data) => data.list),
  })
  const clusters = useMemo(() => clustersQuery.data ?? [], [clustersQuery.data])
  const clusterIds = useMemo(
    () => clusters.map((cluster) => cluster.id),
    [clusters]
  )
  const linesQuery = useQuery<Record<string, DnsLine[]>>({
    queryKey: [...queryKeys.dnsZones, 'lines-by-cluster', clusterIds],
    queryFn: async () => {
      const entries = await Promise.all(
        clusters.map(async (cluster) => {
          const zone = await getData<DnsZone>(
            `/dns-zones/${cluster.dns_zone_id}`
          )
          return [cluster.id, zone.runtime.lines] as const
        })
      )
      return Object.fromEntries(entries)
    },
    enabled: clusterIds.length > 0,
  })
  const query = useQuery({
    queryKey: [...queryKeys.nodes, page, pageSize, keyword, clusterId, status],
    queryFn: () =>
      getData<PageData<Node>>('/nodes/', {
        page,
        page_size: pageSize,
        keyword: keyword || undefined,
        cluster_id: clusterId === 'all' ? undefined : clusterId,
        status: status === 'all' ? undefined : status,
      }),
  })
  const remove = useMutation({
    mutationFn: (item: Node) =>
      sendData('delete', `/nodes/${item.id}`, undefined, item.revision),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({ queryKey: queryKeys.nodes })
    },
  })
  const { mutate: loadNodeCredentials } = useMutation({
    mutationFn: (item: Node) =>
      getData<NodeCredentials>(`/nodes/${item.id}/credentials`),
    onSuccess: setCredentials,
  })

  const columns = useMemo<ColumnDef<Node>[]>(
    () => [
      {
        accessorKey: 'name',
        size: 250,
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='节点 / 集群' />
        ),
        cell: ({ row }) => (
          <DataTableCellContent>
            <span className='font-medium' title={row.original.name}>
              {row.original.name}
            </span>
            <span className='text-xs text-muted-foreground'>
              {row.original.cluster_name} ·{' '}
              {row.original.runtime.agent_version || '未注册'}
            </span>
          </DataTableCellContent>
        ),
      },
      {
        id: 'connection',
        size: 275,
        header: '连接 / 心跳',
        cell: ({ row }) => (
          <DataTableCellContent>
            <StatusBadge status={row.original.runtime.connection_status} />
            <time
              className='text-xs text-muted-foreground tabular-nums'
              dateTime={row.original.runtime.last_heartbeat_at}
              title={row.original.runtime.last_heartbeat_at}
            >
              {formatDate(row.original.runtime.last_heartbeat_at)}
            </time>
          </DataTableCellContent>
        ),
      },
      {
        id: 'ip',
        size: 175,
        header: '节点 IP',
        cell: ({ row }) => {
          const endpoints = row.original.config.endpoints
          const addresses = endpoints
            .slice(0, 2)
            .map((endpoint) => endpoint.ip_address)
            .join(' · ')
          return (
            <DataTableCellContent>
              <code title={addresses}>{addresses}</code>
              {endpoints.length > 2 && (
                <span className='text-xs text-muted-foreground'>
                  +{endpoints.length - 2}
                </span>
              )}
            </DataTableCellContent>
          )
        },
      },
      {
        id: 'line',
        size: 130,
        header: 'DNS 线路',
        cell: ({ row }) => {
          const endpoints = row.original.config.endpoints
          const lines = endpoints
            .slice(0, 2)
            .map((endpoint) =>
              dnsLinePath(
                linesQuery.data?.[row.original.cluster_id] ?? [],
                endpoint.line_code
              )
            )
            .join(' · ')
          return (
            <DataTableCellContent>
              <span title={lines}>{lines}</span>
              {endpoints.length > 2 && (
                <span className='text-xs text-muted-foreground'>
                  +{endpoints.length - 2}
                </span>
              )}
            </DataTableCellContent>
          )
        },
      },
      {
        id: 'metrics',
        size: 300,
        header: '实时负载',
        cell: ({ row }) => (
          <div className='text-xs whitespace-nowrap text-muted-foreground'>
            CPU {row.original.runtime.cpu_usage?.toFixed(1) ?? '—'}% · 内存{' '}
            {row.original.runtime.memory_usage?.toFixed(1) ?? '—'}% ·{' '}
            {formatBytesPerSecond(row.original.runtime.traffic_out_bps)} ·{' '}
            {row.original.runtime.connection_count ?? '—'} 连接
          </div>
        ),
      },
      {
        accessorKey: 'status',
        size: 120,
        header: '启用状态',
        cell: ({ row }) => <StatusBadge status={row.original.status} />,
      },
      {
        id: 'actions',
        size: 52,
        header: () => <span className='sr-only'>操作</span>,
        cell: ({ row }) => (
          <div className='flex justify-end'>
            <RowActions>
              <DropdownMenuItem onSelect={() => setDialog(row.original)}>
                <Pencil /> 编辑
              </DropdownMenuItem>
              <DropdownMenuItem
                onSelect={() => loadNodeCredentials(row.original)}
              >
                <KeyRound /> 接入凭据
              </DropdownMenuItem>
              <DropdownMenuItem onSelect={() => setLogNode(row.original)}>
                <FileTerminal /> 实时日志
              </DropdownMenuItem>
              <DropdownMenuSeparator />
              <DropdownMenuItem
                variant='destructive'
                onSelect={() => setRemoveTarget(row.original)}
              >
                <Trash2 /> 删除
              </DropdownMenuItem>
            </RowActions>
          </div>
        ),
      },
    ],
    [linesQuery.data, loadNodeCredentials]
  )

  return (
    <>
      <ResourceToolbar
        value={filter.draft.keyword}
        onChange={(value) => filter.setField('keyword', value)}
        onSearch={() => {
          const changed = filter.apply()
          setPage(1)
          if (!changed && page === 1) void query.refetch()
        }}
        onReset={() => {
          const changed = filter.reset()
          setPage(1)
          if (!changed && page === 1) void query.refetch()
        }}
        refreshing={query.isFetching}
        placeholder='搜索节点名称…'
        filters={
          <>
            {showClusterFilter && (
              <Select
                value={filter.draft.clusterId}
                onValueChange={(value) => filter.setField('clusterId', value)}
              >
                <SelectTrigger className='w-44'>
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value='all'>全部集群</SelectItem>
                  {clustersQuery.data?.map((cluster) => (
                    <SelectItem key={cluster.id} value={cluster.id}>
                      {cluster.name}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            )}
            <Select
              value={filter.draft.status}
              onValueChange={(value) => filter.setField('status', value)}
            >
              <SelectTrigger className='w-36'>
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value='all'>全部状态</SelectItem>
                <SelectItem value='enabled'>已启用</SelectItem>
                <SelectItem value='disabled'>已停用</SelectItem>
              </SelectContent>
            </Select>
          </>
        }
      />
      <ResourceTable
        columns={columns}
        data={query.data?.list ?? []}
        fixedLayout
        loading={query.isLoading}
        error={query.isError}
        onRetry={() => void query.refetch()}
        page={page}
        pageSize={pageSize}
        totalPages={query.data?.total_pages ?? 1}
        onPaginationChange={(nextPage, nextSize) => {
          setPage(nextPage)
          setPageSize(nextSize)
        }}
        emptyTitle='暂无节点'
        emptyDescription='添加节点后会生成一次性接入凭据。'
      />
      {(dialog || createOpen) && (
        <NodeDialog
          key={!dialog || dialog === 'new' ? 'new' : dialog.id}
          node={!dialog || dialog === 'new' ? undefined : dialog}
          clusters={clustersQuery.data ?? []}
          initialClusterId={clusterId === 'all' ? undefined : clusterId}
          open
          onCredentials={setCredentials}
          onOpenChange={(open) => {
            if (!open) {
              setDialog(null)
              onCreateOpenChange?.(false)
            }
          }}
        />
      )}
      <CredentialsDialog
        credentials={credentials}
        onOpenChange={(open) => !open && setCredentials(null)}
      />
      {logNode && (
        <NodeLogSheet
          key={logNode.id}
          node={logNode}
          onOpenChange={(open) => !open && setLogNode(null)}
        />
      )}
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除节点'
        desc={`确定删除“${removeTarget?.name ?? ''}”吗？该节点的接入凭据将立即失效。`}
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </>
  )
}
