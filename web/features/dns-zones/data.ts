import { queryOptions } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import type { DnsZoneOption, DnsZone, DnsRecord } from './types'

export const dnsZoneOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.dnsZones, 'options'],
  queryFn: ({ queryKey, signal }) => readLiveQuery<DnsZoneOption[]>(streamPath('/dns-zones/collection'), queryKey, signal),
})

export function dnsZonesQuery(params: {
  page: number
  page_size: number
  keyword?: string
  dns_provider_id?: string
}) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.dnsZones, 'list', params],
    queryFn: ({ queryKey, signal }) => readLiveQuery<PageData<DnsZone>>(streamPath('/dns-zones', params), queryKey, signal),
  })
}

export function availableDnsZonesQuery(providerId: string) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.dnsZones, 'available', providerId],
    queryFn: ({ queryKey, signal }) => readLiveQuery(
      streamPath('/dns-zones/available', { dns_provider_id: providerId }), queryKey, signal,
      (data) => (data as { list: Array<{ domain: string; status: string }> }).list),
    enabled: !!providerId,
  })
}

export function createDnsZone(values: {
  dns_provider_id: string
  domain: string
}) {
  return sendData('post', '/dns-zones', values)
}

export function syncDnsZone({
  item,
  policy,
}: {
  item: Pick<DnsZone, 'id'>
  policy?: 'local' | 'remote'
}) {
  return sendData(
    'post',
    '/dns-zones/' + item.id + '/sync',
    policy ? { conflict_policy: policy } : undefined
  )
}

export function removeDnsZone(zone: Pick<DnsZone, 'id' | 'revision'>) {
  return sendData('delete', '/dns-zones/' + zone.id, undefined, zone.revision)
}

export function saveDnsRecords(
  zone: Pick<DnsZone, 'id' | 'revision' | 'dns_provider'>,
  records: DnsRecord[]
) {
  return sendData(
    'put',
    '/dns-zones/' + zone.id,
    {
      records:
        zone.dns_provider === 'cloudflare'
          ? records
          : records.map((record) => ({ ...record, proxied: false })),
    },
    zone.revision
  )
}

export function dnsZoneLinesQuery(zoneId: string | undefined) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.dnsZones, 'lines', zoneId],
    queryFn: ({ queryKey, signal }) => readLiveQuery(streamPath('/dns-zones/' + zoneId), queryKey, signal,
      (zone) => (zone as DnsZone).runtime.lines),
    enabled: !!zoneId,
  })
}
