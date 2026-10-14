import { useEffect, useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { getRouteApi, Link } from '@tanstack/react-router'
import { Cubes3, Globe, Pencil, Plus, Server } from '@gravity-ui/icons'
import { Button, Meter, Skeleton } from '@heroui/react'
import { ApiBusinessError } from '@/lib/api-response'
import { formatPercent, ratioOf } from '@/lib/format'
import { useArrivalState } from '@/lib/history-state'
import { statusLabels } from '@/lib/status'
import { useTabSwitchGuard } from '@/lib/unsaved-changes'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { CopyButton } from '@/components/copy-button'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { watchClusterNodeTransitions } from '@/features/nodes/data'
import { providerLabel } from '@/features/providers/provider-display'
import { ClusterActionMenu } from './cluster-action-menu'
import { ClusterNodes } from './cluster-nodes'
import { ClusterSheet } from './cluster-sheet'
import { ClusterWebsites } from './cluster-websites'
import { clusterSnapshotKey, clusterSnapshotQuery } from './data'
import type { Cluster } from './types'

const route = getRouteApi('/_authenticated/clusters/$clusterId')
// service/domains/cluster/cluster_error.h NOT_FOUND
const clusterNotFoundCode = 16401

const tabs = [
  { id: 'nodes', label: '节点', icon: <Server /> },
  { id: 'websites', label: '网站', icon: <Globe /> },
] as const
type Tab = (typeof tabs)[number]['id']
const clusterStatusLabels = statusLabels(['enabled', 'disabled'])

function ClusterSummary({ cluster }: { cluster: Cluster }) {
  const online = ratioOf(cluster.online_node_count, cluster.node_count)
  return (
    <div className='grid gap-4 md:grid-cols-3'>
      <Section title='接入域名'>
        <div className='flex items-center gap-1'>
          <Mono className='min-w-0 break-all'>{cluster.access_domain}</Mono>
          <CopyButton value={cluster.access_domain} label='复制接入域名' />
        </div>
        <p className='mt-1 text-xs text-muted'>网站域名 CNAME 到此地址</p>
      </Section>
      <Section title='托管域名'>
        <Link
          to='/dns-zones/$zoneId'
          params={{ zoneId: cluster.dns_zone_id }}
          className='link font-mono text-[13px]'
        >
          {cluster.dns_zone_domain}
        </Link>
        <p className='mt-1 text-xs text-muted'>
          DNS 服务商 {providerLabel(cluster.dns_provider_name)}
        </p>
      </Section>
      <Section title='节点在线'>
        <Meter
          aria-label='节点在线比例'
          value={online * 100}
          size='sm'
          color={
            cluster.node_count === 0
              ? 'default'
              : online === 1
                ? 'success'
                : online === 0
                  ? 'danger'
                  : 'warning'
          }
          className='flex flex-col gap-2'
        >
          <div className='flex items-baseline justify-between gap-2'>
            <span className='text-xl font-semibold tabular-nums'>
              {cluster.online_node_count}
              <span className='text-sm font-normal text-muted'>
                {' '}
                / {cluster.node_count}
              </span>
            </span>
            <span className='text-xs text-muted tabular-nums'>
              {cluster.node_count ? formatPercent(online, 0) : '无节点'}
            </span>
          </div>
          <Meter.Track>
            <Meter.Fill />
          </Meter.Track>
        </Meter>
      </Section>
    </div>
  )
}

/** `/clusters/$clusterId` with in-page tabs held in component state (the URL
 * never changes): links open a tab with history state `{ cluster: { tab } }`
 * (nodes by default) and `{ cluster: { createNode: true } }` opens the nodes
 * tab with the add-node sheet. Header and summary come from one snapshot of
 * `/api/clusters/:id/stream`; the active tab's list owns the page's only live
 * stream. Writes on this page re-read the snapshot, and so does an
 * online/offline transition or node add/remove reported by the live 节点 list
 * (once per burst; runtime metric patches never trigger it). */
export function ClusterDetailLayout() {
  const { clusterId } = route.useParams()
  const [view, setView, arrivalKey] = useArrivalState<{
    tab: Tab
    creatingNode: boolean
  }>((state) => ({
    tab: state.cluster?.createNode ? 'nodes' : (state.cluster?.tab ?? 'nodes'),
    creatingNode: state.cluster?.createNode === true,
  }))
  const tabGuard = useTabSwitchGuard()
  const client = useQueryClient()
  useEffect(
    () =>
      watchClusterNodeTransitions(client, clusterId, () => {
        void client.invalidateQueries({
          queryKey: [...clusterSnapshotKey, clusterId],
        })
      }),
    [client, clusterId]
  )
  const query = useQuery(clusterSnapshotQuery(clusterId))
  const cluster = query.data
  const [editing, setEditing] = useState(false)
  useBreadcrumbLabel(cluster?.name)
  if (
    query.error instanceof ApiBusinessError &&
    query.error.code === clusterNotFoundCode
  )
    return (
      <Page>
        <EmptyState
          size='lg'
          title='集群不存在'
          description='集群可能已被删除，或链接不正确。'
          action={
            <LinkButton href='/clusters' variant='tertiary'>
              返回集群列表
            </LinkButton>
          }
        />
      </Page>
    )
  return (
    <Page>
      {cluster ? (
        <DetailHeader
          icon={<Cubes3 />}
          title={cluster.name}
          status={
            <StatusChip status={cluster.status} reserve={clusterStatusLabels} />
          }
          meta={[
            <Mono key='domain'>{cluster.access_domain}</Mono>,
            <span key='nodes' className='whitespace-nowrap tabular-nums'>
              {cluster.node_count} 个节点
            </span>,
          ]}
          actions={
            <>
              <Button
                size='sm'
                variant='tertiary'
                onPress={() => setEditing(true)}
              >
                <Pencil className='size-4' />
                编辑集群
              </Button>
              <Button
                size='sm'
                onPress={() =>
                  tabGuard.request(() =>
                    setView({ tab: 'nodes', creatingNode: true })
                  )
                }
              >
                <Plus className='size-4' />
                添加节点
              </Button>
            </>
          }
          menu={
            <ClusterActionMenu
              cluster={cluster}
              context='detail'
              onEdit={() => setEditing(true)}
            />
          }
        />
      ) : (
        <DetailHeaderSkeleton />
      )}
      <QueryNotice query={query} onRetry={() => void query.refetch()} />
      {cluster ? (
        <ClusterSummary cluster={cluster} />
      ) : (
        <div className='grid gap-4 md:grid-cols-3'>
          {Array.from({ length: 3 }, (_, index) => (
            <Skeleton key={index} className='h-28 rounded-2xl' />
          ))}
        </div>
      )}
      <PageTabs
        label='集群详情'
        items={tabs}
        selected={view.tab}
        onSelectionChange={(tab) => setView({ tab, creatingNode: false })}
        guard={tabGuard}
      >
        {/* The tab's live list opens after the header's one-shot read, so
            the page never holds two connections at once. Tab content is
            keyed by arrival so a new arrival starts from a clean list. */}
        {query.isFetchedAfterMount &&
          (view.tab === 'nodes' ? (
            <ClusterNodes
              key={`${arrivalKey}:nodes`}
              isCreating={view.creatingNode}
              onCreatingChange={(creatingNode) =>
                setView({ tab: 'nodes', creatingNode })
              }
            />
          ) : (
            <ClusterWebsites key={`${arrivalKey}:websites`} />
          ))}
      </PageTabs>
      {cluster && (
        <ClusterSheet
          isOpen={editing}
          cluster={cluster}
          onClose={() => setEditing(false)}
          onSaved={() =>
            void client.invalidateQueries({ queryKey: clusterSnapshotKey })
          }
        />
      )}
    </Page>
  )
}
