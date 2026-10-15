import { useEffect } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { getRouteApi, Link } from '@tanstack/react-router'
import { ArrowLeft, Cubes3, Gear, Globe, Server } from '@gravity-ui/icons'
import { KPI } from '@heroui-pro/react/kpi'
import { Skeleton } from '@heroui/react'
import { ApiBusinessError } from '@/lib/api-response'
import { ratioOf } from '@/lib/format'
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
import { StatusChip } from '@/components/status-chip'
import { watchClusterNodeTransitions } from '@/features/nodes/data'
import { ClusterConfig } from './cluster-config'
import { ClusterNodes } from './cluster-nodes'
import { ClusterWebsites } from './cluster-websites'
import { clusterSnapshotKey, clusterSnapshotQuery } from './data'
import type { Cluster } from './types'

const route = getRouteApi('/_authenticated/clusters/$clusterId')
// service/domains/cluster/cluster_error.h NOT_FOUND
const clusterNotFoundCode = 16401

const tabs = [
  { id: 'nodes', label: '节点', icon: <Server /> },
  { id: 'websites', label: '网站', icon: <Globe /> },
  { id: 'config', label: '配置', icon: <Gear /> },
] as const
type Tab = (typeof tabs)[number]['id']
const clusterStatusLabels = statusLabels(['enabled', 'disabled'])

function ClusterSummary({ cluster }: { cluster: Cluster }) {
  const online = ratioOf(cluster.online_node_count, cluster.node_count)
  return (
    <div className='grid gap-3 md:grid-cols-3'>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>接入域名</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          <dd className='flex min-w-0 items-center gap-1'>
            <Mono className='min-w-0 text-base font-medium break-all'>
              {cluster.access_domain}
            </Mono>
            <CopyButton value={cluster.access_domain} label='复制接入域名' />
          </dd>
        </KPI.Content>
      </KPI>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>托管域名</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          <dd className='min-w-0 truncate'>
            <Link
              to='/dns-zones/$zoneId'
              params={{ zoneId: cluster.dns_zone_id }}
              className='link font-mono text-base font-medium'
            >
              {cluster.dns_zone_domain}
            </Link>
          </dd>
        </KPI.Content>
      </KPI>
      <KPI className='min-w-0'>
        <KPI.Header>
          <KPI.Title>节点在线</KPI.Title>
        </KPI.Header>
        <KPI.Content className='content-start'>
          <KPI.Value
            value={cluster.online_node_count}
            className='whitespace-nowrap'
          >
            {(formatted) => `${formatted} / ${cluster.node_count}`}
          </KPI.Value>
          <KPI.Progress
            value={online * 100}
            status={
              online === 1 ? 'success' : online === 0 ? 'danger' : 'warning'
            }
            className={cluster.node_count ? undefined : 'invisible'}
          />
        </KPI.Content>
      </KPI>
    </div>
  )
}

/** `/clusters/$clusterId` with in-page tabs held in component state (the URL
 * never changes; 节点 by default). Header and summary come from one snapshot
 * of `/api/clusters/:id/stream`, which the 配置 tab edits; the 节点/网站 tab's
 * list owns the page's only live stream. Writes on this page re-read the
 * snapshot, and so does an online/offline transition or node add/remove
 * reported by the live 节点 list (once per burst; runtime metric patches
 * never trigger it). */
export function ClusterDetailLayout() {
  const { clusterId } = route.useParams()
  // Each navigation to a cluster (even cluster to cluster) starts on 节点.
  const [tab, setTab, arrivalKey] = useArrivalState<Tab>(() => 'nodes')
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
          action={<LinkButton href='/clusters'>返回集群列表</LinkButton>}
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
            <LinkButton href='/clusters'>
              <ArrowLeft />
              返回
            </LinkButton>
          }
        />
      ) : (
        <DetailHeaderSkeleton />
      )}
      <QueryNotice query={query} onRetry={() => void query.refetch()} />
      {cluster ? (
        <ClusterSummary cluster={cluster} />
      ) : (
        <div className='grid gap-3 md:grid-cols-3'>
          {Array.from({ length: 3 }, (_, index) => (
            <Skeleton key={index} className='h-28 rounded-2xl' />
          ))}
        </div>
      )}
      <PageTabs
        label='集群详情'
        items={tabs}
        selected={tab}
        onSelectionChange={setTab}
        guard={tabGuard}
      >
        {/* The tab's live list opens after the header's one-shot read, so
            the page never holds two connections at once. */}
        {query.isFetchedAfterMount &&
          (tab === 'nodes' ? (
            <ClusterNodes key={arrivalKey} />
          ) : tab === 'websites' ? (
            <ClusterWebsites key={arrivalKey} />
          ) : (
            <ClusterConfig key={arrivalKey} />
          ))}
      </PageTabs>
    </Page>
  )
}
