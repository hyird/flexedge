import { keepPreviousData, useQuery } from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import type { ListSearchState } from '@/lib/list-search'
import {
  describeStatus,
  enablementStatuses,
  statusOptions,
  statusReserve,
  type StatusDictionary,
} from '@/lib/status'
import { cn } from '@/lib/utils'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { clusterOptionsQuery } from '@/features/clusters/data'
import {
  websitesQuery,
  websitesSnapshotQuery,
  type WebsitesListParams,
} from './data'
import type { Website } from './types'
import {
  defaultOrigin,
  originGroupName,
  deployStatuses,
  deployStatusReserve,
  websiteName,
  websiteStatusReserve,
} from './website-display'
import { websiteFilterKeys, type WebsitesSearch } from './websites-search'

const websiteStatusOptions = statusOptions(enablementStatuses)
/** HTTPS column: the minimum TLS version, or disabled. */
const httpsStatuses = {
  '1.2': { label: 'TLS 1.2+', tone: 'accent' },
  '1.3': { label: 'TLS 1.3+', tone: 'accent' },
  disabled: { label: '未启用', tone: 'default' },
} as const satisfies StatusDictionary
const httpsReserve = statusReserve(httpsStatuses)

/** Websites list body (toolbar, table, pagination). With `clusterId` the list
 * is fixed to that cluster and the cluster filter and column are hidden; with
 * `certificateId` it shows only websites bound to that certificate. Embedded
 * lists (either prop) have no create action in the toolbar. `snapshot` reads
 * each page once and closes the stream (for pages whose one live stream is
 * another resource); the refresh button and `websitesSnapshotKey`
 * invalidation re-read it. */
export function WebsitesTable({
  list,
  clusterId,
  certificateId,
  snapshot = false,
  variant = 'primary',
}: {
  list: ListSearchState<WebsitesSearch>
  clusterId?: string
  certificateId?: string
  snapshot?: boolean
  variant?: 'primary' | 'secondary'
}) {
  const clusters = useQuery({ ...clusterOptionsQuery, enabled: !clusterId })
  const params: WebsitesListParams = {
    page: list.page,
    page_size: list.pageSize,
    keyword: list.keyword || undefined,
    cluster_id: clusterId ?? list.search.cluster_id,
    certificate_id: certificateId,
    status: list.search.status,
  }
  // Exactly one of the two observers is enabled, so only one read happens.
  const liveQuery = useQuery({
    ...websitesQuery(params),
    enabled: !snapshot,
    placeholderData: keepPreviousData,
  })
  const snapshotQuery = useQuery({
    ...websitesSnapshotQuery(params),
    enabled: snapshot,
    placeholderData: keepPreviousData,
  })
  const query = snapshot ? snapshotQuery : liveQuery
  const filterKeys = clusterId ? (['status'] as const) : websiteFilterKeys
  const filtered =
    !!list.keyword || filterKeys.some((key) => list.search[key] !== undefined)
  const columns: DataGridColumn<Website>[] = [
    {
      id: 'name',
      header: '网站',
      isRowHeader: true,
      cell: (website) => (
        <span className='block truncate font-medium'>
          {websiteName(website)}
        </span>
      ),
    },
    {
      id: 'domains',
      header: '域名',
      cell: (website) => {
        const [primary, ...others] = website.config.domains
        return primary ? (
          <span className='flex min-w-0 items-center gap-1.5'>
            <Mono truncate className='min-w-0'>
              {primary.hostname}
            </Mono>
            {others.length > 0 && (
              <span
                className='shrink-0 text-muted'
                title={others.map((domain) => domain.hostname).join('\n')}
              >
                +{others.length} 个
              </span>
            )}
          </span>
        ) : (
          <span className='text-muted'>未绑定域名</span>
        )
      },
    },
    ...(clusterId
      ? []
      : [
          {
            id: 'cluster',
            header: '集群',
            cell: (website: Website) => (
              <Link
                to='/clusters/$clusterId'
                params={{ clusterId: website.cluster_id }}
                className='link block truncate'
              >
                {website.cluster_name || '未知集群'}
              </Link>
            ),
          } satisfies DataGridColumn<Website>,
        ]),
    {
      id: 'origin',
      header: '默认源站',
      cell: (website) => {
        const origin = defaultOrigin(website)
        return (
          <span
            className={cn('block max-w-52 truncate', !origin && 'text-muted')}
          >
            {origin ? originGroupName(origin.group) : '未配置'}
          </span>
        )
      },
    },
    {
      id: 'deployment',
      header: '发布状态',
      cell: (website) => (
        <div className='flex items-center gap-2'>
          <StatusChip
            {...describeStatus(deployStatuses, website.runtime.deploy_status)}
            reserve={deployStatusReserve}
          />
          <span className='text-xs text-muted tabular-nums'>
            {website.runtime.synced_node_count}/
            {website.runtime.target_node_count} 节点
          </span>
        </div>
      ),
    },
    {
      id: 'https',
      header: 'HTTPS',
      cell: (website) => (
        <StatusChip
          {...describeStatus(
            httpsStatuses,
            website.config.https_enabled
              ? website.config.minimum_tls_version
              : 'disabled'
          )}
          reserve={httpsReserve}
        />
      ),
    },
    {
      id: 'status',
      header: '状态',
      cell: (website) => (
        <StatusChip
          {...describeStatus(enablementStatuses, website.status)}
          reserve={websiteStatusReserve}
        />
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      cell: (website) => (
        <LinkButton
          href={`/websites/${website.id}`}
          className='px-2'
          aria-label={`网站 ${websiteName(website)} 的详情`}
        >
          详情
        </LinkButton>
      ),
    },
  ]
  return (
    <div className='flex min-w-0 flex-col gap-4'>
      <ListToolbar
        search={{
          value: list.keyword,
          onChange: (q) => void list.setSearch({ q }),
          placeholder: '搜索名称或域名',
        }}
        filters={
          <>
            {!clusterId && (
              <FilterSelect
                label='集群'
                value={list.search.cluster_id ?? ''}
                onChange={(value) =>
                  void list.setSearch({ cluster_id: value || undefined })
                }
                options={(clusters.data ?? []).map((cluster) => ({
                  id: cluster.id,
                  label: cluster.name,
                }))}
              />
            )}
            <FilterSelect
              label='状态'
              value={list.search.status ?? ''}
              onChange={(value) =>
                void list.setSearch({
                  status: (value || undefined) as WebsitesSearch['status'],
                })
              }
              options={websiteStatusOptions}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters([...filterKeys])}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isFetching && !query.isPending}
        actions={
          clusterId || certificateId ? undefined : (
            <LinkButton href='/websites/new' variant='primary'>
              <Plus />
              创建网站
            </LinkButton>
          )
        }
      />
      <DataTable
        label='网站'
        variant={variant}
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => void query.refetch()}
        empty={
          filtered
            ? {
                title: '没有匹配的网站',
                description: '调整搜索词或筛选条件后重试。',
              }
            : certificateId
              ? {
                  title: '没有网站引用此证书',
                  description:
                    '在网站配置的HTTPS分组中选择此证书后会显示在这里。',
                }
              : {
                  title: '还没有网站',
                  description: clusterId
                    ? '这个集群还没有承载网站。创建网站时选择此集群即可。'
                    : '创建网站，把域名接入边缘节点并配置源站。',
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
    </div>
  )
}
