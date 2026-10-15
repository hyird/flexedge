import { useState } from 'react'
import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Link, Tooltip } from '@heroui/react'
import { hasListFilters, useListState } from '@/lib/list-search'
import { DataTable } from '@/components/data-table'
import { ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { dnsProvidersQuery } from './data'
import { DnsProviderSheet } from './dns-provider-sheet'
import { providerLabel, providerStatusLabels } from './provider-display'
import type { DnsProvider } from './types'

/** `/providers` DNS 账号 tab. Search, pagination and the add/edit sheets are
 * component state; `initialQuery` is the arrival keyword. */
export function DnsProvidersPage({ initialQuery }: { initialQuery?: string }) {
  const list = useListState<{ q?: string }>({ q: initialQuery })
  const [creating, setCreating] = useState(false)
  const [editing, setEditing] = useState<DnsProvider | null>(null)
  const query = useQuery({
    ...dnsProvidersQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
    }),
    placeholderData: keepPreviousData,
  })
  const filtered = hasListFilters(list.search, [])
  const columns: DataGridColumn<DnsProvider>[] = [
    {
      id: 'name',
      header: '账号',
      isRowHeader: true,
      minWidth: 180,
      cell: (item) => (
        <span className='block truncate font-medium'>{item.name}</span>
      ),
    },
    {
      id: 'provider',
      header: '服务商',
      minWidth: 110,
      cell: (item) => providerLabel(item.provider),
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
      width: 72,
      cell: (item) => (
        <Button
          size='sm'
          variant='tertiary'
          className='px-2'
          aria-label={`编辑 DNS 账号 ${item.name}`}
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
                  '添加 Cloudflare 或阿里云账号后，即可托管域名并自动同步解析记录。',
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
      {creating && <DnsProviderSheet onClose={() => setCreating(false)} />}
      {editing && (
        <DnsProviderSheet
          key={editing.id}
          provider={
            query.data?.list.find((item) => item.id === editing.id) ?? editing
          }
          onClose={() => setEditing(null)}
        />
      )}
    </div>
  )
}
