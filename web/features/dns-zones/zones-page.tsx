import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import { describeStatus } from '@/lib/status'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
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
  zoneSyncFilters,
  zoneSyncStatuses,
  zoneSyncStatusReserve,
} from './dns-zone-display'
import type { DnsZone } from './types'

const route = getRouteApi('/_authenticated/dns-zones/')
const filterKeys = ['dns_provider_id', 'sync_status'] as const

export function DnsZonesPage() {
  const list = useListSearch(route)
  const { sync_status: syncFilter, dns_provider_id: providerId } = list.search
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
  })
  const filtered = hasListFilters(list.search, filterKeys)
  const createAction = (
    <Button size='sm' onPress={() => void list.sheet.openCreate()}>
      <Plus className='size-4' />
      添加域名
    </Button>
  )
  const columns: DataGridColumn<DnsZone>[] = [
    {
      id: 'domain',
      header: '域名',
      isRowHeader: true,
      cell: (zone) => (
        <Mono truncate className='font-medium'>
          {zone.domain}
        </Mono>
      ),
    },
    {
      id: 'provider',
      header: 'DNS账号',
      cell: (zone) => (
        <span className='block truncate'>{zone.dns_provider_name}</span>
      ),
    },
    {
      id: 'provider-type',
      header: '服务商',
      cell: (zone) => providerLabel(zone.dns_provider),
    },
    {
      id: 'records',
      header: '记录',
      align: 'end',
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
      cell: (zone) => (
        <span className='tabular-nums'>{zone.website_count}</span>
      ),
    },
    {
      id: 'status',
      header: '同步状态',
      cell: (zone) => (
        <StatusChip
          {...describeStatus(zoneSyncStatuses, zone.sync_status)}
          reserve={zoneSyncStatusReserve}
        />
      ),
    },
    {
      id: 'synced',
      header: '最近同步',
      cell: (zone) => (
        <RelativeTime
          value={zone.last_synced_at}
          className='text-[13px] text-muted'
        />
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      cell: (zone) => (
        <LinkButton
          href={`/dns-zones/${zone.id}`}
          className='px-2'
          aria-label={`域名 ${zone.domain} 的详情`}
        >
          详情
        </LinkButton>
      ),
    },
  ]
  return (
    <Page
      title='域名'
      description='托管在DNS账号中的域名：自定义记录、系统自动记录与解析线路的同步状态。'
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
              label='DNS账号'
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
              options={zoneSyncFilters}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters(filterKeys)}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isRefetching}
        actions={createAction}
      />
      <DataTable
        label='域名'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => void query.refetch()}
        empty={
          filtered
            ? {
                title: '没有匹配的域名',
                description: '调整搜索词或筛选条件后重试。',
              }
            : {
                title: '还没有域名',
                description:
                  '从DNS账号导入域名后，即可为集群、证书和网站自动维护解析记录。',
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
        isOpen={list.sheet.isCreating}
        onClose={() => void list.sheet.close()}
      />
    </Page>
  )
}
