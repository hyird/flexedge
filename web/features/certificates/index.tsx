import { useMemo, useState } from 'react'
import {
  keepPreviousData,
  useMutation,
  useQuery,
} from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { Download, Eye, Pencil, Plus, RefreshCw, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { formatDate } from '@/lib/format'
import { DEFAULT_PAGE_SIZE } from '@/lib/page-size'
import { useResourceFilters } from '@/hooks/use-resource-filters'
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
import type { Certificate } from '@/features/certificates/types'
import { CertificateDetailSheet } from './certificate-detail-sheet'
import { CertificateDialog } from './certificate-dialog'
import {
  certificatesQuery,
  renewCertificate as requestRenewal,
  removeCertificate,
  downloadCertificate,
} from './data'

export function Certificates() {
  const [page, setPage] = useState(1)
  const [pageSize, setPageSize] = useState(DEFAULT_PAGE_SIZE)
  const filter = useResourceFilters({ keyword: '', status: 'all' })
  const { keyword, status } = filter.filters
  const [dialog, setDialog] = useState<Certificate | 'new' | null>(null)
  const [detailTarget, setDetailTarget] = useState<Certificate | null>(null)
  const [removeTarget, setRemoveTarget] = useState<Certificate | null>(null)

  const query = useQuery({
    ...certificatesQuery({
      page,
      page_size: pageSize,
      keyword: keyword || undefined,
      status: status === 'all' ? undefined : status,
    }),
    placeholderData: keepPreviousData,
  })
  const { mutate: renewCertificate } = useMutation({
    mutationFn: requestRenewal,
    onSuccess: async (response) => {
      toast.success(response.message)
    },
  })
  const remove = useMutation({
    mutationFn: removeCertificate,
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
    },
  })

  const download = async (item: Certificate) => {
    const { blob, filename } = await downloadCertificate(item)
    const url = URL.createObjectURL(blob)
    const link = document.createElement('a')
    link.href = url
    link.download = filename
    link.click()
    URL.revokeObjectURL(url)
  }

  const columns = useMemo<ColumnDef<Certificate>[]>(
    () => [
      {
        id: 'domain',
        accessorFn: (item) => item.domains[0],
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='证书域名' />
        ),
        cell: ({ row }) => (
          <DataTableCellContent>
            <div className='font-medium'>{row.original.domains[0]}</div>
            {row.original.domains.length > 1 && (
              <div className='text-xs text-muted-foreground'>
                另有 {row.original.domains.length - 1} 个域名
              </div>
            )}
          </DataTableCellContent>
        ),
      },
      {
        accessorKey: 'issuer',
        header: '签发机构',
        cell: ({ row }) => (
          <DataTableCellContent>
            <span>{row.original.issuer || '—'}</span>
            <span className='text-xs text-muted-foreground'>
              {row.original.certificate_provider}
            </span>
          </DataTableCellContent>
        ),
      },
      {
        accessorKey: 'dns_zone_domain',
        header: 'DNS 验证域名',
      },
      {
        accessorKey: 'website_count',
        header: '使用网站',
        cell: ({ row }) => (
          <span className='tabular-nums'>{row.original.website_count} 个</span>
        ),
      },
      {
        accessorKey: 'expires_at',
        header: '到期时间',
        cell: ({ row }) => (
          <DataTableCellContent>
            <div className='whitespace-nowrap'>
              {formatDate(row.original.expires_at)}
            </div>
            <div className='text-xs text-muted-foreground'>
              {row.original.remaining_days === undefined
                ? '—'
                : `剩余 ${row.original.remaining_days} 天`}
            </div>
          </DataTableCellContent>
        ),
      },
      {
        accessorKey: 'status',
        header: '状态',
        cell: ({ row }) => (
          <DataTableCellContent>
            <StatusBadge status={row.original.status} />
            <span className='text-xs text-muted-foreground'>
              {row.original.usable ? '可用于网站' : '暂不可用'}
            </span>
          </DataTableCellContent>
        ),
      },
      {
        id: 'actions',
        cell: ({ row }) => (
          <RowActions>
            <DropdownMenuItem onSelect={() => setDetailTarget(row.original)}>
              <Eye /> 查看详情
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => setDialog(row.original)}>
              <Pencil /> 续期设置
            </DropdownMenuItem>
            <DropdownMenuItem onSelect={() => renewCertificate(row.original)}>
              <RefreshCw /> 重新签发
            </DropdownMenuItem>
            <DropdownMenuItem
              disabled={!row.original.usable}
              onSelect={() => download(row.original)}
            >
              <Download /> 下载证书
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
    [renewCertificate]
  )

  return (
    <FeatureShell
      title='证书'
      description='申请、自动续期并下载边缘网站证书。'
      actions={
        <Button onClick={() => setDialog('new')}>
          <Plus /> 申请证书
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
        placeholder='搜索证书域名…'
        filters={
          <Select
            value={filter.draft.status}
            onValueChange={(value) => filter.setField('status', value)}
          >
            <SelectTrigger className='w-36'>
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value='all'>全部状态</SelectItem>
              <SelectItem value='pending'>等待中</SelectItem>
              <SelectItem value='issuing'>签发中</SelectItem>
              <SelectItem value='valid'>有效</SelectItem>
              <SelectItem value='renewing'>续签中</SelectItem>
              <SelectItem value='failed'>失败</SelectItem>
              <SelectItem value='expired'>已过期</SelectItem>
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
        emptyTitle='暂无证书'
        emptyDescription='申请证书后，签发进度会显示在这里。'
      />
      {dialog && (
        <CertificateDialog
          key={dialog === 'new' ? 'new' : dialog.id}
          certificate={dialog === 'new' ? undefined : dialog}
          open
          onOpenChange={(open) => !open && setDialog(null)}
        />
      )}
      <CertificateDetailSheet
        certificate={detailTarget}
        onOpenChange={(open) => !open && setDetailTarget(null)}
      />
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除证书'
        desc={`确定删除“${removeTarget?.domains[0] ?? ''}”证书吗？有关联网站时服务端会拒绝删除。`}
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}
