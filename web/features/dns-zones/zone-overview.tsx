import { ArrowsRotateRight } from '@gravity-ui/icons'
import { KPI } from '@heroui-pro/react/kpi'
import { Alert, Button } from '@heroui/react'
import { formatDate } from '@/lib/format'
import { describeStatus } from '@/lib/status'
import { cn } from '@/lib/utils'
import { Mono } from '@/components/mono'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList, Section } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { providerLabel } from '@/features/providers/provider-display'
import { zoneSyncStatuses, zoneSyncStatusReserve } from './dns-zone-display'
import { useZoneDetail } from './use-zone-detail'
import type { useZoneCommands } from './zone-commands'

/** The latest synchronization result and its explicit refresh command. */
export function ZoneOverview({
  commands,
}: {
  commands: ReturnType<typeof useZoneCommands>
}) {
  const zone = useZoneDetail().data
  if (!zone) return null
  const failedRecords = zone.runtime.record_states.filter(
    (state) => state.sync_status === 'failed'
  ).length
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
                  {...describeStatus(zoneSyncStatuses, zone.sync_status)}
                  reserve={zoneSyncStatusReserve}
                />
              ),
            },
            {
              label: 'DNS 账号',
              value: zone.dns_provider_name,
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
            { label: '自动检查', value: '约每 10 分钟' },
            { label: '添加时间', value: formatDate(zone.created_at) },
            { label: '域名 ID', value: <Mono>{zone.id}</Mono> },
          ]}
        />
      </Section>
    </>
  )
}
