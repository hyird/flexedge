import type { InternalAxiosRequestConfig } from 'axios'
import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { api } from '@/lib/api'
import { ApiProtocolError } from '@/lib/api-response'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  recentTasksQuery,
  taskAttemptTraceQuery,
  taskDetailQuery,
  taskHistoryQuery,
  tasksQuery,
} from './data'
import { historyRecordLimit } from './task-history'

/** Fake EventSource: frames are sent by the test; `log` records opens and
 * closes in order. */
class Source extends EventTarget {
  closed = false
  constructor(readonly url: string) {
    super()
  }
  close() {
    this.closed = true
    log.push(`close ${this.url}`)
  }
  frame(event: string, data: unknown) {
    this.dispatchEvent(
      new MessageEvent(event, {
        data: JSON.stringify({ code: 0, message: 'ok', data }),
      })
    )
  }
  /** Plain-JSON event (`attempt`). */
  raw(event: string, data: unknown) {
    this.dispatchEvent(new MessageEvent(event, { data: JSON.stringify(data) }))
  }
}
let sources: Source[] = []
let log: string[] = []
const clients: QueryClient[] = []
const adapter = api.defaults.adapter
afterEach(() => {
  api.defaults.adapter = adapter
  setLiveQueryQueryClient(new QueryClient())
  clients.splice(0).forEach((client) => client.clear())
})

/** Answers GET reads with `respond(config)` as the envelope data; returns
 * the requests in order. */
function serve(respond: (config: InternalAxiosRequestConfig) => unknown) {
  const requests: InternalAxiosRequestConfig[] = []
  api.defaults.adapter = async (config) => {
    requests.push(config)
    return {
      config,
      status: 200,
      statusText: '',
      headers: {},
      data: { code: 0, message: 'ok', data: respond(config) },
    }
  }
  return requests
}

function setup() {
  sources = []
  log = []
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false } },
  })
  clients.push(client)
  setLiveQueryQueryClient(client, undefined, (url) => {
    const source = new Source(url)
    sources.push(source)
    log.push(`open ${url}`)
    return source as never
  })
  return client
}
// Runs the queued promise jobs (stream opening, frame parsing, releases)
// without waiting on the clock.
async function settle() {
  for (let turn = 0; turn < 20; turn += 1) await Promise.resolve()
}

const ref = { resourceType: 'certificate', resourceId: 'cert-1' }
const task = {
  resource_type: 'certificate',
  resource_id: 'cert-1',
  resource_kind: '',
  name: 'www.a-z.xin',
  operation: 'issue',
  status: 'running',
  error: '',
  next_attempt_at: '',
  updated_at: '2026-10-15T08:00:05.000000+00',
  last_success_at: '',
  attempt_count: 1,
  failure_count: 0,
  node_offline: false,
}
const page = {
  list: [task],
  total: 1,
  page: 1,
  page_size: 20,
  total_pages: 1,
  active: 1,
  failed: 0,
}
const summary = {
  attempt_count: 1,
  failure_count: 0,
  first_failure_at: '',
  last_success_at: '2026-10-15T08:00:03.000000+00',
}
const attempt = (number: number, id: string, outcome = 'completed') => ({
  id,
  attempt: number,
  operation: 'issue',
  outcome,
  error: outcome === 'failed' ? 'timeout' : '',
  started_at: '2026-10-15T08:00:00.000000+00',
  emitted_at: `2026-10-15T08:00:0${number}.000000+00`,
  duration_ms: 1000,
  next_attempt_at: '',
  detail: { kind: 'none', truncated: false },
  trace: {
    state: 'available',
    attempt_id: id,
    step_count: 1,
    call_count: 0,
    log_count: 0,
    truncated: false,
    failed_step: null,
    steps: [],
  },
})
const history = (list: unknown[]) => ({ truncated: false, summary, list })

