import { useMemo, useState } from 'react'
import {
  keepPreviousData,
  useMutation,
  useQuery,
} from '@tanstack/react-query'
import type { ColumnDef } from '@tanstack/react-table'
import { CheckCircle2, Pencil, Plus, Trash2 } from 'lucide-react'
import { toast } from 'sonner'
import { formatDate } from '@/lib/format'
import { DEFAULT_PAGE_SIZE } from '@/lib/page-size'
import { useResourceFilters } from '@/hooks/use-resource-filters'
import { Button } from '@/components/ui/button'
import {
  DropdownMenuItem,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTableColumnHeader } from '@/components/data-table'
import { DataTableCellContent } from '@/components/data-table/cell-content'
import { FeatureShell } from '@/components/feature-shell'
import { ResourceTable } from '@/components/resource-table'
import { ResourceToolbar } from '@/components/resource-toolbar'
import { RowActions } from '@/components/row-actions'
import { StatusBadge } from '@/components/status-badge'
import type {
  CertificateProvider,
  DnsProvider,
} from '@/features/providers/types'
import { CertificateProviderDialog } from './certificate-provider-dialog'
import {
  verifyProvider as verifyProviderRequest,
  removeProvider,
  dnsProvidersQuery,
  certificateProvidersQuery,
} from './data'
import { DnsProviderDialog } from './dns-provider-dialog'
import { providerLabel } from './provider-display'

