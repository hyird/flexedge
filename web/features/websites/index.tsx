import { useMemo, useState } from 'react'
import {
  keepPreviousData,
  useMutation,
  useQuery,
  useQueryClient,
} from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { FilePenLine, FileText, Plus, Trash2, Eye } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { queryKeys } from '@/lib/query-keys'
import type { Cluster, PageData } from '@/lib/types'
import { useResourceFilters } from '@/hooks/use-resource-filters'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
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
import { FeatureShell } from '@/components/feature-shell'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'
import { AccessLogSheet } from './access-log-sheet'
import type { Website } from './types'
import { WebsiteDetailSheet } from './website-detail-sheet'
import { WebsiteDialog } from './website-dialog'
import { originGroupLabel } from './website-display'

export function Websites() {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const filter = useResourceFilters({
    keyword: '',
    clusterId: 'all',
    status: 'all',
  })
  const { keyword, clusterId, status } = filter.filters
  const [dialog, setDialog] = useState<Website | 'new' | null>(null)
  const [detailTarget, setDetailTarget] = useState<Website | null>(null)
  const [logTarget, setLogTarget] = useState<Website | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Website | null>(null)

  const clustersQuery = useQuery({
    queryKey: [...queryKeys.clusters, 'options'],
    queryFn: () =>
      getData<PageData<Cluster>>('/clusters/').then((data) => data.list),
  })
  const query = useQuery({
    queryKey: [
      ...queryKeys.websites,
      page,
      pageSize,
      keyword,
      clusterId,
      status,
    ],
    placeholderData: keepPreviousData,
    queryFn: () =>
      getData<PageData<Website>>('/websites/', {
        page,
        page_size: pageSize,
        keyword: keyword || undefined,
        cluster_id: clusterId === 'all' ? undefined : clusterId,
        status: status === 'all' ? undefined : status,
      }),
  })
  const remove = useMutation({
    mutationFn: (item: Website) =>
      sendData('delete', `/websites/${item.id}`, undefined, item.revision),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({ queryKey: queryKeys.websites })
    },
  })

  const columns = useMemo<ColumnDef<Website>[]>(
    () => [
      {
        id: 'name',
        accessorFn: (item) => item.config.name,
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='网站' />
        ),
        cell: ({ row }) => (
          <DataTableCellContent>
            <div className='font-medium'>
              {row.original.config.name ||
                row.original.config.domains[0]?.hostname}
            </div>
            <div className='max-w-64 truncate text-xs text-muted-foreground'>
              {row.original.config.domains
                .map((item) => item.hostname)
                .join('、')}
            </div>
          </DataTableCellContent>
        ),
      },
      {
        accessorKey: 'cluster_name',
        header: '所属集群',
        cell: ({ row }) => (
          <DataTableCellContent>
            <div>{row.original.cluster_name}</div>
            <code className='text-xs text-muted-foreground'>
              {row.original.access_domain}
            </code>
          </DataTableCellContent>
        ),
      },
      {
        id: 'origin',
        header: '回源',
        cell: ({ row }) => {
          const origins = row.original.config.origins
          const primary =
            origins.find(
              (origin) =>
                origin.role === 'primary' && origin.status === 'enabled'
            ) ?? origins[0]
          if (!primary) return '未配置'
          return (
            <DataTableCellContent>
              <div className='font-medium'>
                {originGroupLabel(primary.group)}
              </div>
              <code
                className='max-w-56 truncate text-xs text-muted-foreground'
                title={`${primary.protocol}://${primary.host}:${primary.port}`}
              >
                {primary.protocol}://{primary.host}:{primary.port}
              </code>
              {origins.length > 1 && (
                <span className='text-xs text-muted-foreground'>
                  共 {origins.length} 个源站
                </span>
              )}
            </DataTableCellContent>
          )
        },
      },
      {
        id: 'deploy',
        header: '节点发布',
        cell: ({ row }) => (
          <DataTableCellContent>
            <StatusBadge status={row.original.runtime.deploy_status} />
            <div className='text-xs text-muted-foreground'>
              {row.original.runtime.synced_node_count}/
              {row.original.runtime.target_node_count} 节点
            </div>
          </DataTableCellContent>
        ),
      },
      {
        id: 'https',
        header: 'HTTPS',
        cell: ({ row }) => (
          <Badge variant='outline'>
            {row.original.config.https_enabled
              ? `${row.original.certificates.length} 张证书`
              : '未启用'}
          </Badge>
        ),
      },
      {
        accessorKey: 'status',
        header: '状态',
        cell: ({ row }) => <StatusBadge status={row.original.status} />,
      },
      {
        id: 'actions',
        cell: ({ row }) => (
          <RowActions>
            <DropdownMenuItem onSelect={() => setDetailTarget(row.original)}>
              <Eye /> 查看详情
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setDialog(row.original)}>
              <FilePenLine /> 编辑配置
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setLogTarget(row.original)}>
              <FileText /> 访问日志
            </DropdownMenuItem>
            <DropdownMenuSeparator />
            <DropdownMenuItem
              variant='destructive'
              onSelect={() => setRemoveTarget(row.original)}
            >
              <Trash2 /> 删除
            </DropdownMenuItem>
          </RowActions>
        ),
      },
    ],
    [setDialog]
  )

  return (
    <FeatureShell
      title='网站'
      description='配置域名、源站、TLS 与边缘分发策略。'
      actions={
        <Button onClick={() => setDialog('new')}>
          <Plus /> 创建网站
        </Button>
      }
    >
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
        placeholder='搜索网站或域名…'
        filters={
          <>
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
        emptyTitle='暂无网站'
        emptyDescription='创建网站并将流量分发到边缘集群。'
      />

      {dialog && (
        <WebsiteDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          website={dialog === 'new' ? undefined : dialog}
          clusters={clustersQuery.data ?? []}
          open
          onOpenChange={(open) => {
            if (!open) setDialog(null)
          }}
        />
      )}
      <WebsiteDetailSheet
        website={detailTarget}
        onOpenChange={(open) => !open && setDetailTarget(null)}
      />
      {logTarget && (
        <AccessLogSheet
          key={logTarget.id}
          website={logTarget}
          onOpenChange={(open) => !open && setLogTarget(null)}
        />
      )}
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除网站'
        desc={`确定删除“${removeTarget?.config.name ?? ''}”吗？边缘节点将收到移除配置任务。`}
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}
