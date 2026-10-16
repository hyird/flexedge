import { QueryClient } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { websiteAccessLogHistoryQuery } from './data'

const adapter = api.defaults.adapter
const client = new QueryClient({
  defaultOptions: { queries: { retry: false } },
})
afterEach(() => {
  api.defaults.adapter = adapter
  client.clear()
})

/** Answers history reads with `page(params)` and records their params. */
function serve(page: (params: Record<string, unknown>) => unknown) {
  const requests: unknown[] = []
  api.defaults.adapter = async (config) => {
    requests.push([config.url, config.params])
    return {
      config,
      status: 200,
      statusText: '',
      headers: {},
      data: { code: 0, message: 'ok', data: page(config.params) },
    }
  }
  return requests
}

test('history page sizes use separate cache entries and preserve request pagination', async () => {
  const requests = serve((params) => ({
    list: [],
    page: params.page,
    page_size: params.page_size,
    total: 0,
    total_pages: 0,
  }))
  for (const pageSize of [100, 500, 1000]) {
    const params = { page: 1, page_size: pageSize, keyword: '/api' }
    const result = await client.fetchQuery(
      websiteAccessLogHistoryQuery('website', params)
    )
    expect(result.page_size).toBe(pageSize)
  }
  expect(requests).toEqual(
    [100, 500, 1000].map((pageSize) => [
      '/websites/website/access-logs/history',
      { page: 1, page_size: pageSize, keyword: '/api' },
    ])
  )
  for (const pageSize of [100, 500, 1000]) {
    const { queryKey } = websiteAccessLogHistoryQuery('website', {
      page: 1,
      page_size: pageSize,
      keyword: '/api',
    })
    expect(client.getQueryData(queryKey)?.page_size).toBe(pageSize)
  }
})

test('history rejects invalid records without caching them and passes filters', async () => {
  const params = {
    page: 2,
    page_size: 50,
    keyword: '/api',
    method: 'GET',
    status_class: '5xx',
  }
  const options = websiteAccessLogHistoryQuery('website', params)
  let list: unknown[] = [{ id: 'incomplete' }]
  const requests = serve(() => ({
    list,
    page: 2,
    page_size: 50,
    total: 0,
    total_pages: 1,
  }))
  await expect(client.fetchQuery(options)).rejects.toThrow()
  expect(client.getQueryData(options.queryKey)).toBeUndefined()
  list = []
  expect((await client.fetchQuery(options)).list).toEqual([])
  expect(requests[0]).toEqual(['/websites/website/access-logs/history', params])
})
