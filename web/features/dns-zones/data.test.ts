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
class Source extends EventTarget { close() {} }
afterEach(() => {
  api.defaults.adapter = adapter
  setLiveQueryQueryClient(client)
  client.clear()
})

test('zone snapshots isolate provider selections and transform available data', async () => {
  const sources = new Map<string, Source>()
  setLiveQueryQueryClient(client, undefined, (url) => {
    const source = new Source()
    sources.set(url, source)
    return source as never
  })
  const read = async <T,>(query: { queryKey: readonly unknown[]; queryFn?: unknown }, data: unknown) => {
    const observer = new QueryObserver<T>(client, query as never)
    const stop = observer.subscribe(() => undefined)
    const pending = observer.refetch()
    await new Promise<void>((resolve) => queueMicrotask(resolve))
    const source = [...sources.values()].at(-1)!
    source.dispatchEvent(new MessageEvent('snapshot', {
      data: JSON.stringify({ code: 0, message: 'ok', data }),
    }))
    const result = await pending
    stop()
    observer.destroy()
    return result.data
  }
  expect(await read(availableDnsZonesQuery('provider-a'), { list: [{ domain: 'a.example', status: 'active' }] })).toEqual([{ domain: 'a.example', status: 'active' }])
  expect(await read(availableDnsZonesQuery('provider-b'), { list: [] })).toEqual([])
  expect(await read(dnsZonesQuery({ page: 2, page_size: 10, dns_provider_id: 'provider-a' }), { list: [], total: 0, page: 2, page_size: 10, total_pages: 0 })).toMatchObject({ page: 2 })
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
  await syncDnsZone({ item: zone, policy: 'remote' })
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
    ['post', '/dns-zones/zone/sync', { conflict_policy: 'remote' }, undefined],
  ])
})

test('line queries share a zone read and change cache identity when a cluster changes zones', async () => {
  const sources = new Map<string, Source>()
  setLiveQueryQueryClient(client, undefined, (url) => {
    const source = new Source()
    sources.set(url, source)
    return source as never
  })
  const readLines = async (zone: string) => {
    const observer = new QueryObserver(client, dnsZoneLinesQuery(zone))
    const stop = observer.subscribe(() => undefined)
    const pending = observer.refetch()
    await new Promise<void>((resolve) => queueMicrotask(resolve))
    sources.get(`/api/dns-zones/${zone}/stream`)?.dispatchEvent(new MessageEvent('snapshot', {
      data: JSON.stringify({ code: 0, message: 'ok', data: { runtime: { lines: [{ code: zone }] } } }),
    }))
    const result = await pending
    stop()
    observer.destroy()
    return result.data
  }
  expect(await readLines('zone-a')).toEqual([{ code: 'zone-a' }])
  expect(await readLines('zone-b')).toEqual([{ code: 'zone-b' }])
  expect(dnsZoneLinesQuery(undefined).enabled).toBe(false)
})
