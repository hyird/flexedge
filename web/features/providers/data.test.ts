import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  dnsProviderOptionsQuery,
  dnsProvidersQuery,
  certificateProviderOptionsQuery,
  certificateProvidersQuery,
} from './data'

const adapter = api.defaults.adapter
const clients: QueryClient[] = []
let nextSnapshot: unknown
class Source extends EventTarget {
  closed = false
  close() {
    this.closed = true
  }
}
function install(client: QueryClient, urls: string[] = []) {
  setLiveQueryQueryClient(client, undefined, (url) => {
    urls.push(url)
    const source = new Source()
    queueMicrotask(() =>
      source.dispatchEvent(
        new MessageEvent('snapshot', {
          data: JSON.stringify({ code: 0, message: 'ok', data: nextSnapshot }),
        })
      )
    )
    return source as never
  })
}
afterEach(() => {
  api.defaults.adapter = adapter
  clients.splice(0).forEach((client) => {
    setLiveQueryQueryClient(client)
    client.clear()
  })
})
function client() {
  const value = new QueryClient({
    defaultOptions: { queries: { retry: false } },
  })
  clients.push(value)
  return value
}
const timestamps = {
  created_at: '2026-01-01T00:00:00.000000+00:00',
  updated_at: '2026-01-01T00:00:00.000000+00:00',
}
function dnsProvider(id: string) {
  return {
    id,
    revision: 1,
    name: `DNS ${id}`,
    account_id: '0123456789abcdef0123456789abcdef',
    provider: 'cloudflare',
    token_hint: '****abcd',
    status: 'unverified',
    zone_count: 0,
    ...timestamps,
  }
}
function certificateProvider(
  status: string,
  provider: 'letsencrypt' | 'zerossl' = 'letsencrypt'
) {
  return {
    id: provider,
    revision: 1,
    provider,
    credential_mode: 'email',
    account_email: 'ops@example.com',
    status,
    ...timestamps,
  }
}
test('DNS provider options read one full collection', async () => {
  const value = Array.from({ length: 1001 }, (_, i) => dnsProvider(String(i)))
  const urls: unknown[] = []
  api.defaults.adapter = async (config) => {
    urls.push(config.url)
    return {
      config,
      status: 200,
      statusText: '',
      headers: {},
      data: { code: 0, message: 'ok', data: value },
    }
  }
  const options = await client().fetchQuery(dnsProviderOptionsQuery)
  expect(options).toHaveLength(1001)
  expect(urls).toEqual(['/providers/dns/options'])
})
test('certificate options remain a full collection separate from table pages', async () => {
  const value = [
    certificateProvider('unverified'),
    certificateProvider('verified', 'zerossl'),
  ]
  const urls: unknown[] = []
  api.defaults.adapter = async (config) => {
    urls.push({ url: config.url, params: config.params })
    return {
      config,
      status: 200,
      statusText: '',
      headers: {},
      data: { code: 0, message: 'ok', data: value },
    }
  }
  const current = client()
  nextSnapshot = {
    list: [value[0]],
    total: 2,
    page: 1,
    page_size: 1,
    total_pages: 2,
  }
  install(current)
  expect(
    await current.fetchQuery(
      certificateProvidersQuery({
        page: 1,
        page_size: 1,
      })
    )
  ).toMatchObject({ list: [value[0]], total: 2, total_pages: 2 })
  expect(await current.fetchQuery(certificateProviderOptionsQuery)).toEqual(
    value
  )
  expect(urls).toEqual([
    { url: '/providers/certificate/options', params: undefined },
  ])
})
test('table queries retain filters and use keys distinct from picker reads', async () => {
  nextSnapshot = { list: [], total: 0, page: 2, page_size: 10, total_pages: 0 }
  const params = { page: 2, page_size: 10, keyword: 'cloud' }
  const current = client()
  const urls: string[] = []
  install(current, urls)
  await current.fetchQuery(dnsProvidersQuery(params))
  await current.fetchQuery(certificateProvidersQuery(params))
  expect(urls).toEqual([
    '/api/providers/dns/stream?keyword=cloud&page=2&page_size=10',
    '/api/providers/certificate/stream?keyword=cloud&page=2&page_size=10',
  ])
  expect(dnsProvidersQuery(params).queryKey).not.toEqual(
    dnsProviderOptionsQuery.queryKey
  )
  expect(certificateProvidersQuery(params).queryKey).toEqual([
    'providers',
    'certificate',
    'list',
    params,
  ])
  for (const changed of [
    { ...params, page: 3 },
    { ...params, page_size: 20 },
    { ...params, keyword: 'ZeroSSL' },
  ]) {
    expect(certificateProvidersQuery(changed).queryKey).not.toEqual(
      certificateProvidersQuery(params).queryKey
    )
  }
  expect(certificateProviderOptionsQuery.queryKey).toEqual([
    'providers',
    'certificate',
    'options',
  ])
})

