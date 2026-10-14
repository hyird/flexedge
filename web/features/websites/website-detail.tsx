import { useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import {
  getRouteApi,
  Link,
  Outlet,
  useLocation,
  useNavigate,
} from '@tanstack/react-router'
import {
  ChartLine,
  FileText,
  Gear,
  Globe,
  LayoutHeaderCells,
  Pencil,
  TrashBin,
} from '@gravity-ui/icons'
import { Skeleton, toast } from '@heroui/react'
import { queryKeys } from '@/lib/query-keys'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { ActionMenu } from '@/components/action-menu'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { Mono } from '@/components/mono'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { RouteTabs } from '@/components/route-tabs'
import { StatusChip } from '@/components/status-chip'
import {
  isWebsiteNotFound,
  removeWebsite,
  websiteDetailQuery,
  websiteSnapshotQuery,
} from './data'
import { WebsiteContext } from './website-context'
import { websiteName } from './website-display'

const route = getRouteApi('/_authenticated/websites/$websiteId')

/** `/websites/$websiteId` layout: header, route tabs and the active tab.
 * One live stream per page: the overview and settings tabs subscribe to the
 * website detail stream and the header reuses it; traffic and logs keep
 * their own stream, so the header reads the detail once (or reuses the last
 * live frame) and re-reads it after writes via `useRefreshWebsite`. */
export function WebsiteDetailLayout() {
  const { websiteId } = route.useParams()
  const client = useQueryClient()
  const navigate = useNavigate()
  const base = `/websites/${websiteId}`
  const pathname = useLocation({
    select: (location) => location.pathname.replace(/\/+$/, ''),
  })
  const liveTab = pathname === base || pathname === `${base}/settings`
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
  const [removing, setRemoving] = useState(false)
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
  return (
    <Page>
      <DetailHeader
        icon={<Globe />}
        title={name}
        status={<StatusChip status={website.status} />}
        meta={[
          <Link
            key='cluster'
            to='/clusters/$clusterId'
            params={{ clusterId: website.cluster_id }}
            className='link'
          >
            {website.cluster_name || '未知集群'}
          </Link>,
          <Mono key='access'>{website.access_domain}</Mono>,
          <span key='deploy' className='inline-flex items-center gap-1.5'>
            <StatusChip status={website.runtime.deploy_status} />
            <span className='tabular-nums'>
              {website.runtime.synced_node_count}/
              {website.runtime.target_node_count} 节点
            </span>
          </span>,
        ]}
        actions={
          <LinkButton href={`${base}/settings`}>
            <Pencil />
            编辑配置
          </LinkButton>
        }
        menu={
          <ActionMenu
            label={`网站 ${name} 的更多操作`}
            items={[
              {
                id: 'logs',
                label: '访问日志',
                icon: <FileText />,
                href: `${base}/logs`,
              },
              {
                id: 'delete',
                label: '删除网站',
                icon: <TrashBin />,
                danger: true,
                separated: true,
                onAction: () => setRemoving(true),
              },
            ]}
          />
        }
      />
      {!deleted && (
        <QueryNotice
          query={query}
          onRetry={() => void query.refetch()}
          title={liveTab ? '实时状态已中断' : '网站信息刷新失败'}
        />
      )}
      <RouteTabs
        label='网站详情'
        items={[
          { href: base, label: '概览', icon: <LayoutHeaderCells /> },
          { href: `${base}/traffic`, label: '流量分析', icon: <ChartLine /> },
          { href: `${base}/logs`, label: '访问日志', icon: <FileText /> },
          { href: `${base}/settings`, label: '配置', icon: <Gear /> },
        ]}
      />
      <WebsiteContext value={{ website, refresh }}>
        <Outlet />
      </WebsiteContext>
      <ConfirmDialog
        isOpen={removing}
        onClose={() => setRemoving(false)}
        title={`删除网站 ${name}？`}
        description='网站配置会从所有边缘节点撤下，域名将不再由边缘处理。此操作无法撤销。'
        confirmLabel='删除网站'
        onConfirm={async () => {
          await removeWebsite(website)
          setDeleted(true)
          toast.success(`网站 ${name} 已删除`)
          await navigate({ to: '/websites', replace: true })
          client.removeQueries({
            queryKey: [...queryKeys.websites, website.id],
          })
          void client.invalidateQueries({ queryKey: queryKeys.websites })
        }}
      />
    </Page>
  )
}

function DetailSkeleton() {
  return (
    <div className='flex flex-col gap-6'>
      <DetailHeaderSkeleton />
      <div className='flex gap-4'>
        {Array.from({ length: 5 }, (_, index) => (
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
