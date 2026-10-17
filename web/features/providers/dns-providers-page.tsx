import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button, Link, Tooltip } from '@heroui/react'
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
import { dnsProvidersQuery } from './data'
import { DnsProviderSheet } from './dns-provider-sheet'
import {
  providerLabel,
  providerStatuses,
  providerStatusReserve,
} from './provider-display'
import type { DnsProvider } from './types'

const route = getRouteApi('/_authenticated/providers/dns')

/** `/providers/dns`. Search, pagination and the add/edit sheets live in the
 * URL; the list is paged by the server. */
export function DnsProvidersPage() {
  const list = useListSearch(route)
  const { sheet } = list
  const query = useQuery({
    ...dnsProvidersQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
    }),
    placeholderData: keepPreviousData,
  })
  const editing = useListSheetTarget(sheet, query)
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
          aria-label={`编辑 DNS 账号 ${item.name}`}
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
      {sheet.isCreating && (
        <DnsProviderSheet onClose={() => void sheet.close()} />
      )}
      {editing && (
        <DnsProviderSheet
          key={editing.id}
          provider={editing}
          onClose={() => void sheet.close()}
        />
      )}
    </div>
  )
}
