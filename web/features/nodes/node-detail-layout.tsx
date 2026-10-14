import { useQuery } from '@tanstack/react-query'
import { getRouteApi, Link } from '@tanstack/react-router'
import { ChartLine, FileText, Gear, Server } from '@gravity-ui/icons'
import { ApiBusinessError } from '@/lib/api-response'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { StatusChip } from '@/components/status-chip'
import { nodeQuery, nodeSnapshotQuery } from './data'
import { NodeActionMenu } from './node-action-menu'
import { NodeConfig } from './node-config'
import { NodeLogs } from './node-logs'
import { NodeOverview } from './node-overview'
import type { NodeDetailTab } from './node-search'
import { connectionLabels, nodeStatusLabels } from './node-status'
import { RegistrationChip } from './registration-chip'

const route = getRouteApi('/_authenticated/nodes/$nodeId')
// service/domains/node/node_error.h NOT_FOUND
const nodeNotFoundCode = 16501

type Tab = 'overview' | NodeDetailTab
const tabs = [
  { id: 'overview', label: '概览', icon: <ChartLine /> },
  { id: 'logs', label: '日志', icon: <FileText /> },
  { id: 'config', label: '配置', icon: <Gear /> },
] as const

/** `/nodes/$nodeId` with in-page tabs (`?tab=logs|config`, overview default).
 * One live stream per page: the overview and config tabs share
 * `/api/nodes/:id/stream` with this header, while the logs tab owns the log
 * stream and the header reads the node once (re-read after writes). */
export function NodeDetailLayout() {
  const { nodeId } = route.useParams()
  const { tab = 'overview' } = route.useSearch()
  const navigate = route.useNavigate()
  const onLogs = tab === 'logs'
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
  const selectTab = (next: Tab) =>
    void navigate({
      to: '.',
      search: (previous) => ({
        ...previous,
        tab: next === 'overview' ? undefined : next,
      }),
      replace: true,
    })
  return (
    <Page>
      {node ? (
        <DetailHeader
          icon={<Server />}
          title={node.name}
          status={
            <StatusChip
              status={node.runtime.connection_status}
              reserve={connectionLabels}
            />
          }
          meta={[
            <RegistrationChip
              key='registration'
              status={node.runtime.registration_status}
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
            <span key='version'>
              Agent <Mono>{node.runtime.agent_version || '—'}</Mono>
            </span>,
            <StatusChip
              key='status'
              status={node.status}
              reserve={nodeStatusLabels}
            />,
          ]}
          actions={
            <LinkButton href={`/nodes/${nodeId}?tab=config`} variant='tertiary'>
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
      <PageTabs
        label='节点详情'
        items={tabs}
        selected={tab}
        onSelectionChange={selectTab}
      >
        {tab === 'overview' ? (
          <NodeOverview />
        ) : tab === 'logs' ? (
          <NodeLogs />
        ) : (
          <NodeConfig />
        )}
      </PageTabs>
    </Page>
  )
}