test('certificate table rejects the obsolete array snapshot', async () => {
  const current = client()
  nextSnapshot = [certificateProvider('unverified')]
  install(current)
  await expect(
    current.fetchQuery(
      certificateProvidersQuery({
        page: 1,
        page_size: 20,
      })
    )
  ).rejects.toThrow()
})

test('certificate provider tables receive background verification while observed', async () => {
  const current = client()
  const query = certificateProvidersQuery({ page: 1, page_size: 10 })
  const snapshot = (status: string) => ({
    list: [certificateProvider(status)],
    total: 1,
    page: 1,
    page_size: 10,
    total_pages: 1,
  })
  const sources = new Map<string, Source>()
  setLiveQueryQueryClient(current, undefined, (url) => {
    const source = new Source()
    sources.set(url, source)
    queueMicrotask(() =>
      source.dispatchEvent(
        new MessageEvent('snapshot', {
          data: JSON.stringify({
            code: 0,
            message: 'ok',
            data: snapshot('unverified'),
          }),
        })
      )
    )
    return source as never
  })
  const observer = new QueryObserver(current, query)
  const unsubscribe = observer.subscribe(() => undefined)
  try {
    await expect(observer.refetch()).resolves.toMatchObject({
      data: snapshot('unverified'),
    })
    const listSource = sources.get(
      '/api/providers/certificate/stream?page=1&page_size=10'
    )!
    expect(listSource.closed).toBe(false)
    listSource.dispatchEvent(
      new MessageEvent('snapshot', {
        data: JSON.stringify({
          code: 0,
          message: 'ok',
          data: snapshot('verified'),
        }),
      })
    )
    expect(current.getQueryData(query.queryKey)).toEqual(snapshot('verified'))
    unsubscribe()
    await new Promise<void>((resolve) => queueMicrotask(resolve))
    expect(listSource.closed).toBe(true)
  } finally {
    unsubscribe()
    observer.destroy()
  }
})

test('provider writes own immutable fields, credential mode and revision headers', async () => {
  const {
    saveDnsProvider,
    saveCertificateProvider,
    verifyProvider,
    removeProvider,
  } = await import('./data')
  const calls: unknown[] = []
  api.defaults.adapter = async (config) => {
    calls.push({
      method: config.method,
      url: config.url,
      revision: config.headers.get('If-Match'),
      body: config.data ? JSON.parse(config.data) : undefined,
    })
    return {
      config,
      status: 200,
      statusText: '',
      headers: {},
      data: { code: 0, message: 'ok' },
    }
  }
  const dns = {
    name: 'DNS',
    provider: 'aliyun' as const,
    account_id: 'account-id',
    api_token: '',
  }
  await saveDnsProvider(dns)
  await saveDnsProvider(dns, { id: 'dns-id', revision: 7 })
  await saveCertificateProvider(
    {
      provider: 'zerossl',
      credential_mode: 'email',
      account_email: 'test@example.com',
      access_key: 'unused',
    },
    { id: 'cert-id', revision: 8 }
  )
  for (const kind of ['dns', 'certificate'] as const) {
    await verifyProvider({ kind, id: 'id', revision: 9 })
    await removeProvider({ kind, id: 'id', revision: 10 })
  }
  expect(calls).toEqual([
    { method: 'post', url: '/providers/dns', revision: undefined, body: dns },
    {
      method: 'put',
      url: '/providers/dns/dns-id',
      revision: '"7"',
      body: { name: 'DNS' },
    },
    {
      method: 'put',
      url: '/providers/certificate/cert-id',
      revision: '"8"',
      body: {
        credential_mode: 'email',
        account_email: 'test@example.com',
      },
    },
    ...['dns', 'certificate'].flatMap((kind) => [
      {
        method: 'post',
        url: `/providers/${kind}/id/verify`,
        revision: '"9"',
        body: undefined,
      },
      {
        method: 'delete',
        url: `/providers/${kind}/id`,
        revision: '"10"',
        body: undefined,
      },
    ]),
  ])
})

test('certificate key edits omit inactive credentials and omit a retained key', async () => {
  const { saveCertificateProvider } = await import('./data')
  const bodies: unknown[] = []
  api.defaults.adapter = async (config) => {
    bodies.push(JSON.parse(config.data))
    return {
      config,
      status: 200,
      statusText: '',
      headers: {},
      data: { code: 0, message: 'ok' },
    }
  }
  const values = {
    provider: 'zerossl' as const,
    credential_mode: 'access_key' as const,
    account_email: 'unused@example.com',
    access_key: '',
  }
  await saveCertificateProvider(values, { id: 'id', revision: 1 })
  await saveCertificateProvider(
    { ...values, access_key: 'replacement' },
    { id: 'id', revision: 1 }
  )
  expect(bodies).toEqual([
    { credential_mode: 'access_key' },
    { credential_mode: 'access_key', access_key: 'replacement' },
  ])
})
