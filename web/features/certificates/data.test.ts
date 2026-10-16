import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  createCertificate,
  updateCertificateRenewal,
  renewCertificate,
  removeCertificate,
  downloadCertificate,
  certificatesQuery,
} from './data'

class Source extends EventTarget {
  closed = false
  close() {
    this.closed = true
  }
}

function certificate(id: string) {
  return {
    id,
    revision: 1,
    created_at: '2026-10-01T00:00:00.000000+00',
    updated_at: '2026-10-01T00:00:00.000000+00',
    domains: ['example.com'],
    issuer: "Let's Encrypt",
    certificate_provider_id: 'provider',
    certificate_provider: 'letsencrypt',
    status: 'pending',
    usable: false,
    config: { auto_renew: true },
    dns_zone_id: 'zone',
    dns_zone_domain: 'example.com',
    website_count: 0,
  }
}

const client = new QueryClient({
  defaultOptions: { queries: { retry: false } },
})

const adapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = adapter
  setLiveQueryQueryClient(client)
  client.clear()
})

test('certificate lists stay live after the first snapshot', async () => {
  const listSource = new Source()
  setLiveQueryQueryClient(client, undefined, () => listSource as never)
  const query = new QueryObserver(
    client,
    certificatesQuery({ page: 2, page_size: 10, keyword: 'example' })
  )
  const unsubscribe = query.subscribe(() => undefined)
  const listPending = query.refetch()
  listSource.dispatchEvent(
    new MessageEvent('snapshot', {
      data: JSON.stringify({
        code: 0,
        message: 'ok',
        data: {
          list: [certificate('certificate')],
          total: 1,
          page: 2,
          page_size: 10,
          total_pages: 1,
        },
      }),
    })
  )
  await expect(listPending).resolves.toMatchObject({
    data: { list: [{ id: 'certificate' }] },
  })
  expect(listSource.closed).toBe(false)
  listSource.dispatchEvent(
    new MessageEvent('snapshot', {
      data: JSON.stringify({
        code: 0,
        message: 'ok',
        data: {
          list: [certificate('replacement')],
          total: 1,
          page: 2,
          page_size: 10,
          total_pages: 1,
        },
      }),
    })
  )
  await new Promise<void>((resolve) => queueMicrotask(resolve))
  expect(
    client.getQueryData(
      certificatesQuery({ page: 2, page_size: 10, keyword: 'example' }).queryKey
    )
  ).toMatchObject({ list: [{ id: 'replacement' }] })
  unsubscribe()
  query.destroy()
})

test('certificate commands preserve creation and revision contracts', async () => {
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
  const input = {
    domain: '*.example.com',
    certificate_provider_id: 'provider',
    dns_zone_id: 'zone',
    config: { auto_renew: true },
  }
  const target = { id: 'certificate', revision: 5 }
  await createCertificate(input)
  await updateCertificateRenewal(target, false)
  await renewCertificate(target)
  await removeCertificate(target)
  expect(calls).toEqual([
    ['post', '/certificates', input, undefined],
    ['put', '/certificates/certificate', { auto_renew: false }, '"5"'],
    ['post', '/certificates/certificate/renew', undefined, '"5"'],
    ['delete', '/certificates/certificate', undefined, '"5"'],
  ])
})

test('certificate archive download preserves binary data and server filename', async () => {
  const blob = new Blob(['test archive'])
  let disposition: string | undefined = 'attachment; filename="issued.zip"'
  api.defaults.adapter = async (config) => {
    expect(config.responseType).toBe('blob')
    expect(config.url).toBe('/certificates/certificate/download')
    return {
      config,
      status: 200,
      statusText: '',
      headers: { 'content-disposition': disposition },
      data: blob,
    }
  }
  const target = { id: 'certificate', domains: ['example.com'] }
  expect(await downloadCertificate(target)).toEqual({
    blob,
    filename: 'issued.zip',
  })
  disposition = undefined
  expect(await downloadCertificate(target)).toEqual({
    blob,
    filename: 'example.com.zip',
  })
})

test('attention filters are server parameters: they reach the stream URL and the cache key', async () => {
  const urls: string[] = []
  setLiveQueryQueryClient(client, undefined, (url) => {
    urls.push(url)
    return new Source() as never
  })
  const expiring = certificatesQuery({ page: 1, page_size: 20, expiring: 1 })
  const abnormal = certificatesQuery({ page: 1, page_size: 20, abnormal: 1 })
  expect(expiring.queryKey).not.toEqual(abnormal.queryKey)
  for (const options of [expiring, abnormal]) {
    const observer = new QueryObserver(client, options)
    const stop = observer.subscribe(() => undefined)
    void observer.refetch().catch(() => undefined)
    await new Promise<void>((resolve) => queueMicrotask(resolve))
    stop()
    observer.destroy()
  }
  expect(urls).toEqual([
    '/api/certificates/stream?expiring=1&page=1&page_size=20',
    '/api/certificates/stream?abnormal=1&page=1&page_size=20',
  ])
})
