import type { QueryClient } from '@tanstack/react-query'
import { queryKeys } from '@/lib/query-keys'
import type { SyncEvent, SyncResourceType } from '@/lib/types'

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
  website: '网站域名检查',
} satisfies Record<SyncResourceType, string>

export function queryKeysForSyncEvents(
  events: readonly SyncEvent[]
): QueryKey[] {
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

export function syncEventFailureMessage(event: SyncEvent) {
  return `${resourceLabels[event.resource_type]}失败，请查看资源详情`
}

export async function refreshSyncEventQueries(
  client: QueryClient,
  events: readonly SyncEvent[]
) {
  await Promise.all(
    queryKeysForSyncEvents(events).map((queryKey) =>
      client.invalidateQueries({ queryKey }, { throwOnError: true })
    )
  )
}
