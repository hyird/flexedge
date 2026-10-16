import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  dnsZoneLinesQuery,
  availableDnsZonesQuery,
  dnsZonesQuery,
  saveDnsRecords,
  removeDnsZone,
  syncDnsZone,
} from './data'
import type { DnsRecord } from './types'

const adapter = api.defaults.adapter
const client = new QueryClient({
  defaultOptions: { queries: { retry: false } },
})
class Source extends EventTarget {
  close() {}
}
afterEach(() => {
  api.defaults.adapter = adapter
  setLiveQueryQueryClient(client)
  client.clear()
})

/** Answers every GET with `data(url, params)` and records the request. */
function serve(
  data: (url: string, params: Record<string, unknown> | undefined) => unknown
) {
  const requests: unknown[] = []
  api.defaults.adapter = async (config) => {
    requests.push([config.url, config.params])
    return {
      config,
      status: 200,
      statusText: '',
      headers: {},
      data: {
        code: 0,
        message: 'ok',
        data: data(config.url ?? '', config.params),
      },
    }
  }
  return requests
}

test('available zones are read per provider and unwrap their list', async () => {
  const requests = serve((_, params) =>
    params?.dns_provider_id === 'provider-a'
      ? { list: [{ domain: 'a.example', status: 'active' }] }
      : { list: [] }
  )
  expect(await client.fetchQuery(availableDnsZonesQuery('provider-a'))).toEqual(
    [{ domain: 'a.example', status: 'active' }]
  )
  expect(await client.fetchQuery(availableDnsZonesQuery('provider-b'))).toEqual(
    []
  )
  expect(requests).toEqual([
    ['/dns-zones/available', { dns_provider_id: 'provider-a' }],
    ['/dns-zones/available', { dns_provider_id: 'provider-b' }],
  ])
  expect(availableDnsZonesQuery('').enabled).toBe(false)
})

test('record saves enforce provider capabilities without mutating editor values and retain revision semantics', async () => {
  const calls: unknown[] = []
  api.defaults.adapter = async (config) => {
    calls.push([
      config.method,
      config.url,
      config.data ? JSON.parse(config.data) : undefined,
      config.headers.get('If-Match'),
    ])
    return {
      config,
      status: 200,
      statusText: '',
      headers: {},
      data: { code: 0, message: 'ok' },
    }
  }
  const record: DnsRecord = {
    id: 'record',
    type: 'A',
    name: '@',
    content: '192.0.2.1',
    ttl: 600,
    proxied: true,
    line_code: 'default',
  }
  const zone = { id: 'zone', revision: 7, dns_provider: 'aliyun' }
  await saveDnsRecords(zone, [record])
  await saveDnsRecords({ ...zone, dns_provider: 'cloudflare' }, [record])
  await removeDnsZone(zone)
  await syncDnsZone({ item: zone })
  expect(record.proxied).toBe(true)
  expect(calls).toEqual([
    [
      'put',
      '/dns-zones/zone',
      { records: [{ ...record, proxied: false }] },
      '"7"',
    ],
    ['put', '/dns-zones/zone', { records: [record] }, '"7"'],
    ['delete', '/dns-zones/zone', undefined, '"7"'],
    ['post', '/dns-zones/zone/sync', undefined, undefined],
  ])
})

const line = (code: string) => ({
  code,
  name: code,
  display_name: code,
  status: 'enabled',
})

test('line queries read their zone and change cache identity when a cluster changes zones', async () => {
  const requests = serve((url) => ({
    runtime: { lines: [line(url.split('/').at(-1) ?? '')] },
  }))
  expect(await client.fetchQuery(dnsZoneLinesQuery('zone-a'))).toEqual([
    line('zone-a'),
  ])
  expect(await client.fetchQuery(dnsZoneLinesQuery('zone-b'))).toEqual([
    line('zone-b'),
  ])
  expect(requests).toEqual([
    ['/dns-zones/zone-a', undefined],
    ['/dns-zones/zone-b', undefined],
  ])
  expect(dnsZoneLinesQuery(undefined).enabled).toBe(false)
})

test('reads that break the DTO contract are rejected', async () => {
  serve(() => ({ runtime: { lines: [{ code: 'default' }] } }))
  await expect(client.fetchQuery(dnsZoneLinesQuery('zone'))).rejects.toThrow()
  serve(() => ({ list: [{ domain: 'a.example' }] }))
  await expect(
    client.fetchQuery(availableDnsZonesQuery('provider'))
  ).rejects.toThrow()
})

test('the sync filter is a server parameter: it reaches the stream URL and the cache key', async () => {
  const urls: string[] = []
  setLiveQueryQueryClient(client, undefined, (url) => {
    urls.push(url)
    return new Source() as never
  })
  const issues = dnsZonesQuery({ page: 2, page_size: 50, sync_status: 'issue' })
  const all = dnsZonesQuery({ page: 2, page_size: 50 })
  expect(issues.queryKey).not.toEqual(all.queryKey)
  const observer = new QueryObserver(client, issues)
  const stop = observer.subscribe(() => undefined)
  void observer.refetch().catch(() => undefined)
  await new Promise<void>((resolve) => queueMicrotask(resolve))
  expect(urls).toEqual([
    '/api/dns-zones/stream?page=2&page_size=50&sync_status=issue',
  ])
  stop()
  observer.destroy()
})