export function Providers() {
  const [tab, setTab] = useState('dns')
  const certificateFilter = useResourceFilters({ keyword: '' })
  const [certificatePage, setCertificatePage] = useState(1)
  const [certificatePageSize, setCertificatePageSize] =
    useState(DEFAULT_PAGE_SIZE)
  const [dnsPage, setDnsPage] = useState(1)
  const [dnsPageSize, setDnsPageSize] = useState(DEFAULT_PAGE_SIZE)
  const filter = useResourceFilters({ keyword: '' })
  const { keyword } = filter.filters
  const [dnsDialog, setDnsDialog] = useState<DnsProvider | 'new' | null>(null)
  const [certificateDialog, setCertificateDialog] = useState<
    CertificateProvider | 'new' | null
  >(null)
  const [removeTarget, setRemoveTarget] = useState<
    | { kind: 'dns'; item: DnsProvider }
    | { kind: 'certificate'; item: CertificateProvider }
    | null
  >(null)

  const dnsQuery = useQuery({
    ...dnsProvidersQuery({
      page: dnsPage,
      page_size: dnsPageSize,
      keyword: keyword || undefined,
    }),
    placeholderData: keepPreviousData,
  })
  const certificateQuery = useQuery(certificateProvidersQuery)
  // This endpoint returns the complete list; filter and paginate it locally.
  const certificates = (certificateQuery.data ?? []).filter((item) =>
    `${item.provider} ${item.account_email ?? ''}`
      .toLowerCase()
      .includes(certificateFilter.filters.keyword.toLowerCase())
  )

  const { mutate: verifyProvider } = useMutation({
    mutationFn: verifyProviderRequest,
    onSuccess: async (response) => {
      toast.success(response.message)
    },
  })

  const remove = useMutation({
    mutationFn: (target: NonNullable<typeof removeTarget>) =>
      removeProvider({
        kind: target.kind,
        id: target.item.id,
        revision: target.item.revision,
      }),
    onSuccess: async (response) => {
      toast.success(response.message)
      setRemoveTarget(null)
    },
  })

  const dnsColumns = useMemo<ColumnDef<DnsProvider>[]>(
    () => [
      {
        accessorKey: 'provider',
        header: '平台',
        cell: ({ row }) => providerLabel(row.original.provider),
      },
      {
        accessorKey: 'name',
        header: ({ column }) => (
          <DataTableColumnHeader column={column} title='账号名称' />
        ),
        cell: ({ row }) => (
          <DataTableCellContent>
            <div className='font-medium'>{row.original.name}</div>
          </DataTableCellContent>
        ),
      },
      {
        accessorKey: 'account_id',
        header: '账户标识',
        cell: ({ row }) => (
          <code className='text-xs'>{row.original.account_id}</code>
        ),
      },
      {
        accessorKey: 'zone_count',
        header: '托管域名',
        cell: ({ row }) => (
          <span className='tabular-nums'>{row.original.zone_count} 个</span>
        ),
      },
      {
        accessorKey: 'last_verified_at',
        header: '最近验证',
        cell: ({ row }) => (
          <span className='whitespace-nowrap text-muted-foreground'>
            {formatDate(row.original.last_verified_at)}
          </span>
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
            <DropdownMenuItem onSelect={() => setDnsDialog(row.original)}>
              <Pencil /> 编辑
            </DropdownMenuItem>
            <DropdownMenuItem
              onSelect={() =>
                verifyProvider({
                  kind: 'dns',
                  id: row.original.id,
                  revision: row.original.revision,
                })
              }
            >
              <CheckCircle2 /> 验证凭据
            </DropdownMenuItem>
            <DropdownMenuSeparator />
            <DropdownMenuItem
              variant='destructive'
              onSelect={() =>
                setRemoveTarget({ kind: 'dns', item: row.original })
              }
            >
              <Trash2 /> 删除
            </DropdownMenuItem>
          </RowActions>
        ),
      },
    ],
    [verifyProvider]
  )

  const certificateColumns = useMemo<ColumnDef<CertificateProvider>[]>(
    () => [
      {
        accessorKey: 'provider',
        header: '供应商',
        cell: ({ row }) => (
          <div className='font-medium'>
            {providerLabel(row.original.provider)}
          </div>
        ),
      },
      {
        accessorKey: 'credential_mode',
        header: '接入方式',
        cell: ({ row }) =>
          row.original.credential_mode === 'email' ? '邮箱' : 'Access Key',
      },
      {
        accessorKey: 'account_email',
        header: '账户',
        cell: ({ row }) =>
          row.original.account_email || row.original.access_key_hint || '—',
      },
      {
        accessorKey: 'last_verified_at',
        header: '最近验证',
        cell: ({ row }) => (
          <span className='whitespace-nowrap text-muted-foreground'>
            {formatDate(row.original.last_verified_at)}
          </span>
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
            <DropdownMenuItem
              onSelect={() => setCertificateDialog(row.original)}
            >
              <Pencil /> 编辑
            </DropdownMenuItem>
            <DropdownMenuItem
              onSelect={() =>
                verifyProvider({
                  kind: 'certificate',
                  id: row.original.id,
                  revision: row.original.revision,
                })
              }
            >
              <CheckCircle2 /> 验证凭据
            </DropdownMenuItem>
            <DropdownMenuSeparator />
            <DropdownMenuItem
              variant='destructive'
              onSelect={() =>
                setRemoveTarget({
                  kind: 'certificate',
                  item: row.original,
                })
              }
            >
              <Trash2 /> 删除
            </DropdownMenuItem>
          </RowActions>
        ),
      },
    ],
    [verifyProvider]
  )

  return (
    <FeatureShell title='服务商' description='管理 DNS 与证书供应商凭据。'>
      <Tabs value={tab} onValueChange={setTab} className='space-y-4'>
        <div className='flex flex-wrap items-center justify-between gap-2'>
          <TabsList>
            <TabsTrigger value='dns'>DNS 服务商</TabsTrigger>
            <TabsTrigger value='certificate'>证书供应商</TabsTrigger>
          </TabsList>
          {tab === 'dns' ? (
            <Button onClick={() => setDnsDialog('new')}>
              <Plus /> 添加 DNS 账号
            </Button>
          ) : (
            <Button onClick={() => setCertificateDialog('new')}>
              <Plus /> 添加证书供应商
            </Button>
          )}
        </div>
        <TabsContent value='dns' className='space-y-4'>
          <ResourceToolbar
            value={filter.draft.keyword}
            onChange={(value) => filter.setField('keyword', value)}
            onSearch={() => {
              const changed = filter.apply()
              setDnsPage(1)
              if (!changed && dnsPage === 1) void dnsQuery.refetch()
            }}
            onReset={() => {
              const changed = filter.reset()
              setDnsPage(1)
              if (!changed && dnsPage === 1) void dnsQuery.refetch()
            }}
            refreshing={dnsQuery.isFetching}
            placeholder='搜索账号名称…'
          />
          <ResourceTable
            columns={dnsColumns}
            data={dnsQuery.data?.list ?? []}
            loading={dnsQuery.isLoading}
            error={dnsQuery.isError}
            onRetry={() => void dnsQuery.refetch()}
            page={dnsPage}
            pageSize={dnsPageSize}
            totalPages={dnsQuery.data?.total_pages ?? 1}
            onPaginationChange={(nextPage, nextSize) => {
              setDnsPage(nextPage)
              setDnsPageSize(nextSize)
            }}
          />
        </TabsContent>
        <TabsContent value='certificate' className='space-y-4'>
          <ResourceToolbar
            value={certificateFilter.draft.keyword}
            onChange={(value) => certificateFilter.setField('keyword', value)}
            placeholder='搜索供应商或邮箱…'
            onSearch={() => {
              certificateFilter.apply()
              setCertificatePage(1)
              void certificateQuery.refetch()
            }}
            onReset={() => {
              const changed = certificateFilter.reset()
              setCertificatePage(1)
              if (!changed && certificatePage === 1) {
                void certificateQuery.refetch()
              }
            }}
            refreshing={certificateQuery.isFetching}
          />
          <ResourceTable
            columns={certificateColumns}
            data={certificates.slice(
              (certificatePage - 1) * certificatePageSize,
              certificatePage * certificatePageSize
            )}
            loading={certificateQuery.isLoading}
            error={certificateQuery.isError}
            onRetry={() => void certificateQuery.refetch()}
            page={certificatePage}
            pageSize={certificatePageSize}
            totalPages={Math.max(
              1,
              Math.ceil(certificates.length / certificatePageSize)
            )}
            onPaginationChange={(nextPage, nextSize) => {
              setCertificatePage(nextPage)
              setCertificatePageSize(nextSize)
            }}
          />
        </TabsContent>
      </Tabs>

      {dnsDialog && (
        <DnsProviderDialog
          key={dnsDialog === 'new' ? 'new' : dnsDialog.id}
          provider={dnsDialog === 'new' ? undefined : dnsDialog}
          open
          onOpenChange={(open) => !open && setDnsDialog(null)}
        />
      )}
      {certificateDialog && (
        <CertificateProviderDialog
          key={certificateDialog === 'new' ? 'new' : certificateDialog.id}
          provider={certificateDialog === 'new' ? undefined : certificateDialog}
          open
          onOpenChange={(open) => !open && setCertificateDialog(null)}
        />
      )}
      <ConfirmDialog
        open={!!removeTarget}
        onOpenChange={(open) => !open && setRemoveTarget(null)}
        title='删除服务商配置'
        desc={
          removeTarget?.kind === 'dns'
            ? `将删除“${removeTarget.item.name}”，已关联资源会阻止此操作。`
            : '将删除此证书供应商，已关联证书会阻止此操作。'
        }
        confirmText={remove.isPending ? '正在删除…' : '确认删除'}
        cancelBtnText='取消'
        destructive
        isLoading={remove.isPending}
        handleConfirm={() => removeTarget && remove.mutate(removeTarget)}
      />
    </FeatureShell>
  )
}
