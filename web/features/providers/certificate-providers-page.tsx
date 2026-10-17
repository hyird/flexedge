import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Tooltip } from '@heroui/react'
import {
  hasListFilters,
  useListSearch,
  useListSheetTarget,
} from '@/lib/list-search'
import { describeStatus } from '@/lib/status'
import { DataTable } from '@/components/data-table'
import { ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { CertificateProviderSheet } from './certificate-provider-sheet'
import { certificateProvidersQuery } from './data'
import {
  providerLabel,
  providerStatuses,
  providerStatusReserve,
} from './provider-display'
import type { CertificateProvider } from './types'

const route = getRouteApi('/_authenticated/providers/certificate')

/** `/providers/certificate`. Search, pagination and add/edit sheets live in
 * the URL; GET and live snapshots use the same server-side page. */
export function CertificateProvidersPage() {
  const list = useListSearch(route)
  const { sheet } = list
  const query = useQuery({
    ...certificateProvidersQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
    }),
    placeholderData: keepPreviousData,
  })
  const editing = useListSheetTarget(sheet, query)
  const filtered = hasListFilters(list.search, [])
  const columns: DataGridColumn<CertificateProvider>[] = [
    {
      id: 'provider',
      header: '服务商',
      isRowHeader: true,
      minWidth: 170,
      cell: (item) => (
        <span className='font-medium'>{providerLabel(item.provider)}</span>
      ),
    },
    {
      id: 'mode',
      header: '接入方式',
      minWidth: 120,
      cell: (item) =>
        item.credential_mode === 'email' ? '邮箱' : 'API Access Key',
    },
    {
      id: 'account',
      header: '账户',
      minWidth: 220,
      cell: (item) => (
        <Mono truncate>
          {item.account_email || item.access_key_hint || '—'}
        </Mono>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 120,
      cell: (item) => {
        const chip = (
          <StatusChip
            {...describeStatus(providerStatuses, item.status)}
            reserve={providerStatusReserve}
          />
        )
        return item.last_error ? (
          <Tooltip delay={300}>
            <Tooltip.Trigger aria-label={item.last_error}>
              {chip}
            </Tooltip.Trigger>
            <Tooltip.Content className='max-w-80'>
              {item.last_error}
            </Tooltip.Content>
          </Tooltip>
        ) : (
          chip
        )
      },
    },
    {
      id: 'verified',
      header: '最近验证',
      minWidth: 120,
      cell: (item) => (
        <RelativeTime
          value={item.last_verified_at}
          className='text-sm text-muted'
        />
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      width: 72,
      cell: (item) => (
        <Button
          size='sm'
          variant='tertiary'
          className='px-2'
          aria-label={`编辑证书服务商 ${providerLabel(item.provider)}`}
          onPress={() => void sheet.openEdit(item.id)}
        >
          编辑
        </Button>
      ),
    },
  ]
  const createButton = (
    <Button size='sm' onPress={() => void sheet.openCreate()}>
      <Plus className='size-4' />
      添加服务商
    </Button>
  )
  return (
    <div className='flex flex-col gap-3'>
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索服务商或账户',
        }}
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters([])}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isFetching && !query.isPending}
        actions={createButton}
      />
      <DataTable
        label='证书服务商'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => query.refetch()}
        minWidth={820}
        empty={
          filtered
            ? {
                title: '没有匹配的证书服务商',
                description: '调整搜索关键字后重试。',
              }
            : {
                title: '还没有证书服务商',
                description:
                  "添加 Let's Encrypt 或 ZeroSSL 凭据后，即可为网站申请受信任证书。",
              }
        }
        footer={
          <TablePagination
            page={list.page}
            pageSize={list.pageSize}
            total={query.data?.total ?? 0}
            onPageChange={(page) => void list.setPage(page)}
            onPageSizeChange={(size) => void list.setPageSize(size)}
          />
        }
      />
      {sheet.isCreating && (
        <CertificateProviderSheet onClose={() => void sheet.close()} />
      )}
      {editing && (
        <CertificateProviderSheet
          key={editing.id}
          provider={editing}
          onClose={() => void sheet.close()}
        />
      )}
    </div>
  )
}
