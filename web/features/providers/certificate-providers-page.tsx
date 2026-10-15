import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Tooltip } from '@heroui/react'
import { hasListFilters, pageLocally, useListState } from '@/lib/list-search'
import { DataTable } from '@/components/data-table'
import { ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { CertificateProviderSheet } from './certificate-provider-sheet'
import { certificateProvidersListQuery } from './data'
import { providerLabel, providerStatusLabels } from './provider-display'
import type { CertificateProvider } from './types'

/** `/providers` 证书供应商 tab. The certificate provider stream returns the
 * whole (small) collection, so search and pagination run on the client; they
 * and the add/edit sheets are component state, and `initialQuery` is the
 * arrival keyword. */
export function CertificateProvidersPage({
  initialQuery,
}: {
  initialQuery?: string
}) {
  const list = useListState<{ q?: string }>({ q: initialQuery })
  const [creating, setCreating] = useState(false)
  const [editing, setEditing] = useState<CertificateProvider | null>(null)
  const query = useQuery(certificateProvidersListQuery)
  const keyword = list.keyword.toLowerCase()
  const matches = query.data?.filter((item) =>
    [
      item.provider,
      providerLabel(item.provider),
      item.account_email ?? '',
      item.access_key_hint ?? '',
    ].some((value) => value.toLowerCase().includes(keyword))
  )
  const pageData = matches && pageLocally(matches, list.page, list.pageSize)
  const filtered = hasListFilters(list.search, [])
  const columns: DataGridColumn<CertificateProvider>[] = [
    {
      id: 'provider',
      header: '供应商',
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
      cell: (item) =>
        item.last_error ? (
          <Tooltip delay={300}>
            <Tooltip.Trigger aria-label={item.last_error}>
              <StatusChip status={item.status} reserve={providerStatusLabels} />
            </Tooltip.Trigger>
            <Tooltip.Content className='max-w-80'>
              {item.last_error}
            </Tooltip.Content>
          </Tooltip>
        ) : (
          <StatusChip status={item.status} reserve={providerStatusLabels} />
        ),
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
          aria-label={`编辑证书供应商 ${providerLabel(item.provider)}`}
          onPress={() => setEditing(item)}
        >
          编辑
        </Button>
      ),
    },
  ]
  const createButton = (
    <Button size='sm' onPress={() => setCreating(true)}>
      <Plus className='size-4' />
      添加供应商
    </Button>
  )
  return (
    <div className='flex flex-col gap-3'>
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索供应商或账户',
        }}
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters([])}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isFetching && !query.isPending}
        actions={createButton}
      />
      <DataTable
        label='证书供应商'
        columns={columns}
        data={pageData?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => query.refetch()}
        minWidth={820}
        empty={
          filtered
            ? {
                title: '没有匹配的证书供应商',
                description: '调整搜索关键字后重试。',
              }
            : {
                title: '还没有证书供应商',
                description:
                  "添加 Let's Encrypt 或 ZeroSSL 凭据后，即可为网站申请受信任证书。",
              }
        }
        footer={
          <TablePagination
            page={list.page}
            pageSize={list.pageSize}
            total={pageData?.total ?? 0}
            onPageChange={(page) => void list.setPage(page)}
            onPageSizeChange={(size) => void list.setPageSize(size)}
          />
        }
      />
      {creating && (
        <CertificateProviderSheet onClose={() => setCreating(false)} />
      )}
      {editing && (
        <CertificateProviderSheet
          key={editing.id}
          provider={
            query.data?.find((item) => item.id === editing.id) ?? editing
          }
          onClose={() => setEditing(null)}
        />
      )}
    </div>
  )
}
