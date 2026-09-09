import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import { ApiProtocolError } from '@/lib/api-response'
import { taskDetailQuery, taskHistoryQuery, tasksQuery } from './data'
import type { Task } from './types'

const clients: QueryClient[] = []
let nextSnapshot: unknown
class Source extends EventTarget { close() {} }
afterEach(() => {
  setLiveQueryQueryClient(new QueryClient())
  clients.splice(0).forEach((client) => client.clear())
})
const task: Task = {
  id: 'task-1',
  resource_type: 'website',
  resource_id: 'website-1',
  name: 'Website',
  operation: 'apply',
  version: 3,
  status: 'completed',
  error: '',
  failures: 0,
  updated_at: '2026-09-08T00:00:00Z',
  next_attempt_at: '',
}
const page = {
  list: [task],
  total: 1,
  page: 1,
  page_size: 6,
  total_pages: 1,
  active: 0,
  failed: 0,
}
function setup() {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false } },
  })
  clients.push(client)
  setLiveQueryQueryClient(client, undefined, () => {
    const source = new Source()
    queueMicrotask(() => source.dispatchEvent(new MessageEvent('snapshot', {
      data: JSON.stringify({ code: 0, message: '', data: nextSnapshot }),
    })))
    return source as never
  })
  return client
}
function respond(data: unknown) {
  nextSnapshot = data
}

test('list rejects malformed task states and pagination before caching', async () => {
  for (const value of [
    { ...page, list: [{ ...task, status: 'unexpected' }] },
    { ...page, active: '0' },
    { ...page, list: null },
    { ...page, page: 2 },
  ]) {
    const client = setup()
    respond(value)
    await expect(client.fetchQuery(tasksQuery())).rejects.toBeInstanceOf(
      ApiProtocolError
    )
    expect(client.getQueryData(tasksQuery().queryKey)).toBeUndefined()
  }
  respond(page)
  expect(await setup().fetchQuery(tasksQuery({ page_size: 6 }))).toEqual(page)
})

test('detail and history own their revision parameters and validate their responses', async () => {
  const client = setup()
  nextSnapshot = task
  expect(await client.fetchQuery(taskDetailQuery(task))).toEqual(task)
  nextSnapshot = { list: [{ outcome: 'failed', error: 'timeout', emitted_at: '2026-09-08' }], truncated: false }
  expect((await client.fetchQuery(taskHistoryQuery(task))).list[0].error).toBe(
    'timeout'
  )
  respond({ ...task, failures: -1 })
  await expect(client.fetchQuery(taskDetailQuery(task))).rejects.toBeInstanceOf(
    ApiProtocolError
  )
  respond({
    list: [{ outcome: 'unknown', error: '', emitted_at: '' }],
    truncated: false,
  })
  await expect(
    client.fetchQuery(taskHistoryQuery(task))
  ).rejects.toBeInstanceOf(ApiProtocolError)
})

test('missing selections and node history do not start requests', () => {
  const client = setup()
  const observers = [
    new QueryObserver(client, taskDetailQuery(null)),
    new QueryObserver(client, taskHistoryQuery(null)),
    new QueryObserver(
      client,
      taskHistoryQuery({ ...task, resource_type: 'node' })
    ),
  ]
  const stops = observers.map((observer) => observer.subscribe(() => undefined))
  expect(
    observers.map((observer) => observer.getCurrentResult().fetchStatus)
  ).toEqual(['idle', 'idle', 'idle'])
  stops.forEach((stop) => stop())
})
