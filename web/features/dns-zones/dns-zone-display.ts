import { ApiBusinessError } from '@/lib/api-response'
import {
  enablementStatuses,
  statusIds,
  statusOptions,
  statusReserve,
  type StatusDictionary,
} from '@/lib/status'
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

/** Zone `sync_status` (wire: pending/synced/failed/conflict; mock and
 * transient states: queued/syncing) and the `out_of_sync`/`consistent`
 * states `displaySyncStatus` derives from a conflict. */
export const zoneSyncStatuses = {
  synced: { label: '已同步', tone: 'success' },
  pending: { label: '等待同步', tone: 'warning' },
  queued: { label: '排队中', tone: 'accent' },
  syncing: { label: '同步中', tone: 'accent' },
  failed: { label: '同步失败', tone: 'danger' },
  conflict: { label: '存在冲突', tone: 'danger' },
  out_of_sync: { label: '未同步', tone: 'warning' },
  consistent: { label: '一致', tone: 'success' },
} as const satisfies StatusDictionary
export const zoneSyncStatusReserve = statusReserve(zoneSyncStatuses)

/** A record's `record_states[].sync_status`; same wording as the zone. */
export const recordSyncStatuses = {
  synced: zoneSyncStatuses.synced,
  pending: zoneSyncStatuses.pending,
  syncing: zoneSyncStatuses.syncing,
  failed: zoneSyncStatuses.failed,
  conflict: zoneSyncStatuses.conflict,
} as const satisfies StatusDictionary
export const recordSyncStatusReserve = statusReserve(recordSyncStatuses)

/** A provider line's `status`. */
export const lineStatuses = enablementStatuses
export const lineStatusReserve = statusReserve(lineStatuses)

/** `sync_status` list filter values, sent to `/api/dns-zones/stream`.
 * `issue` (failed or conflict) matches the overview's "DNS 同步异常" count;
 * the others reuse the chip wording. */
const zoneSyncFilterStatuses = {
  issue: { label: '同步异常', tone: 'danger' },
  synced: zoneSyncStatuses.synced,
  pending: zoneSyncStatuses.pending,
  failed: zoneSyncStatuses.failed,
  conflict: zoneSyncStatuses.conflict,
} as const satisfies StatusDictionary
export type ZoneSyncFilter = keyof typeof zoneSyncFilterStatuses
export const zoneSyncFilters = statusOptions(zoneSyncFilterStatuses)
export const zoneSyncFilterIds = statusIds(zoneSyncFilterStatuses)

/** A DataGrid row carrying the cached record and its current sync state. */
export type ZoneRecordRow = {
  record: DnsRecord
  source: 'system' | 'custom'
  state?: DnsZone['runtime']['record_states'][number]
}

/** System records first (read-only), then custom records; both carry their
 * `record_states` entry (a website CNAME the provider rejected shows failed). */
export function zoneRecordRows(zone: {
  config: Pick<DnsZone['config'], 'records'>
  runtime: Pick<DnsZone['runtime'], 'projected_records' | 'record_states'>
}): ZoneRecordRow[] {
  const states = new Map(
    zone.runtime.record_states.map((state) => [state.id, state])
  )
  return [
    ...zone.runtime.projected_records.map((record) => ({
      record,
      source: 'system' as const,
      state: states.get(record.id),
    })),
    ...zone.config.records.map((record) => ({
      record,
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
