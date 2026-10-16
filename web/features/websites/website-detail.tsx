import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import {
  ArrowLeft,
  ChartLine,
  FileText,
  Gear,
  Globe,
  LayoutHeaderCells,
} from '@gravity-ui/icons'
import { Skeleton } from '@heroui/react'
import { useArrivalState } from '@/lib/history-state'
import { useTabSwitchGuard } from '@/lib/unsaved-changes'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { StatusChip } from '@/components/status-chip'
import { AccessLogs } from './access-logs'
import {
  isWebsiteNotFound,
  websiteDetailQuery,
  websiteSnapshotQuery,
} from './data'
import {
  WebsiteContext,
  type WebsiteArrival,
  type WebsiteDetailTab,
} from './website-context'
import {
  deployStatusReserve,
  websiteName,
  websiteStatusReserve,
} from './website-display'
import { WebsiteLifecycleActions } from './website-lifecycle-actions'
import { WebsiteOverview } from './website-overview'
import { WebsiteSettingsPage } from './website-settings'
import { WebsiteTraffic } from './website-traffic'

const route = getRouteApi('/_authenticated/websites/$websiteId')

const tabItems = [
  { id: 'overview', label: '概览', icon: <LayoutHeaderCells /> },
  { id: 'traffic', label: '流量分析', icon: <ChartLine /> },
  { id: 'logs', label: '访问日志', icon: <FileText /> },
  { id: 'settings', label: '配置', icon: <Gear /> },
] as const

/** `/websites/$websiteId`: header, in-page tabs and the active tab. The tab is
 * component state that never touches the URL; each navigation to the page
 * starts on the overview, and `openTab` (links inside tabs) may seed the
 * opened tab's starting values. The header keeps the return and delete
 * actions together across tabs.
 * One live stream per page: the overview and settings tabs subscribe to the
 * website detail stream and the header reuses it; traffic and logs keep
 * their own stream, so the header reads the detail once (or reuses the last
 * live frame) and re-reads it after writes via `useRefreshWebsite`. Only the
 * selected tab is mounted, so leaving a tab closes its stream. */
export function WebsiteDetailLayout() {
  const { websiteId } = route.useParams()
  const [view, setView, arrivalKey] = useArrivalState<{
    tab: WebsiteDetailTab
    initial?: WebsiteArrival
  }>(() => ({ tab: 'overview' }))
  const { tab } = view
  const tabGuard = useTabSwitchGuard()
  // Links inside tabs ask before leaving a dirty settings draft, like
  // clicking another tab.
  const openTab = (next: WebsiteDetailTab, initial?: WebsiteArrival) => {
    if (next !== tab || initial)
      tabGuard.request(() => setView({ tab: next, initial }))
  }
  const liveTab = tab === 'overview' || tab === 'settings'
  const live = useQuery({ ...websiteDetailQuery(websiteId), enabled: liveTab })
  const snapshot = useQuery({
    ...websiteSnapshotQuery(websiteId),
    // A frame from the live tab is just as fresh; reading again would open a
    // second stream next to the tab's own.
    enabled: !liveTab && !live.data,
  })
  const showLive =
    liveTab || (!!live.data && live.dataUpdatedAt >= snapshot.dataUpdatedAt)
  const query = showLive ? live : snapshot
  // After a confirmed delete the stream reports "not found" before navigation
  // settles; keep showing the last snapshot instead of flashing that state.
  const [deleted, setDeleted] = useState(false)
  const website = query.data
  useBreadcrumbLabel(website ? websiteName(website) : undefined)
  const refresh = () => {
    if (!liveTab) void snapshot.refetch()
  }

  if (!deleted && query.isError && isWebsiteNotFound(query.error))
    return (
      <Page>
        <EmptyState
          size='lg'
          icon={<Globe />}
          title='网站不存在'
          description='该网站可能已被删除，或链接中的网站 ID 有误。'
          action={<LinkButton href='/websites'>返回网站列表</LinkButton>}
        />
      </Page>
    )
  if (!website)
    return (
      <Page>
        {query.isError ? (
          <QueryNotice
            query={query}
            onRetry={() => void query.refetch()}
            title='网站加载失败'
          />
        ) : (
          <DetailSkeleton />
        )}
      </Page>
    )

  const name = websiteName(website)
  const tabKey = `${arrivalKey}:${tab}`
  const content =
    tab === 'overview' ? (
      <WebsiteOverview key={tabKey} />
    ) : tab === 'traffic' ? (
      <WebsiteTraffic key={tabKey} />
    ) : tab === 'logs' ? (
      <AccessLogs key={tabKey} />
    ) : (
      <WebsiteSettingsPage key={tabKey} initial={view.initial} />
    )
  return (
    <WebsiteContext
      value={{
        website,
        refresh,
        openTab,
        markDeleted: () => setDeleted(true),
      }}
    >
      <Page>
        <DetailHeader
          icon={<Globe />}
          title={name}
          status={
            <StatusChip
              status={website.status}
              reserve={websiteStatusReserve}
            />
          }
          meta={[
            <span key='cluster'>{website.cluster_name || '未知集群'}</span>,
            <Mono key='access'>{website.access_domain}</Mono>,
            <span key='deploy' className='inline-flex items-center gap-1.5'>
              <StatusChip
                status={website.runtime.deploy_status}
                reserve={deployStatusReserve}
              />
              <span className='tabular-nums'>
                {website.runtime.synced_node_count}/
                {website.runtime.target_node_count} 节点
              </span>
            </span>,
          ]}
          actions={
            <>
              <LinkButton href='/websites'>
                <ArrowLeft />
                返回
              </LinkButton>
              <WebsiteLifecycleActions />
            </>
          }
        />
        {!deleted && (
          <QueryNotice
            query={query}
            onRetry={() => void query.refetch()}
            title={liveTab ? '实时状态已中断' : '网站信息刷新失败'}
          />
        )}
        <PageTabs
          label='网站详情'
          items={tabItems}
          selected={tab}
          guard={tabGuard}
          onSelectionChange={(next) =>
            setView({ tab: next, initial: undefined })
          }
        >
          {content}
        </PageTabs>
      </Page>
    </WebsiteContext>
  )
}

function DetailSkeleton() {
  return (
    <div className='flex flex-col gap-6'>
      <DetailHeaderSkeleton />
      <div className='flex gap-4'>
        {Array.from({ length: 4 }, (_, index) => (
          <Skeleton key={index} className='h-8 w-20 rounded-md' />
        ))}
      </div>
      <div className='grid gap-4 sm:grid-cols-3'>
        {Array.from({ length: 3 }, (_, index) => (
          <Skeleton key={index} className='h-28 rounded-2xl' />
        ))}
      </div>
      <Skeleton className='h-64 rounded-2xl' />
    </div>
  )
}
