import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { getRouteApi, Link } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { Button } from '@heroui/react'
import { hasListFilters, useListSearch } from '@/lib/list-search'
import {
  describeStatus,
  enablementStatuses,
  statusOptions,
  statusReserve,
} from '@/lib/status'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { Page } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { dnsZoneOptionsQuery } from '@/features/dns-zones/data'
import { clusterFilterKeys, type ClustersSearch } from './cluster-search'
import { ClusterSheet } from './cluster-sheet'
import { clustersQuery } from './data'
import type { Cluster } from './types'

const route = getRouteApi('/_authenticated/clusters/')
const clusterStatusReserve = statusReserve(enablementStatuses)

export function ClustersPage() {
  const list = useListSearch(route)
  const { search } = list
  const zones = useQuery(dnsZoneOptionsQuery)
  const query = useQuery({
    ...clustersQuery({
      page: list.page,
      page_size: list.pageSize,
      keyword: list.keyword || undefined,
      status: search.status,
      dns_zone_id: search.dns_zone_id,
    }),
    placeholderData: keepPreviousData,
  })
  const filtered = hasListFilters(search, clusterFilterKeys)
  const columns: DataGridColumn<Cluster>[] = [
    {
      id: 'name',
      header: '集群',
      isRowHeader: true,
      minWidth: 180,
      cell: (cluster) => (
        <span className='block truncate font-medium'>{cluster.name}</span>
      ),
    },
    {
      id: 'access-domain',
      header: '接入域名',
      minWidth: 200,
      cell: (cluster) => <Mono truncate>{cluster.access_domain}</Mono>,
    },
    {
      id: 'zone',
      header: '托管域名',
      minWidth: 180,
      cell: (cluster) => (
        <Link
          to='/dns-zones/$zoneId'
          params={{ zoneId: cluster.dns_zone_id }}
          className='link block truncate text-sm'
        >
          {cluster.dns_zone_domain}
        </Link>
      ),
    },
    {
      id: 'nodes',
      header: '节点在线',
      minWidth: 120,
      cell: (cluster) => (
        <span className='text-sm whitespace-nowrap tabular-nums'>
          {cluster.online_node_count} / {cluster.node_count}
        </span>
      ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 100,
      cell: (cluster) => (
        <StatusChip
          {...describeStatus(enablementStatuses, cluster.status)}
          reserve={clusterStatusReserve}
        />
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      width: 72,
      cell: (cluster) => (
        <LinkButton
          href={`/clusters/${cluster.id}`}
          className='px-2'
          aria-label={`集群 ${cluster.name} 的详情`}
        >
          详情
        </LinkButton>
      ),
    },
  ]
  const createButton = (
    <Button size='sm' onPress={() => void list.sheet.openCreate()}>
      <Plus className='size-4' />
      创建集群
    </Button>
  )
  return (
    <Page
      title='集群'
      description='按集群组织边缘节点；每个集群拥有一个接入域名，网站通过 CNAME 接入。'
    >
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索集群名称',
        }}
        filters={
          <>
            <FilterSelect
              label='状态'
              value={search.status ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  status: (value || undefined) as ClustersSearch['status'],
                })
              }
              options={statusOptions(enablementStatuses)}
            />
            <FilterSelect
              label='托管域名'
              value={search.dns_zone_id ?? ''}
              onChange={(value) =>
                void list.setSearch({ dns_zone_id: value || undefined })
              }
              options={(zones.data ?? []).map((zone) => ({
                id: zone.id,
                label: zone.domain,
              }))}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters(clusterFilterKeys)}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isFetching && !query.isPending}
        actions={createButton}
      />
      <DataTable
        label='集群列表'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => void query.refetch()}
        minWidth={760}
        empty={
          filtered
            ? {
                title: '没有匹配的集群',
                description: '调整搜索或筛选条件后重试。',
              }
            : {
                title: '还没有集群',
                description: '创建集群并绑定托管域名后，即可添加边缘节点。',
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
      <ClusterSheet
        isOpen={list.sheet.isCreating}
        onClose={() => void list.sheet.close()}
      />
    </Page>
  )
}
