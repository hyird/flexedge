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
  usableCertificateOptionsQuery,
  certificatesQuery,
} from './data'

class Source extends EventTarget {
  close() {}
}

const client = new QueryClient({ defaultOptions: { queries: { retry: false } } })

const adapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = adapter
  setLiveQueryQueryClient(client)
  client.clear()
})

test('certificate snapshots replace option and filtered list caches', async () => {
  const source = new Source()
  setLiveQueryQueryClient(client, undefined, () => source as never)
  const options = new QueryObserver(client, usableCertificateOptionsQuery)
  const stop = options.subscribe(() => undefined)
  const pending = options.refetch()
  source.dispatchEvent(new MessageEvent('snapshot', {
    data: JSON.stringify({ code: 0, message: 'ok', data: [{ id: 'first' }] }),
  }))
  await expect(pending).resolves.toMatchObject({ data: [{ id: 'first' }] })
  source.dispatchEvent(new MessageEvent('snapshot', {
    data: JSON.stringify({ code: 0, message: 'ok', data: [{ id: 'replacement' }] }),
  }))
  await new Promise<void>((resolve) => queueMicrotask(resolve))
  expect(client.getQueryData(usableCertificateOptionsQuery.queryKey)).toEqual([{ id: 'replacement' }])
  stop()
  options.destroy()

  const listSource = new Source()
  setLiveQueryQueryClient(client, undefined, () => listSource as never)
  const query = new QueryObserver(client, certificatesQuery({ page: 2, page_size: 10, keyword: 'example' }))
  const unsubscribe = query.subscribe(() => undefined)
  const listPending = query.refetch()
  listSource.dispatchEvent(new MessageEvent('snapshot', {
    data: JSON.stringify({ code: 0, message: 'ok', data: { list: [{ id: 'certificate' }], total: 1, page: 2, page_size: 10, total_pages: 1 } }),
  }))
  await expect(listPending).resolves.toMatchObject({ data: { list: [{ id: 'certificate' }] } })
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
