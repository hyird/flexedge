import { ArrowLeft, Globe } from '@gravity-ui/icons'
import { Link } from '@heroui/react'
import { useArrivalState } from '@/lib/history-state'
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
  displaySyncStatus,
  isZoneNotFound,
  zoneSyncStatusLabels,
} from './dns-zone-display'
import { useZoneDetail } from './use-zone-detail'
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
 * conflicts and removal live on 概览; records are edited on 记录. */
export function ZoneDetailLayout() {
  const tabGuard = useTabSwitchGuard()
  const [tab, setTab, arrivalKey] = useArrivalState<ZoneTab>(() => 'overview')
  // The layout's query is the zone's live detail stream; every tab reads the
  // same query key, so the page keeps exactly one connection.
  const query = useZoneDetail()
  const zone = query.data
  useBreadcrumbLabel(zone?.domain)
  if (!zone) {
    if (query.isError && isZoneNotFound(query.error))
      return (
        <Page>
          <EmptyState
            title='域名不存在'
            description='该域名可能已被移除，或链接有误。'
            action={<LinkButton href='/dns-zones'>返回域名管理</LinkButton>}
          />
        </Page>
      )
    return (
      <Page>
        <QueryNotice
          query={query}
          onRetry={() => void query.refetch()}
          title='域名加载失败'
        />
        {query.isPending && <DetailHeaderSkeleton />}
      </Page>
    )
  }
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
          <LinkButton href='/dns-zones' variant='tertiary'>
            <ArrowLeft />
            返回
          </LinkButton>
        }
      />
      <PageTabs
        guard={tabGuard}
        label='域名详情'
        items={tabs}
        selected={tab}
        onSelectionChange={setTab}
      >
        {/* A new arrival remounts the tab, so its search, filters and open
         * record form start over. */}
        {tab === 'records' ? (
          <ZoneRecords key={arrivalKey} />
        ) : tab === 'lines' ? (
          <ZoneLines key={arrivalKey} />
        ) : (
          <ZoneOverview key={arrivalKey} />
        )}
      </PageTabs>
    </Page>
  )
}
