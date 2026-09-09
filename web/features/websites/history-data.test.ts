import { QueryClient } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import { websiteAccessLogHistoryQuery } from './data'

class Source extends EventTarget {
  close() {}
}

const client = new QueryClient({
  defaultOptions: { queries: { retry: false } },
})
afterEach(() => {
  setLiveQueryQueryClient(client)
  client.clear()
})

function snapshot(data: unknown) {
  return new MessageEvent('snapshot', {
    data: JSON.stringify({ code: 0, message: 'ok', data }),
  })
}

test('history page sizes use separate cache entries and preserve request pagination', async () => {
  const sources: Source[] = []
  const urls: string[] = []
  setLiveQueryQueryClient(client, undefined, (url) => {
    urls.push(url)
    const source = new Source()
    sources.push(source)
    return source as never
  })
  const requestedSizes: number[] = []
  for (const pageSize of [100, 500, 1000]) {
    const params = { page: 1, page_size: pageSize, keyword: '/api' }
    const pending = client.fetchQuery(websiteAccessLogHistoryQuery('website', params))
    requestedSizes.push(pageSize)
    sources.at(-1)?.dispatchEvent(snapshot({
      list: [], page: params.page, page_size: pageSize, total: 0, total_pages: 0,
    }))
    const result = await pending
    expect(result.page_size).toBe(pageSize)
  }
  expect(requestedSizes).toEqual([100, 500, 1000])
  expect(urls).toEqual([
    '/api/websites/website/access-logs/history/stream?keyword=%2Fapi&page=1&page_size=100',
    '/api/websites/website/access-logs/history/stream?keyword=%2Fapi&page=1&page_size=500',
    '/api/websites/website/access-logs/history/stream?keyword=%2Fapi&page=1&page_size=1000',
  ])
  for (const pageSize of [100, 500, 1000]) {
    const { queryKey } = websiteAccessLogHistoryQuery('website', {
      page: 1,
      page_size: pageSize,
      keyword: '/api',
    })
    expect(client.getQueryData(queryKey)?.page_size).toBe(pageSize)
  }
})
test('history rejects invalid records without caching them and passes filters and cancellation', async () => {
  const params = {
    page: 2,
    page_size: 50,
    keyword: '/api',
    method: 'GET',
    status_class: '5xx',
  }
  const options = websiteAccessLogHistoryQuery('website', params)
  const sources: Source[] = []
  const urls: string[] = []
  setLiveQueryQueryClient(client, undefined, (url) => {
    urls.push(url)
    const source = new Source()
    sources.push(source)
    return source as never
  })
  const pending = client.fetchQuery(options)
  sources[0].dispatchEvent(snapshot({
    list: [{ id: 'incomplete' }], page: 2, page_size: 50, total: 0, total_pages: 1,
  }))
  await expect(pending).rejects.toThrow()
  expect(client.getQueryData(options.queryKey)).toBeUndefined()
  await new Promise<void>((resolve) => queueMicrotask(resolve))
  const retry = client.fetchQuery(options)
  sources.at(-1)?.dispatchEvent(snapshot({
    list: [], page: 2, page_size: 50, total: 0, total_pages: 1,
  }))
  expect((await retry).list).toEqual([])
  expect(urls[0]).toBe('/api/websites/website/access-logs/history/stream?keyword=%2Fapi&method=GET&page=2&page_size=50&status_class=5xx')
})
