import { getRouteApi, useNavigate } from '@tanstack/react-router'
import {
  ArrowsRotateRight,
  Globe,
  PencilToSquare,
  TrashBin,
} from '@gravity-ui/icons'
import { Button, Link } from '@heroui/react'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { ActionMenu, type ActionMenuItem } from '@/components/action-menu'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { providerLabel } from '@/features/providers/provider-display'
import {
  displaySyncStatus,
  hasMeaningfulConflicts,
  isZoneNotFound,
  zoneSyncStatusLabels,
} from './dns-zone-display'
import { useZoneDetail } from './use-zone-detail'
import { useZoneCommands } from './zone-commands'
import { ZoneLines } from './zone-lines'
import { ZoneOverview } from './zone-overview'
import { ZoneRecords } from './zone-records'

const route = getRouteApi('/_authenticated/dns-zones/$zoneId')
type ZoneTab = 'overview' | 'records' | 'lines'
const tabs = [
  { id: 'overview', label: '概览' },
  { id: 'records', label: '记录' },
  { id: 'lines', label: '解析线路' },
] as const

export function ZoneDetailLayout() {
  const navigate = useNavigate()
  const routeNavigate = route.useNavigate()
  const tab: ZoneTab =
    route.useSearch({ select: (search) => search.tab }) ?? 'overview'
  // The layout's query is the zone's live detail stream; every tab reads the
  // same query key, so the page keeps exactly one connection.
  const query = useZoneDetail()
  const zone = query.data
  useBreadcrumbLabel(zone?.domain)
  const commands = useZoneCommands({
    onRemoved: () => void navigate({ to: '/dns-zones' }),
  })
  if (!zone) {
    if (query.isError && isZoneNotFound(query.error))
      return (
        <Page>
          <EmptyState
            title='DNS 区域不存在'
            description='该区域可能已被移除，或链接有误。'
            action={<LinkButton href='/dns-zones'>返回 DNS 区域</LinkButton>}
          />
        </Page>
      )
    return (
      <Page>
        <QueryNotice
          query={query}
          onRetry={() => void query.refetch()}
          title='DNS 区域加载失败'
        />
        {query.isPending && <DetailHeaderSkeleton />}
      </Page>
    )
  }
  const selectTab = (next: ZoneTab) =>
    void routeNavigate({
      // q/type/source/edit only belong to the records tab.
      search: (previous) => ({
        ...previous,
        tab: next === 'overview' ? undefined : next,
        q: undefined,
        type: undefined,
        source: undefined,
        edit: undefined,
      }),
      replace: true,
    })
  const conflictItems: ActionMenuItem[] = hasMeaningfulConflicts(zone)
    ? [
        {
          id: 'local',
          label: '使用本地记录覆盖服务商',
          onAction: () => commands.resolve(zone, 'local'),
        },
        {
          id: 'remote',
          label: '使用服务商记录替换本地',
          onAction: () => commands.resolve(zone, 'remote'),
        },
      ]
    : []
  return (
    <Page>
      <QueryNotice
        query={query}
        onRetry={() => void query.refetch()}
        title='实时连接中断，显示的是最后一次收到的数据'
      />
      <DetailHeader
        icon={<Globe />}
        title={zone.domain}
        status={
          <StatusChip
            status={displaySyncStatus(zone)}
            reserve={zoneSyncStatusLabels}
          />
        }
        meta={[
          <Link
            key='provider'
            href={`/providers?q=${encodeURIComponent(zone.dns_provider_name)}`}
          >
            {zone.dns_provider_name} · {providerLabel(zone.dns_provider)}
          </Link>,
          <span key='synced'>
            最近同步 <RelativeTime value={zone.last_synced_at} />
          </span>,
        ]}
        actions={
          <>
            <Button
              size='sm'
              variant='tertiary'
              isPending={commands.syncingId === zone.id}
              onPress={() => commands.sync(zone)}
            >
              <ArrowsRotateRight className='size-4' />
              立即同步
            </Button>
            <LinkButton href={`/dns-zones/${zone.id}?tab=records&edit=1`}>
              <PencilToSquare />
              编辑记录
            </LinkButton>
          </>
        }
        menu={
          <ActionMenu
            label={`DNS 区域 ${zone.domain} 的更多操作`}
            items={[
              ...conflictItems,
              {
                id: 'remove',
                label: '移除区域',
                icon: <TrashBin />,
                danger: true,
                separated: conflictItems.length > 0,
                onAction: () => commands.remove(zone),
              },
            ]}
          />
        }
      />
      {commands.dialogs}
      <PageTabs
        label='DNS 区域详情'
        items={tabs}
        selected={tab}
        onSelectionChange={selectTab}
      >
        {/* The records editor's SaveBar is the tab content's last child. */}
        {tab === 'records' ? (
          <ZoneRecords />
        ) : tab === 'lines' ? (
          <ZoneLines />
        ) : (
          <ZoneOverview />
        )}
      </PageTabs>
    </Page>
  )
}
