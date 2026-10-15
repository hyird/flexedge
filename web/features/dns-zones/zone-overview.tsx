import { useNavigate } from '@tanstack/react-router'
import { ArrowsRotateRight, TrashBin } from '@gravity-ui/icons'
import type { DataGridColumn } from '@heroui-pro/react'
import { KPI } from '@heroui-pro/react/kpi'
import { Alert, Button, Link } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { cn } from '@/lib/utils'
import { DataTable } from '@/components/data-table'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { providerLabel } from '@/features/providers/provider-display'
import { dnsLinePath } from './dns-lines'
import { displaySyncStatus, zoneSyncStatusLabels } from './dns-zone-display'
import type { DnsZone } from './types'
import { useZoneDetail } from './use-zone-detail'
import { useZoneCommands } from './zone-commands'

type Conflict = DnsZone['runtime']['conflicts'][number]

/** The 概览 tab: sync state, conflicts (the only place to resolve them),
 * 立即同步 next to the sync details and 移除域名 at the bottom of them. */
export function ZoneOverview() {
  const zone = useZoneDetail().data
  const navigate = useNavigate()
  const commands = useZoneCommands({
    // The zone is gone, so a pending guard has nothing left to protect.
    onRemoved: () =>
      void navigate({ to: '/dns-zones', replace: true, ignoreBlocker: true }),
  })
  if (!zone) return null
  const failedRecords = zone.runtime.record_states.filter(
    (state) => state.sync_status === 'failed'
  ).length
  const conflictColumns: DataGridColumn<Conflict>[] = [
    { id: 'type', header: '类型', accessorKey: 'type', width: 80 },
    {
      id: 'name',
      header: '主机记录',
      isRowHeader: true,
      minWidth: 120,
      cell: (item) => <Mono>{item.name}</Mono>,
    },
    {
      id: 'line',
      header: '线路',
      minWidth: 110,
      cell: (item) => dnsLinePath(zone.runtime.lines, item.line_code),
    },
    {
      id: 'local',
      header: '本地值',
      minWidth: 180,
      cell: (item) => (
        <Mono truncate className='max-w-80'>
          {item.local_content}
        </Mono>
      ),
    },
    {
      id: 'remote',
      header: '服务商值',
      minWidth: 180,
      cell: (item) => (
        <Mono truncate className='max-w-80'>
          {item.remote_content}
        </Mono>
      ),
    },
  ]
  return (
    <>
      {zone.last_error && (
        <Alert status='danger'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>最近一次同步失败</Alert.Title>
            <Alert.Description>{zone.last_error}</Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      <div className='grid grid-cols-2 gap-3 md:grid-cols-4'>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>自定义记录</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value value={zone.config.records.length} />
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>系统自动记录</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value value={zone.runtime.projected_records.length} />
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>同步失败记录</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value
              value={failedRecords}
              className={failedRecords ? 'text-danger' : undefined}
            />
          </KPI.Content>
        </KPI>
        <KPI className='min-w-0'>
          <KPI.Header>
            <KPI.Title>关联网站</KPI.Title>
          </KPI.Header>
          <KPI.Content>
            <KPI.Value value={zone.website_count} />
          </KPI.Content>
        </KPI>
      </div>
      {zone.runtime.conflicts.length > 0 && (
        <Section
          title={`同步冲突 · ${zone.runtime.conflicts.length}`}
          description='以下记录在本地与服务商中的值不一致，选择以哪一方为准后会立即同步。'
          actions={
            <>
              <Button
                size='sm'
                variant='tertiary'
                onPress={() => commands.resolve(zone, 'remote')}
              >
                使用服务商记录
              </Button>
              <Button
                size='sm'
                variant='tertiary'
                onPress={() => commands.resolve(zone, 'local')}
              >
                使用本地记录
              </Button>
            </>
          }
        >
          <DataTable
            label='同步冲突'
            columns={conflictColumns}
            data={zone.runtime.conflicts}
            isLoading={false}
            minWidth={680}
            empty={{ title: '没有冲突' }}
          />
        </Section>
      )}
      <Section
        title='同步信息'
        contentClassName='flex flex-col gap-4'
        actions={
          <Button
            size='sm'
            variant='tertiary'
            isPending={commands.syncingId === zone.id}
            onPress={() => commands.sync(zone)}
          >
            <ArrowsRotateRight className='size-4' />
            立即同步
          </Button>
        }
      >
        <DescriptionList
          columns={3}
          items={[
            {
              label: '同步状态',
              value: (
                <StatusChip
                  status={displaySyncStatus(zone)}
                  reserve={zoneSyncStatusLabels}
                />
              ),
            },
            {
              label: 'DNS 账号',
              value: (
                <Link
                  href='/providers'
                  routerOptions={{
                    state: { providers: { q: zone.dns_provider_name } },
                  }}
                >
                  {zone.dns_provider_name}
                </Link>
              ),
            },
            { label: '平台', value: providerLabel(zone.dns_provider) },
            {
              label: '最近同步',
              value: (
                // Date and relative time always occupy the same slots.
                <span className='flex gap-x-2 whitespace-nowrap'>
                  {formatDate(zone.last_synced_at)}
                  <RelativeTime
                    value={zone.last_synced_at}
                    className={cn(
                      'text-muted',
                      !zone.last_synced_at && 'invisible'
                    )}
                  />
                </span>
              ),
            },
            {
              label: '解析线路更新',
              value: formatDate(zone.runtime.lines_synced_at),
            },
            {
              label: '已有记录导入',
              value: zone.runtime.records_imported ? '已导入' : '尚未导入',
            },
            { label: '添加时间', value: formatDate(zone.created_at) },
            { label: '域名 ID', value: <Mono>{zone.id}</Mono> },
          ]}
        />
        <Button
          size='sm'
          variant='danger-soft'
          className='self-start'
          onPress={() => commands.remove(zone)}
        >
          <TrashBin className='size-4' />
          移除域名
        </Button>
      </Section>
      {commands.dialogs}
    </>
  )
}
