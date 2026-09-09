import type { DnsZone } from '@/features/dns-zones/types'

export function hasMeaningfulConflicts(zone: DnsZone) {
  return zone.runtime.conflicts.length > 0
}

export function displaySyncStatus(zone: DnsZone) {
  if (zone.sync_status === 'conflict') {
    return hasMeaningfulConflicts(zone) ? 'out_of_sync' : 'consistent'
  }

  return zone.sync_status
}
