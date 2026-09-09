import { QueryClient } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  dnsProviderOptionsQuery,
  dnsProvidersQuery,
  certificateProvidersQuery,
} from './data'

const adapter = api.defaults.adapter
const clients: QueryClient[] = []
let nextSnapshot: unknown
class Source extends EventTarget { close() {} }
function install(client: QueryClient) {
  setLiveQueryQueryClient(client, undefined, () => {
    const source = new Source()
    queueMicrotask(() => source.dispatchEvent(new MessageEvent('snapshot', {
      data: JSON.stringify({ code: 0, message: 'ok', data: nextSnapshot }),
    })))
    return source as never
  })
}
afterEach(() => {
  api.defaults.adapter = adapter
  clients.splice(0).forEach((client) => { setLiveQueryQueryClient(client); client.clear() })
})
function client() {
  const value = new QueryClient({
    defaultOptions: { queries: { retry: false } },
  })
  clients.push(value)
  return value
}
test('DNS provider options use one full collection snapshot', async () => {
  const value = Array.from({ length: 1001 }, (_, i) => ({ id: String(i) }))
  nextSnapshot = value
  const current = client(); install(current)
  const options = await current.fetchQuery(dnsProviderOptionsQuery)
  expect(options).toHaveLength(1001)
})
test('table queries retain filters and certificate queries use their shared endpoint', async () => {
  nextSnapshot = { list: [], total: 0, page: 2, page_size: 10, total_pages: 0 }
  const params = { page: 2, page_size: 10, keyword: 'cloud' }
  const current = client(); install(current)
  await current.fetchQuery(dnsProvidersQuery(params))
  nextSnapshot = []
  await current.fetchQuery(certificateProvidersQuery)
  expect(dnsProvidersQuery(params).queryKey).not.toEqual(
    dnsProviderOptionsQuery.queryKey
  )
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