test('the list reads the filtered task stream, keeps unknown enums raw and rejects malformed pages', async () => {
  let client = setup()
  const filters = {
    page: 1,
    page_size: 20,
    status: 'attention',
    type: 'certificate',
    keyword: 'a-z',
  }
  const read = client.fetchQuery(tasksQuery(filters))
  await settle()
  expect(sources[0].url).toBe(
    '/api/tasks/stream?keyword=a-z&page=1&page_size=20&status=attention&type=certificate'
  )
  sources[0].frame('snapshot', {
    ...page,
    list: [
      { ...task, status: 'future_status', resource_type: 'gateway' },
      {
        ...task,
        resource_type: 'provider',
        resource_id: 'p-1',
        resource_kind: 'certificate',
        operation: 'verify',
      },
    ],
  })
  const data = await read
  expect(data.list[0]).toMatchObject({
    status: 'future_status',
    resource_type: 'gateway',
  })
  expect(data.list[1].resource_kind).toBe('certificate')
  for (const value of [
    { ...page, list: [{ ...task, failure_count: -1 }] },
    { ...page, failed: '0' },
    { ...page, list: null },
    { ...page, page: 2 },
  ]) {
    client = setup()
    const failed = client.fetchQuery(tasksQuery())
    await settle()
    sources[0].frame('snapshot', value)
    await expect(failed).rejects.toBeInstanceOf(ApiProtocolError)
  }
})

test('header and overview read the recent tasks over GET, never a stream', async () => {
  const client = setup()
  const requests = serve(() => ({ ...page, page_size: 6 }))
  const data = await client.fetchQuery(recentTasksQuery(6))
  expect(requests.map((config) => [config.url, config.params])).toEqual([
    ['/tasks', { page: 1, page_size: 6 }],
  ])
  expect(data.list).toHaveLength(1)
  expect(sources).toEqual([])
  serve(() => ({ ...page, list: null }))
  await expect(client.fetchQuery(recentTasksQuery(20))).rejects.toBeInstanceOf(
    ApiProtocolError
  )
})

test('the detail sheet over the list switches the page stream and back', async () => {
  const client = setup()
  const list = new QueryObserver(client, tasksQuery())
  const stopList = list.subscribe(() => undefined)
  await settle()
  sources[0].frame('snapshot', page)
  await settle()
  const header = new QueryObserver(client, taskDetailQuery(ref))
  const records = new QueryObserver(client, taskHistoryQuery(ref))
  const stopSheet = [header, records].map((observer) =>
    observer.subscribe(() => undefined)
  )
  await settle()
  expect(sources.filter((source) => !source.closed)).toHaveLength(1)
  expect(log).toEqual([
    'open /api/tasks/stream',
    'close /api/tasks/stream',
    'open /api/tasks/certificate/cert-1/stream',
  ])
  // The list keeps its snapshot underneath.
  expect(list.getCurrentResult().data?.total).toBe(1)
  stopSheet.forEach((stop) => stop())
  await settle()
  expect(log.slice(3)).toEqual([
    'close /api/tasks/certificate/cert-1/stream',
    'open /api/tasks/stream',
  ])
  expect(sources.filter((source) => !source.closed)).toHaveLength(1)
  stopList()
})

test('one detail stream carries the task, its records and new attempts', async () => {
  const client = setup()
  const header = new QueryObserver(client, taskDetailQuery(ref))
  const records = new QueryObserver(client, taskHistoryQuery(ref))
  const stops = [header, records].map((observer) =>
    observer.subscribe(() => undefined)
  )
  await settle()
  expect(sources.map((source) => source.url)).toEqual([
    '/api/tasks/certificate/cert-1/stream',
  ])
  const source = sources[0]
  source.frame('snapshot', task)
  source.frame('history', history([attempt(1, '10')]))
  await settle()
  expect(header.getCurrentResult().data?.name).toBe('www.a-z.xin')
  expect(
    records.getCurrentResult().data?.list.map((item) => item.attempt)
  ).toEqual([1])

  // A new attempt goes on top and replaces the summary.
  source.raw('attempt', {
    item: attempt(2, '11', 'failed'),
    summary: { ...summary, attempt_count: 2, failure_count: 1 },
  })
  await settle()
  let data = records.getCurrentResult().data
  expect(data?.list.map((item) => item.attempt)).toEqual([2, 1])
  expect(data?.summary).toMatchObject({ attempt_count: 2, failure_count: 1 })

  // The same attempt again (e.g. replayed after a reconnect) is not
  // duplicated; malformed events change nothing.
  source.raw('attempt', {
    item: attempt(2, '11', 'failed'),
    summary: { ...summary, attempt_count: 2, failure_count: 1 },
  })
  source.raw('attempt', { item: 'broken', summary })
  source.raw('attempt', { item: attempt(3, '12') })
  await settle()
  data = records.getCurrentResult().data
  expect(data?.list.map((item) => item.id)).toEqual(['11', '10'])
  expect(records.getCurrentResult().isError).toBe(false)
  expect(header.getCurrentResult().isError).toBe(false)

  // A recovery `history` frame replaces the records as a whole.
  source.frame(
    'history',
    history([attempt(3, '12'), attempt(2, '11', 'failed')])
  )
  await settle()
  expect(records.getCurrentResult().data?.list.map((item) => item.id)).toEqual([
    '12',
    '11',
  ])
  expect(sources).toHaveLength(1)
  stops.forEach((stop) => stop())
  await settle()
  expect(source.closed).toBe(true)
})

