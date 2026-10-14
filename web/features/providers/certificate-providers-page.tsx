import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Tooltip } from '@heroui/react'
import { hasListFilters, pageLocally, useListSearch } from '@/lib/list-search'
import { ActionMenu } from '@/components/action-menu'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTable } from '@/components/data-table'
import { ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { CertificateProviderSheet } from './certificate-provider-sheet'
import { certificateProvidersListQuery } from './data'
import { useProviderActions } from './provider-actions'
import { providerLabel, providerStatusLabels } from './provider-display'
import type { CertificateProvider } from './types'

const route = getRouteApi('/_authenticated/providers/')

/** The certificate provider stream returns the whole (small) collection, so
 * search and pagination run on the client while still living in the URL. */
export function CertificateProvidersPage() {
  const list = useListSearch(route)
  const navigate = route.useNavigate()
  const [editing, setEditing] = useState<CertificateProvider | null>(null)
  const [removing, setRemoving] = useState<CertificateProvider | null>(null)
  const actions = useProviderActions()
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
  const setCreating = (create: boolean) =>
    navigate({
      search: (previous) => ({ ...previous, create: create ? 1 : undefined }),
      replace: !create,
    })
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
      width: 56,
      align: 'end',
      cell: (item) => (
        <ActionMenu
          label={`证书供应商 ${providerLabel(item.provider)} 的操作`}
          items={[
            {
              id: 'verify',
              label: actions.isVerifying(item.id) ? '正在验证…' : '验证凭据',
              disabled: actions.isVerifying(item.id),
              onAction: () =>
                actions.verify({
                  kind: 'certificate',
                  id: item.id,
                  revision: item.revision,
                }),
            },
            { id: 'edit', label: '编辑', onAction: () => setEditing(item) },
            {
              id: 'delete',
              label: '删除供应商',
              danger: true,
              separated: true,
              onAction: () => setRemoving(item),
            },
          ]}
        />
      ),
    },
  ]
  const createButton = (
    <Button size='sm' onPress={() => void setCreating(true)}>
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
                action: createButton,
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
      {list.search.create === 1 && (
        <CertificateProviderSheet onClose={() => void setCreating(false)} />
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
      <ConfirmDialog
        isOpen={!!removing}
        onClose={() => setRemoving(null)}
        title={`删除证书供应商 ${removing ? providerLabel(removing.provider) : ''}？`}
        description='删除后无法再用该凭据签发或续期证书。仍有证书使用它时，删除会被拒绝。'
        confirmLabel='删除供应商'
        onConfirm={() =>
          removing &&
          actions.remove({
            kind: 'certificate',
            id: removing.id,
            revision: removing.revision,
          })
        }
      />
    </div>
  )
}
