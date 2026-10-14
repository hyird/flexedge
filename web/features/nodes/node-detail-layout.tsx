import { useQuery } from '@tanstack/react-query'
import { getRouteApi, Link, Outlet, useMatch } from '@tanstack/react-router'
import { ChartLine, FileText, Gear, Server } from '@gravity-ui/icons'
import { ApiBusinessError } from '@/lib/api-response'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { RouteTabs } from '@/components/route-tabs'
import { StatusChip } from '@/components/status-chip'
import { nodeQuery, nodeSnapshotQuery } from './data'
import { NodeActionMenu } from './node-action-menu'

const route = getRouteApi('/_authenticated/nodes/$nodeId')
// service/domains/node/node_error.h NOT_FOUND
const nodeNotFoundCode = 16501

/** `/nodes/$nodeId`. One live stream per page: the overview and config tabs
 * share `/api/nodes/:id/stream` with this header, while the logs tab owns the
 * log stream and the header reads the node once (re-read after writes). */
export function NodeDetailLayout() {
  const { nodeId } = route.useParams()
  const onLogs = !!useMatch({
    from: '/_authenticated/nodes/$nodeId/logs',
    shouldThrow: false,
  })
  const live = useQuery({ ...nodeQuery(nodeId), enabled: !onLogs })
  const snapshot = useQuery({ ...nodeSnapshotQuery(nodeId), enabled: onLogs })
  const query = onLogs ? snapshot : live
  // Entering the logs tab keeps the last live frame until the snapshot lands.
  const node = onLogs ? (snapshot.data ?? live.data) : live.data
  useBreadcrumbLabel(node?.name)
  const notFound =
    query.error instanceof ApiBusinessError &&
    query.error.code === nodeNotFoundCode
  if (notFound)
    return (
      <Page>
        <EmptyState
          size='lg'
          title='节点不存在'
          description='节点可能已被删除，或链接不正确。'
          action={
            <LinkButton href='/nodes' variant='tertiary'>
              返回节点列表
            </LinkButton>
          }
        />
      </Page>
    )
  const base = `/nodes/${nodeId}`
  return (
    <Page>
      {node ? (
        <DetailHeader
          icon={<Server />}
          title={node.name}
          status={<StatusChip status={node.runtime.connection_status} />}
          meta={[
            <StatusChip
              key='registration'
              status={node.runtime.registration_status}
              label={
                node.runtime.registration_status === 'pending'
                  ? '等待接入'
                  : undefined
              }
            />,
            <span key='cluster'>
              集群{' '}
              <Link
                to='/clusters/$clusterId'
                params={{ clusterId: node.cluster_id }}
                className='link'
              >
                {node.cluster_name}
              </Link>
            </span>,
            node.runtime.agent_version ? (
              <span key='version'>
                版本 <Mono>{node.runtime.agent_version}</Mono>
              </span>
            ) : null,
            <StatusChip key='status' status={node.status} />,
          ]}
          actions={
            <LinkButton href={`${base}/config`} variant='tertiary'>
              <Gear />
              编辑配置
            </LinkButton>
          }
          menu={<NodeActionMenu node={node} context='detail' />}
        />
      ) : (
        <DetailHeaderSkeleton />
      )}
      <QueryNotice query={query} onRetry={() => void query.refetch()} />
      <RouteTabs
        label='节点详情'
        items={[
          { href: base, label: '概览', icon: <ChartLine /> },
          { href: `${base}/logs`, label: '日志', icon: <FileText /> },
          { href: `${base}/config`, label: '配置', icon: <Gear /> },
        ]}
      />
      <Outlet />
    </Page>
  )
}