test('the records keep the newest 100 attempts', async () => {
  const client = setup()
  const records = new QueryObserver(client, taskHistoryQuery(ref))
  const stop = records.subscribe(() => undefined)
  await settle()
  sources[0].frame(
    'history',
    history(
      Array.from({ length: historyRecordLimit }, (_, index) =>
        attempt(historyRecordLimit - index, String(1000 - index))
      )
    )
  )
  // Each SSE event is its own browser task; the first frame settles first.
  await settle()
  sources[0].raw('attempt', {
    item: attempt(101, '2000'),
    summary: { ...summary, attempt_count: 101 },
  })
  await settle()
  const list = records.getCurrentResult().data?.list ?? []
  expect(list).toHaveLength(historyRecordLimit)
  expect(list[0].attempt).toBe(101)
  expect(list.at(-1)?.attempt).toBe(2)
  stop()
})

test('unknown record values and malformed details degrade per attempt', async () => {
  const client = setup()
  const read = client.fetchQuery(taskHistoryQuery(ref))
  await settle()
  sources[0].frame(
    'history',
    history([
      { ...attempt(3, '3'), outcome: 'cancelled' },
      { ...attempt(2, '2'), detail: { kind: 'future_kind' } },
      { ...attempt(1, '1'), trace: 'broken' },
    ])
  )
  const data = await read
  expect(data.list.map((item) => item.outcome)).toEqual([
    'cancelled',
    'completed',
    'completed',
  ])
  expect(data.list[1].detail).toEqual({ kind: 'invalid' })
  expect(data.list[2].trace).toEqual({ invalid: true })
})

const tracePath = (attemptId: string) =>
  `/tasks/certificate/cert-1/attempts/${attemptId}/trace`
const traceSnapshot = { state: 'available', steps: [] }

test('a trace is a cached GET read, made only once its attempt is visible', async () => {
  const client = setup()
  const requests = serve(() => traceSnapshot)
  const observer = new QueryObserver(
    client,
    taskAttemptTraceQuery(ref, '48213', false)
  )
  const stop = observer.subscribe(() => undefined)
  await settle()
  expect(observer.getCurrentResult().fetchStatus).toBe('idle')
  expect(requests).toEqual([])
  observer.setOptions(taskAttemptTraceQuery(ref, '48213', true))
  // Joins the read the now-visible attempt started.
  const trace = await client.fetchQuery(taskAttemptTraceQuery(ref, '48213'))
  expect(requests.map((config) => config.url)).toEqual([tracePath('48213')])
  expect(trace.state).toBe('available')
  // Traces never change: a cached trace is never read again.
  await client.fetchQuery(taskAttemptTraceQuery(ref, '48213'))
  expect(requests).toHaveLength(1)
  expect(sources).toEqual([])
  stop()
  serve(() => ({ ...traceSnapshot, steps: 'broken' }))
  await expect(
    client.fetchQuery(taskAttemptTraceQuery(ref, '48214'))
  ).rejects.toBeInstanceOf(ApiProtocolError)
})
