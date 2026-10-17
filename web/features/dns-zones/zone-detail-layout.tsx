import { useNavigate } from '@tanstack/react-router'
import { ArrowLeft, TrashBin } from '@gravity-ui/icons'
import { Button, Skeleton } from '@heroui/react'
import { useArrivalState } from '@/lib/history-state'
import { describeStatus } from '@/lib/status'
import { useTabSwitchGuard } from '@/lib/unsaved-changes'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { providerLabel } from '@/features/providers/provider-display'
import {
  isZoneNotFound,
  zoneSyncStatuses,
  zoneSyncStatusReserve,
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

/** `/dns-zones/$zoneId` with in-page tabs kept in component state; every
 * arrival starts on 概览 and switching tabs never touches the URL. Sync,
 * removal is beside 返回 and records are edited on 记录. */
export function ZoneDetailLayout() {
  const navigate = useNavigate()
  const commands = useZoneCommands({
    // The zone is gone, so a pending guard has nothing left to protect.
    onRemoved: () =>
      void navigate({ to: '/dns-zones', replace: true, ignoreBlocker: true }),
  })
  const tabGuard = useTabSwitchGuard()
  const [tab, setTab, arrivalKey] = useArrivalState<ZoneTab>(() => 'overview')
  // The layout's query is the zone's live detail stream; every tab reads the
  // same query key, so the page keeps exactly one connection.
  const query = useZoneDetail()
  const zone = query.data
  useBreadcrumbLabel(zone?.domain)
  if (!zone && query.isError && isZoneNotFound(query.error))
    return (
      <Page>
        <EmptyState
          title='域名不存在'
          description='该域名可能已被移除，或链接有误。'
          action={<LinkButton href='/dns-zones'>返回域名</LinkButton>}
        />
      </Page>
    )
  return (
    <Page fit={tab === 'records'}>
      <QueryNotice
        query={query}
        onRetry={() => void query.refetch()}
        title='实时连接中断，显示的是最后一次收到的数据'
      />
      {zone ? (
        <DetailHeader
          title={zone.domain}
          status={
            <StatusChip
              {...describeStatus(zoneSyncStatuses, zone.sync_status)}
              reserve={zoneSyncStatusReserve}
            />
          }
          meta={[
            <span key='provider'>
              {zone.dns_provider_name} · {providerLabel(zone.dns_provider)}
            </span>,
            <span key='synced'>
              最近同步 <RelativeTime value={zone.last_synced_at} />
            </span>,
          ]}
          actions={
            <>
              <LinkButton href='/dns-zones'>
                <ArrowLeft />
                返回
              </LinkButton>
              <Button
                size='sm'
                variant='danger-soft'
                onPress={() => commands.remove(zone)}
              >
                <TrashBin />
                移除域名
              </Button>
            </>
          }
        />
      ) : (
        <DetailHeaderSkeleton />
      )}
      <PageTabs
        fit={tab === 'records'}
        guard={tabGuard}
        label='域名详情'
        items={tabs}
        selected={tab}
        onSelectionChange={setTab}
      >
        {/* A new arrival remounts the tab, so its search, filters and open
         * record form start over. */}
        {!zone ? (
          query.isPending && (
            <div aria-label='正在加载域名内容' className='flex flex-col gap-4'>
              <Skeleton className='h-28 rounded-2xl' />
              <Skeleton className='h-64 rounded-2xl' />
            </div>
          )
        ) : tab === 'records' ? (
          <ZoneRecords key={arrivalKey} />
        ) : tab === 'lines' ? (
          <ZoneLines key={arrivalKey} />
        ) : (
          <ZoneOverview key={arrivalKey} commands={commands} />
        )}
      </PageTabs>
      {commands.dialogs}
    </Page>
  )
}
