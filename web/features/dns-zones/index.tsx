import { useMemo, useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { Eye, FilePenLine, Plus, RefreshCw, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { getData, sendData } from '@/lib/api'
import { formatDate } from '@/lib/format'
import { queryKeys } from '@/lib/query-keys'
import type { DnsProvider, DnsZone, PageData } from '@/lib/types'
import { useResourceFilters } from '@/hooks/use-resource-filters'
import { Button } from '@/components/ui/button'
import {
  DropdownMenuItem,
  DropdownMenuLabel,
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
import { CreateZoneDialog } from './create-zone-dialog'
import {
  displaySyncStatus,
  hasMeaningfulConflicts,
} from './dns-zone-display'
import { RecordsDialog } from './records-dialog'
import { ZoneDetailSheet } from './zone-detail-sheet'

export function DnsZones() {
  const queryClient = useQueryClient()
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(10)
  const filter = useResourceFilters({ keyword: '', providerId: 'all' })
  const { keyword, providerId } = filter.filters
  const [createOpen, setCreateOpen] = useState(false)
  const [editTarget, setEditTarget] = useState<DnsZone | null>(null)
  const [detailTarget, setDetailTarget] = useState<DnsZone | null>(null)
  const [removeTarget, setRemoveTarget] = useState<DnsZone | null>(null)

  const providersQuery = useQuery({
    queryKey: [...queryKeys.providers, 'dns', 'options'],
    queryFn: () =>
      getData<PageData<DnsProvider>>('/providers/dns').then((data) => data.list),
  })
  const query = useQuery({
    queryKey: [...queryKeys.dnsZones, page, pageSize, keyword, providerId],
    queryFn: () =>
      getData<PageData<DnsZone>>('/dns-zones/', {
        page,
        page_size: pageSize,
        keyword: keyword || undefined,
        dns_provider_id: providerId === 'all' ? undefined : providerId,
      }),
  })
  const { mutate: syncZone } = useMutation({
    mutationFn: ({
      item,
      policy,
    }: {
      item: DnsZone
      policy?: 'local' | 'remote'
    }) =>
      sendData(
        'post',
        `/dns-zones/${item.id}/sync`,
        policy ? { conflict_policy: policy } : undefined
      ),
    onSuccess: async (response) => {
      toast.success(response.message)
      await queryClient.invalidateQueries({ queryKey: queryKeys.dnsZones })
    },
  })
  const remove = useMutation({
    mutationFn: (item: DnsZone) =>
      sendData('delete', `/dns-zones/${item.id}`, undefined, item.revision),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
      await queryClient.invalidateQueries({ queryKey: queryKeys.dnsZones })
    },
  })

  const columns = useMemo<ColumnDef<DnsZone>[]>(
    () => [
      {
        accessorKey: 'domain',
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='托管域名' />
        ),
        cell: ({ row }) => (
          <DataTableCellContent>
            <div className='font-medium'>{row.original.domain}</div>
          </DataTableCellContent>
        ),
      },
      {
        accessorKey: 'dns_provider_name',
        header: 'DNS 服务商',
      },
      {
        id: 'records',
        header: '记录数',
        cell: ({ row }) => (
          <span className='tabular-nums'>
            {row.original.config.records.length} 条
          </span>
        ),
      },
      {
        accessorKey: 'website_count',
        header: '关联网站',
      },
      {
        accessorKey: 'sync_status',
        header: '同步状态',
        cell: ({ row }) => <StatusBadge status={displaySyncStatus(row.original)} />,
      },
      {
        accessorKey: 'last_synced_at',
        header: '最近同步',
        cell: ({ row }) => (
          <span className='whitespace-nowrap text-muted-foreground'>
            {formatDate(row.original.last_synced_at)}
          </span>
        ),
      },
      {
        id: 'actions',
        cell: ({ row }) => (
          <RowActions>
            <DropdownMenuItem onSelect={() => setDetailTarget(row.original)}>
              <Eye /> 查看详情
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setEditTarget(row.original)}>
              <FilePenLine /> 编辑记录
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => syncZone({ item: row.original })}>
              <RefreshCw /> 立即同步
            </DropdownMenuItem>
            {hasMeaningfulConflicts(row.original) && (
              <>
                <DropdownMenuLabel>解决冲突</DropdownMenuLabel>
                <DropdownMenuItem
                  onSelect={() =>
                    syncZone({ item: row.original, policy: 'local' })
                  }
                >
                  使用本地记录
                </DropdownMenuItem>
                <DropdownMenuItem
                  onSelect={() =>
                    syncZone({ item: row.original, policy: 'remote' })
                  }
                >
                  使用远端记录
                </DropdownMenuItem>
              </>
            )}
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
    [syncZone]
  )

  return (
    <FeatureShell
      title='DNS 托管'
      description='同步 DNS 服务商区域、记录与线路状态。'
      actions={
        <Button onClick={() => setCreateOpen(true)}>
          <Plus /> 添加域名
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
        placeholder='搜索域名…'
        filters={
          <Select
            value={filter.draft.providerId}
            onValueChange={(value) => filter.setField('providerId', value)}
          >
            <SelectTrigger className='w-44'>
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value='all'>全部服务商</SelectItem>
              {providersQuery.data?.map((provider) => (
                <SelectItem key={provider.id} value={provider.id}>
                  {provider.name}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
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
        emptyTitle='暂无托管域名'
        emptyDescription='先添加 DNS 服务商，再导入可用域名。'
      />
      <CreateZoneDialog
        open={createOpen}
        onOpenChange={setCreateOpen}
        providers={providersQuery.data ?? []}
      />
      {editTarget && (
        <RecordsDialog
          key={editTarget.id}
          zone={editTarget}
          open
          onOpenChange={(open) => !open && setEditTarget(null)}
        />
      )}
      <ZoneDetailSheet
        zone={detailTarget}
        onOpenChange={(open) => !open && setDetailTarget(null)}
      />
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='移除托管域名'
        desc={`确定移除“${removeTarget?.domain ?? ''}”吗？有关联网站时服务端会拒绝删除。`}
        confirmText={remove.isPending ? '正在删除…' : '确认移除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}
