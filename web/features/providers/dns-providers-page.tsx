import { useState } from 'react'
import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Link, Tooltip } from '@heroui/react'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import { ActionMenu } from '@/components/action-menu'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTable } from '@/components/data-table'
import { ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { dnsProvidersQuery } from './data'
import { DnsProviderSheet } from './dns-provider-sheet'
import { useProviderActions } from './provider-actions'
import { providerLabel, providerStatusLabels } from './provider-display'
import type { DnsProvider } from './types'

const route = getRouteApi('/_authenticated/providers/')

export function DnsProvidersPage() {
  const list = useListSearch(route)
  const navigate = route.useNavigate()
  const [editing, setEditing] = useState<DnsProvider | null>(null)
  const [removing, setRemoving] = useState<DnsProvider | null>(null)
  const actions = useProviderActions()
  const query = useQuery({
    ...dnsProvidersQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
    }),
    placeholderData: keepPreviousData,
  })
  const filtered = hasListFilters(list.search, [])
  const setCreating = (create: boolean) =>
    navigate({
      search: (previous) => ({ ...previous, create: create ? 1 : undefined }),
      replace: !create,
    })
  const columns: DataGridColumn<DnsProvider>[] = [
    {
      id: 'name',
      header: '账号',
      isRowHeader: true,
      minWidth: 220,
      cell: (item) => (
        <div className='flex min-w-0 flex-col'>
          <span className='truncate font-medium'>{item.name}</span>
          <span className='text-xs text-muted'>
            {providerLabel(item.provider)}
          </span>
        </div>
      ),
    },
    {
      id: 'account',
      header: '账户标识',
      minWidth: 180,
      cell: (item) => <Mono truncate>{item.account_id}</Mono>,
    },
    {
      id: 'token',
      header: '凭据',
      minWidth: 110,
      cell: (item) => (
        <Mono className='text-muted'>{item.token_hint || '—'}</Mono>
      ),
    },
    {
      id: 'zones',
      header: '托管域名',
      minWidth: 100,
      align: 'end',
      cell: (item) =>
        item.zone_count ? (
          <Link
            href={`/dns-zones?${new URLSearchParams({ dns_provider_id: item.id })}`}
            className='tabular-nums'
          >
            {item.zone_count} 个
          </Link>
        ) : (
          <span className='text-muted tabular-nums'>0 个</span>
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
          label={`DNS 账号 ${item.name} 的操作`}
          items={[
            {
              id: 'verify',
              label: actions.isVerifying(item.id) ? '正在验证…' : '验证凭据',
              disabled: actions.isVerifying(item.id),
              onAction: () =>
                actions.verify({
                  kind: 'dns',
                  id: item.id,
                  revision: item.revision,
                }),
            },
            { id: 'edit', label: '编辑', onAction: () => setEditing(item) },
            {
              id: 'delete',
              label: '删除账号',
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
      添加账号
    </Button>
  )
  return (
    <div className='flex flex-col gap-3'>
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索账号名称或账户标识',
        }}
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters([])}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isFetching && !query.isPending}
        actions={createButton}
      />
      <DataTable
        label='DNS 账号'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => query.refetch()}
        minWidth={900}
        empty={
          filtered
            ? {
                title: '没有匹配的 DNS 账号',
                description: '调整搜索关键字后重试。',
              }
            : {
                title: '还没有 DNS 账号',
                description:
                  '添加 Cloudflare 或阿里云账号后，即可托管 DNS 区域并自动同步解析记录。',
                action: createButton,
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
      {list.search.create === 1 && (
        <DnsProviderSheet onClose={() => void setCreating(false)} />
      )}
      {editing && (
        <DnsProviderSheet
          key={editing.id}
          provider={
            query.data?.list.find((item) => item.id === editing.id) ?? editing
          }
          onClose={() => setEditing(null)}
        />
      )}
      <ConfirmDialog
        isOpen={!!removing}
        onClose={() => setRemoving(null)}
        title={`删除 DNS 账号 ${removing?.name ?? ''}？`}
        description='删除后将无法再用该账号同步解析记录。仍有托管 DNS 区域时，删除会被拒绝。'
        confirmLabel='删除账号'
        onConfirm={() =>
          removing &&
          actions.remove({
            kind: 'dns',
            id: removing.id,
            revision: removing.revision,
          })
        }
      />
    </div>
  )
}
