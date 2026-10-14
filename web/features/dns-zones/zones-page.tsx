import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { Link, getRouteApi } from '@tanstack/react-router'
import {
  ArrowsRotateRight,
  Eye,
  PencilToSquare,
  Plus,
  TrashBin,
} from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import { ActionMenu, type ActionMenuItem } from '@/components/action-menu'
import { DataTable } from '@/components/data-table'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { Page } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { dnsProviderOptionsQuery } from '@/features/providers/data'
import { providerLabel } from '@/features/providers/provider-display'
import { CreateZoneSheet } from './create-zone-sheet'
import { dnsZonesQuery } from './data'
import {
  displaySyncStatus,
  hasMeaningfulConflicts,
  zoneSyncFilters,
  zoneSyncStatusLabels,
} from './dns-zone-display'
import type { DnsZone } from './types'
import { useZoneCommands } from './zone-commands'

const route = getRouteApi('/_authenticated/dns-zones/')
const filterKeys = ['dns_provider_id', 'sync_status'] as const

export function DnsZonesPage() {
  const list = useListSearch(route)
  const { sync_status: syncFilter, dns_provider_id: providerId } = list.search
  const commands = useZoneCommands()
  // The provider filter only needs the account names once.
  const providers = useQuery({ ...dnsProviderOptionsQuery, gcTime: 0 })
  const query = useQuery({
    ...dnsZonesQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
      dns_provider_id: providerId,
      sync_status: syncFilter,
    }),
    placeholderData: keepPreviousData,
    meta: { inlineError: true },
  })
  const filtered = hasListFilters(list.search, filterKeys)
  const navigate = route.useNavigate()
  // The sheet flag is not a filter: toggling it keeps the current page.
  const setCreateOpen = (open: boolean) =>
    void navigate({
      search: (previous) => ({ ...previous, create: open ? 1 : undefined }),
      replace: true,
    })
  const createAction = (
    <Button size='sm' onPress={() => setCreateOpen(true)}>
      <Plus className='size-4' />
      添加 DNS 区域
    </Button>
  )
  const rowActions = (zone: DnsZone): ActionMenuItem[] => [
    {
      id: 'detail',
      label: '查看详情',
      icon: <Eye />,
      href: `/dns-zones/${zone.id}`,
    },
    {
      id: 'records',
      label: '编辑记录',
      icon: <PencilToSquare />,
      href: `/dns-zones/${zone.id}`,
      state: { dnsZone: { tab: 'records', editRecords: true } },
    },
    {
      id: 'sync',
      label: '立即同步',
      icon: <ArrowsRotateRight />,
      disabled: commands.syncingId === zone.id,
      onAction: () => commands.sync(zone),
    },
    ...(hasMeaningfulConflicts(zone)
      ? [
          {
            id: 'local',
            label: '冲突处理：使用本地记录',
            separated: true,
            onAction: () => commands.resolve(zone, 'local'),
          },
          {
            id: 'remote',
            label: '冲突处理：使用远端记录',
            onAction: () => commands.resolve(zone, 'remote'),
          },
        ]
      : []),
    {
      id: 'remove',
      label: '移除区域',
      icon: <TrashBin />,
      danger: true,
      separated: true,
      onAction: () => commands.remove(zone),
    },
  ]
  const columns: DataGridColumn<DnsZone>[] = [
    {
      id: 'domain',
      header: '域名',
      isRowHeader: true,
      minWidth: 220,
      cell: (zone) => (
        <Link
          to='/dns-zones/$zoneId'
          params={{ zoneId: zone.id }}
          className='link font-medium'
        >
          <Mono>{zone.domain}</Mono>
        </Link>
      ),
    },
    {
      id: 'provider',
      header: 'DNS 账号',
      minWidth: 180,
      cell: (zone) => (
        <div className='flex min-w-0 flex-col'>
          <span className='truncate'>{zone.dns_provider_name}</span>
          <span className='text-xs text-muted'>
            {providerLabel(zone.dns_provider)}
          </span>
        </div>
      ),
    },
    {
      id: 'records',
      header: '记录',
      align: 'end',
      minWidth: 80,
      cell: (zone) => (
        <span className='tabular-nums'>
          {zone.config.records.length + zone.runtime.projected_records.length}
        </span>
      ),
    },
    {
      id: 'websites',
      header: '关联网站',
      align: 'end',
      minWidth: 90,
      cell: (zone) => (
        <span className='tabular-nums'>{zone.website_count}</span>
      ),
    },
    {
      id: 'status',
      header: '同步状态',
      minWidth: 130,
      cell: (zone) => (
        <StatusChip
          status={displaySyncStatus(zone)}
          reserve={zoneSyncStatusLabels}
        />
      ),
    },
    {
      id: 'synced',
      header: '最近同步',
      minWidth: 120,
      cell: (zone) => (
        <RelativeTime
          value={zone.last_synced_at}
          className='text-[13px] text-muted'
        />
      ),
    },
    {
      id: 'actions',
      header: '',
      align: 'end',
      width: 56,
      cell: (zone) => (
        <ActionMenu
          label={`DNS 区域 ${zone.domain} 的操作`}
          items={rowActions(zone)}
        />
      ),
    },
  ]
  return (
    <Page
      title='DNS 区域'
      description='托管在 DNS 账号中的域名：自定义记录、系统自动记录与解析线路的同步状态。'
      actions={createAction}
    >
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索域名',
        }}
        filters={
          <>
            <FilterSelect
              label='DNS 账号'
              value={providerId ?? ''}
              onChange={(value) =>
                void list.setSearch({ dns_provider_id: value || undefined })
              }
              options={(providers.data ?? []).map((item) => ({
                id: item.id,
                label: item.name,
              }))}
            />
            <FilterSelect
              label='同步状态'
              value={syncFilter ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  sync_status: (value || undefined) as typeof syncFilter,
                })
              }
              options={[...zoneSyncFilters]}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters(filterKeys)}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isRefetching}
      />
      <DataTable
        label='DNS 区域'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => void query.refetch()}
        minWidth={900}
        empty={
          filtered
            ? {
                title: '没有匹配的 DNS 区域',
                description: '调整搜索词或筛选条件后重试。',
              }
            : {
                title: '还没有 DNS 区域',
                description:
                  '从 DNS 账号导入域名后，即可为集群、证书和网站自动维护解析记录。',
                action: createAction,
              }
        }
        footer={
          <TablePagination
            page={list.page}
            pageSize={list.pageSize}
            total={query.data?.total ?? 0}
            onPageChange={list.setPage}
            onPageSizeChange={list.setPageSize}
          />
        }
      />
      <CreateZoneSheet
        isOpen={list.search.create === 1}
        onClose={() => setCreateOpen(false)}
      />
      {commands.dialogs}
    </Page>
  )
}
