import type { SyncEvent, SyncResourceType } from '@/lib/types'
import { queryKeys } from '@/lib/query-keys'

type QueryKey = readonly string[]

export const syncEventRefreshKeys = {
  provider: [queryKeys.providers, queryKeys.dnsZones],
  dns_zone: [queryKeys.dnsZones, queryKeys.clusters, queryKeys.nodes],
  certificate: [queryKeys.certificates, queryKeys.websites],
  website: [queryKeys.websites],
} satisfies Record<SyncResourceType, readonly QueryKey[]>

const resourceLabels = {
  provider: '供应商检测',
  dns_zone: 'DNS 同步',
  certificate: '证书签发',
  website: '网站发布',
} satisfies Record<SyncResourceType, string>

export function queryKeysForSyncEvents(events: readonly SyncEvent[]): QueryKey[] {
  const keys = new Map<string, QueryKey>()
  for (const key of [queryKeys.overview] satisfies QueryKey[]) {
    keys.set(key.join('\u0000'), key)
  }
  for (const event of events) {
    for (const key of syncEventRefreshKeys[event.resource_type]) {
      keys.set(key.join('\u0000'), key)
    }
  }
  return [...keys.values()]
}

export function syncEventCompletionMessage(event: SyncEvent) {
  return `${resourceLabels[event.resource_type]}已完成，相关数据已刷新`
}
