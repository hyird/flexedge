import { useState } from 'react'
import {
  keepPreviousData,
  useQuery,
  useQueryClient,
} from '@tanstack/react-query'
import { Link } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { toast } from '@heroui/react'
import type { ListSearchState } from '@/lib/list-search'
import { queryKeys } from '@/lib/query-keys'
import { ActionMenu } from '@/components/action-menu'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DataTable } from '@/components/data-table'
import { LinkButton } from '@/components/link-button'
import { FilterSelect, ListToolbar } from '@/components/list-toolbar'
import { Mono } from '@/components/mono'
import { StatusChip } from '@/components/status-chip'
import { TablePagination } from '@/components/table-pagination'
import { clusterOptionsQuery } from '@/features/clusters/data'
import {
  removeWebsite,
  websitesQuery,
  websitesSnapshotQuery,
  type WebsitesListParams,
} from './data'
import type { Website } from './types'
import {
  defaultOrigin,
  deployStatusReserve,
  originAddress,
  websiteName,
  websiteStatusReserve,
} from './website-display'
import { websiteFilterKeys, type WebsitesSearch } from './websites-search'

const statusOptions = [
  { id: 'enabled', label: '已启用' },
  { id: 'disabled', label: '已停用' },
]
const httpsReserve = ['TLS 1.2+', 'TLS 1.3+', '未启用']

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
}: {
  list: ListSearchState<WebsitesSearch>
  clusterId?: string
  certificateId?: string
  snapshot?: boolean
}) {
  const client = useQueryClient()
  const [removing, setRemoving] = useState<Website | null>(null)
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
      minWidth: 220,
      cell: (website) => {
        const [primary, ...others] = website.config.domains
        return (
          <div className='flex min-w-0 flex-col gap-0.5'>
            <Link
              to='/websites/$websiteId'
              params={{ websiteId: website.id }}
              className='link truncate font-medium'
            >
              {websiteName(website)}
            </Link>
            <span className='flex min-w-0 items-center gap-1.5 text-xs text-muted'>
              {primary ? (
                <Mono truncate className='text-xs'>
                  {primary.hostname}
                </Mono>
              ) : (
                '未绑定域名'
              )}
              {others.length > 0 && (
                <span
                  className='shrink-0'
                  title={others.map((domain) => domain.hostname).join('\n')}
                >
                  +{others.length} 个
                </span>
              )}
            </span>
          </div>
        )
      },
    },
    ...(clusterId
      ? []
      : [
          {
            id: 'cluster',
            header: '集群',
            minWidth: 170,
            cell: (website: Website) => (
              <div className='flex min-w-0 flex-col gap-0.5'>
                <Link
                  to='/clusters/$clusterId'
                  params={{ clusterId: website.cluster_id }}
                  className='link truncate'
                >
                  {website.cluster_name || '未知集群'}
                </Link>
                <Mono truncate className='text-xs text-muted'>
                  {website.access_domain || '—'}
                </Mono>
              </div>
            ),
          } satisfies DataGridColumn<Website>,
        ]),
    {
      id: 'origin',
      header: '默认源站',
      minWidth: 190,
      cell: (website) => {
        const origin = defaultOrigin(website)
        return (
          <div className='flex min-w-0 flex-col gap-0.5'>
            <span className={origin ? 'truncate' : 'truncate text-muted'}>
              {origin ? origin.group : '未配置'}
            </span>
            <Mono truncate className='text-xs text-muted'>
              {origin ? originAddress(origin) : '—'}
            </Mono>
          </div>
        )
      },
    },
    {
      id: 'deployment',
      header: '发布状态',
      minWidth: 180,
      cell: (website) => (
        <div className='flex items-center gap-2'>
          <StatusChip
            status={website.runtime.deploy_status}
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
      minWidth: 110,
      cell: (website) =>
        website.config.https_enabled ? (
          <StatusChip
            label={`TLS ${website.config.minimum_tls_version}+`}
            tone='accent'
            reserve={httpsReserve}
          />
        ) : (
          <StatusChip label='未启用' tone='default' reserve={httpsReserve} />
        ),
    },
    {
      id: 'status',
      header: '状态',
      minWidth: 100,
      cell: (website) => (
        <StatusChip status={website.status} reserve={websiteStatusReserve} />
      ),
    },
    {
      id: 'actions',
      header: <span className='sr-only'>操作</span>,
      width: 56,
      cell: (website) => (
        <ActionMenu
          label={`网站 ${websiteName(website)} 的操作`}
          items={[
            {
              id: 'detail',
              label: '查看详情',
              href: `/websites/${website.id}`,
            },
            {
              id: 'settings',
              label: '编辑配置',
              href: `/websites/${website.id}`,
              state: { website: { tab: 'settings' } },
            },
            {
              id: 'logs',
              label: '访问日志',
              href: `/websites/${website.id}`,
              state: { website: { tab: 'logs' } },
            },
            {
              id: 'delete',
              label: '删除网站',
              danger: true,
              separated: true,
              onAction: () => setRemoving(website),
            },
          ]}
        />
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
              options={statusOptions}
            />
          </>
        }
        hasFilters={filtered}
        onClearFilters={() => void list.clearFilters([...filterKeys])}
        onRefresh={() => query.refetch()}
        isRefreshing={query.isFetching && !query.isPending}
        actions={
          clusterId || certificateId ? undefined : (
            <LinkButton href='/websites/new'>
              <Plus />
              创建网站
            </LinkButton>
          )
        }
      />
      <DataTable
        label='网站'
        columns={columns}
        data={query.data?.list}
        isLoading={query.isPending}
        error={query.isError ? query.error : undefined}
        onRetry={() => void query.refetch()}
        minWidth={clusterId ? 880 : 1040}
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
                    '在网站配置的 HTTPS 分组中选择此证书后会显示在这里。',
                }
              : {
                  title: '还没有网站',
                  description: clusterId
                    ? '这个集群还没有承载网站。创建网站时选择此集群即可。'
                    : '创建网站，把域名接入边缘节点并配置源站。',
                  action: (
                    <LinkButton href='/websites/new'>
                      <Plus />
                      创建网站
                    </LinkButton>
                  ),
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
      <ConfirmDialog
        isOpen={!!removing}
        onClose={() => setRemoving(null)}
        title={`删除网站 ${removing ? websiteName(removing) : ''}？`}
        description='网站配置会从所有边缘节点撤下，域名将不再由边缘处理。此操作无法撤销。'
        confirmLabel='删除网站'
        onConfirm={async () => {
          if (!removing) return
          await removeWebsite(removing)
          toast.success('网站已删除')
          void client.invalidateQueries({ queryKey: queryKeys.websites })
        }}
      />
    </div>
  )
}
