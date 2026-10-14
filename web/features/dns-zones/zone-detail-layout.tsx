import { Outlet, useNavigate } from '@tanstack/react-router'
import {
  ArrowsRotateRight,
  Globe,
  PencilToSquare,
  TrashBin,
} from '@gravity-ui/icons'
import { Button, Dropdown, Label, Link } from '@heroui/react'
import { useBreadcrumbLabel } from '@/components/layout/breadcrumb-trail'
import { ActionMenu } from '@/components/action-menu'
import { DetailHeader, DetailHeaderSkeleton } from '@/components/detail-header'
import { LinkButton } from '@/components/link-button'
import { EmptyState, Page, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { RouteTabs } from '@/components/route-tabs'
import { StatusChip } from '@/components/status-chip'
import { providerLabel } from '@/features/providers/provider-display'
import {
  displaySyncStatus,
  hasMeaningfulConflicts,
  isZoneNotFound,
} from './dns-zone-display'
import { useZoneDetail } from './use-zone-detail'
import { useZoneCommands } from './zone-commands'

export function ZoneDetailLayout() {
  const navigate = useNavigate()
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
  const base = `/dns-zones/${zone.id}`
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
        status={<StatusChip status={displaySyncStatus(zone)} />}
        meta={[
          <Link
            key='provider'
            href={`/providers/dns?q=${encodeURIComponent(zone.dns_provider_name)}`}
          >
            {zone.dns_provider_name} · {providerLabel(zone.dns_provider)}
          </Link>,
          <span key='revision' className='tabular-nums'>
            同步版本 {zone.synced_revision}/{zone.desired_revision}
          </span>,
          <span key='synced'>
            最近同步 <RelativeTime value={zone.last_synced_at} />
          </span>,
        ]}
        actions={
          <>
            {hasMeaningfulConflicts(zone) && (
              <Dropdown>
                <Button size='sm' variant='danger-soft'>
                  处理冲突（{zone.runtime.conflicts.length}）
                </Button>
                <Dropdown.Popover placement='bottom end' className='min-w-56'>
                  <Dropdown.Menu
                    aria-label='冲突处理方式'
                    onAction={(key) =>
                      commands.resolve(
                        zone,
                        key === 'remote' ? 'remote' : 'local'
                      )
                    }
                  >
                    <Dropdown.Item id='local' textValue='使用本地记录'>
                      <Label>使用本地记录覆盖服务商</Label>
                    </Dropdown.Item>
                    <Dropdown.Item id='remote' textValue='使用远端记录'>
                      <Label>使用服务商记录替换本地</Label>
                    </Dropdown.Item>
                  </Dropdown.Menu>
                </Dropdown.Popover>
              </Dropdown>
            )}
            <Button
              size='sm'
              variant='tertiary'
              isPending={commands.syncingId === zone.id}
              onPress={() => commands.sync(zone)}
            >
              <ArrowsRotateRight className='size-4' />
              立即同步
            </Button>
            <LinkButton href={`${base}/records?edit=1`}>
              <PencilToSquare />
              编辑记录
            </LinkButton>
          </>
        }
        menu={
          <ActionMenu
            label={`DNS 区域 ${zone.domain} 的更多操作`}
            items={[
              {
                id: 'remove',
                label: '移除区域',
                icon: <TrashBin />,
                danger: true,
                onAction: () => commands.remove(zone),
              },
            ]}
          />
        }
      />
      <RouteTabs
        label='DNS 区域详情'
        items={[
          { href: base, label: '概览' },
          { href: `${base}/records`, label: '记录' },
          { href: `${base}/lines`, label: '解析线路' },
        ]}
      />
      {commands.dialogs}
      {/* Last child: the records editor's SaveBar must end the Page. */}
      <Outlet />
    </Page>
  )
}
