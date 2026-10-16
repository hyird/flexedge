import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import { getData, pageDataSchema, sendData, streamPath } from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import { resourceListSearchQuery } from '@/lib/resource'
import type { ZoneSyncFilter } from './dns-zone-display'
import {
  availableDnsZonesSchema,
  dnsLineSchema,
  dnsZoneOptionsSchema,
  dnsZoneSchema,
  type DnsRecord,
  type DnsZone,
} from './types'

const dnsZonePageSchema = pageDataSchema(dnsZoneSchema)
/** The part of `/dns-zones/:id` a line picker reads. */
const dnsZoneLinesSchema = z.object({
  runtime: z.object({ lines: z.array(dnsLineSchema) }),
})

export const dnsZoneOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.dnsZones, 'options'],
  queryFn: async ({ signal }) =>
    parseResponseData(
      dnsZoneOptionsSchema,
      await getData<unknown>('/dns-zones/collection', undefined, signal)
    ),
})

export function dnsZonesQuery(params: {
  page: number
  page_size: number
  keyword?: string
  dns_provider_id?: string
  /** `issue` = failed (overview `dns_zone_issue_count`). */
  sync_status?: ZoneSyncFilter
}) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.dnsZones, 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/dns-zones', params),
        queryKey,
        signal,
        (value) => parseResponseData(dnsZonePageSchema, value)
      ),
  })
}

/** Command palette search of DNS zones. */
export function dnsZoneSearchQuery(keyword: string) {
  return resourceListSearchQuery(
    '/dns-zones',
    queryKeys.dnsZones,
    keyword,
    dnsZoneSchema.pick({ id: true, domain: true }),
    (zone) => ({ id: zone.id, label: zone.domain })
  )
}

/** Live zone detail (`/dns-zones/:id/stream`): one stream shared by the
 * detail layout and its tabs. */
export function dnsZoneDetailQuery(zoneId: string) {
  return queryOptions({
    retry: false,
    // The detail layout renders not-found and stream errors itself.
    meta: { inlineError: true },
    queryKey: [...queryKeys.dnsZones, 'detail', zoneId],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/dns-zones/' + zoneId),
        queryKey,
        signal,
        (value) => parseResponseData(dnsZoneSchema, value)
      ),
  })
}

export function availableDnsZonesQuery(providerId: string) {
  return queryOptions({
    retry: false,
    queryKey: [...queryKeys.dnsZones, 'available', providerId],
    queryFn: async ({ signal }) =>
      parseResponseData(
        availableDnsZonesSchema,
        await getData<unknown>(
          '/dns-zones/available',
          { dns_provider_id: providerId },
          signal
        )
      ).list,
    enabled: !!providerId,
  })
}

/** After any zone write (create, record save, sync, removal): live streams
 * push the new state, so one-shot snapshots (options, lines, available
 * zones) are only marked stale. */
export function afterDnsZoneWrite(client: QueryClient) {
  return client.invalidateQueries({
    queryKey: queryKeys.dnsZones,
    refetchType: 'none',
  })
}

export function createDnsZone(values: {
  dns_provider_id: string
  domain: string
}) {
  return sendData('post', '/dns-zones', values)
}

export function syncDnsZone({ item }: { item: Pick<DnsZone, 'id'> }) {
  return sendData('post', '/dns-zones/' + item.id + '/sync', undefined)
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
    queryFn: async ({ signal }) =>
      parseResponseData(
        dnsZoneLinesSchema,
        await getData<unknown>('/dns-zones/' + zoneId, undefined, signal)
      ).runtime.lines,
    enabled: !!zoneId,
  })
}
