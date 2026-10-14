import { useNavigate } from '@tanstack/react-router'
import {
  ArrowsRotateRight,
  Globe,
  PencilToSquare,
  TrashBin,
} from '@gravity-ui/icons'
import { Button, Link } from '@heroui/react'
import { useArrivalState } from '@/lib/history-state'
import { useTabSwitchGuard } from '@/lib/unsaved-changes'
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

type ZoneTab = 'overview' | 'records' | 'lines'
const tabs = [
  { id: 'overview', label: '概览' },
  { id: 'records', label: '记录' },
  { id: 'lines', label: '解析线路' },
] as const

/** `/dns-zones/$zoneId` with in-page tabs kept in component state (概览 by
 * default). Links open a tab, or the records editor, through the `dnsZone`
 * history state; switching tabs never touches the URL. */
export function ZoneDetailLayout() {
  const navigate = useNavigate()
  // The header's 编辑记录 switches tabs too, so it shares the tabs' guard.
  const tabGuard = useTabSwitchGuard()
  const [view, setView, arrivalKey] = useArrivalState((state) => {
    const requested = tabs.find(({ id }) => id === state.dnsZone?.tab)
    const tab: ZoneTab = requested?.id ?? 'overview'
    return {
      tab,
      editRecords: tab === 'records' && state.dnsZone?.editRecords === true,
    }
  })
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
  const selectTab = (tab: ZoneTab) => setView({ tab, editRecords: false })
  const setEditingRecords = (editRecords: boolean) =>
    setView({ tab: 'records', editRecords })
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
            href='/providers'
            routerOptions={{
              state: { providers: { q: zone.dns_provider_name } },
            }}
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
            <Button
              size='sm'
              onPress={() => {
                if (view.tab === 'records' && view.editRecords) return
                tabGuard.request(() => setEditingRecords(true))
              }}
            >
              <PencilToSquare className='size-4' />
              编辑记录
            </Button>
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
        guard={tabGuard}
        label='DNS 区域详情'
        items={tabs}
        selected={view.tab}
        onSelectionChange={selectTab}
      >
        {/* The records editor's SaveBar is the tab content's last child. A new
         * arrival remounts the tab, so its search and filters start over. */}
        {view.tab === 'records' ? (
          <ZoneRecords
            key={arrivalKey}
            editing={view.editRecords}
            onEditingChange={setEditingRecords}
          />
        ) : view.tab === 'lines' ? (
          <ZoneLines key={arrivalKey} />
        ) : (
          <ZoneOverview key={arrivalKey} />
        )}
      </PageTabs>
    </Page>
  )
}
