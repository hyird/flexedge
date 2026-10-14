import { ApiBusinessError } from '@/lib/api-response'
import type { DnsRecord, DnsZone } from '@/features/dns-zones/types'

export function hasMeaningfulConflicts(zone: DnsZone) {
  return zone.runtime.conflicts.length > 0
}

export function displaySyncStatus(zone: DnsZone) {
  if (zone.sync_status === 'conflict') {
    return hasMeaningfulConflicts(zone) ? 'out_of_sync' : 'consistent'
  }

  return zone.sync_status
}

/** `sync_status` list filter values, sent to `/api/dns-zones/stream`.
 * `issue` (failed or conflict) matches the overview's "DNS 同步异常" count. */
export const zoneSyncFilters = [
  { id: 'issue', label: '同步异常' },
  { id: 'synced', label: '已同步' },
  { id: 'pending', label: '等待同步' },
  { id: 'failed', label: '同步失败' },
  { id: 'conflict', label: '存在冲突' },
] as const
export type ZoneSyncFilter = (typeof zoneSyncFilters)[number]['id']
export const zoneSyncFilterIds = zoneSyncFilters.map(({ id }) => id) as [
  ZoneSyncFilter,
  ...ZoneSyncFilter[],
]

export type ZoneRecordRow = DnsRecord & {
  source: 'system' | 'custom'
  state?: DnsZone['runtime']['record_states'][number]
}

/** System records first (read-only), then custom records with their sync state. */
export function zoneRecordRows(zone: DnsZone): ZoneRecordRow[] {
  const states = new Map(
    zone.runtime.record_states.map((state) => [state.id, state])
  )
  return [
    ...zone.runtime.projected_records.map((record) => ({
      ...record,
      source: 'system' as const,
    })),
    ...zone.config.records.map((record) => ({
      ...record,
      source: 'custom' as const,
      state: states.get(record.id),
    })),
  ]
}

const zoneNotFoundCode = 16201

/** `resource-error` for a deleted or unknown zone. */
export function isZoneNotFound(error: unknown) {
  return error instanceof ApiBusinessError && error.code === zoneNotFoundCode
}
