import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { ApiProtocolError } from '@/lib/api-response'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  operationDetailQuery,
  operationsQuery,
  recentOperationsQuery,
  subtaskHistoryQuery,
  taskAttemptTraceQuery,
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
afterEach(() => {
  setLiveQueryQueryClient(new QueryClient())
  clients.splice(0).forEach((client) => client.clear())
})
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

const operation = {
  id: 'op-1',
  action: 'website.update',
  target_type: 'website',
  target_id: 'website-1',
  target_name: '品牌官网',
  target_deleted: false,
  actor_type: 'user',
  actor_name: 'QA 管理员',
  created_at: '2026-10-15T08:00:00.000000+00',
  updated_at: '2026-10-15T08:00:05.000000+00',
  finished_at: '',
  status: 'running',
  subtask_count: 2,
  settled_count: 1,
  failed_count: 0,
  next_attempt_at: '',
  error: '',
  summary: { sections: ['domains', 'https'] },
}
const subtask = {
  id: 'sub-1',
  parent_id: '',
  kind: 'dns_sync',
  resource_type: 'dns_zone',
  resource_id: 'zone-1',
  resource_name: 'a-z.xin',
  resource_deleted: false,
  status: 'completed',
  warning: '',
  node_offline: false,
  created_at: '2026-10-15T08:00:00.000000+00',
  running_since: '',
  finished_at: '2026-10-15T08:00:03.000000+00',
  duration_ms: 3000,
  attempt_count: 1,
  failure_count: 0,
  recovered: false,
  next_attempt_at: '',
  error: '',
}
const detail = {
  ...operation,
  subtasks: [subtask, { ...subtask, id: 'sub-2', status: 'running' }],
}
const page = {
  list: [operation],
  total: 1,
  page: 1,
  page_size: 20,
  total_pages: 1,
  active: 1,
  attention: 0,
}
const summary = {
  attempt_count: 1,
  failure_count: 0,
  first_failure_at: '',
  last_success_at: '2026-10-15T08:00:03.000000+00',
  next_attempt_at: '',
}
const attempt = (number: number, attemptId: string, outcome = 'completed') => ({
  attempt: number,
  outcome,
  error: outcome === 'failed' ? 'timeout' : '',
  started_at: '2026-10-15T08:00:00.000000+00',
  emitted_at: `2026-10-15T08:00:0${number}.000000+00`,
  duration_ms: 1000,
  next_attempt_at: '',
  detail: { kind: 'none', truncated: false },
  trace: {
    state: 'available',
    attempt_id: attemptId,
    step_count: 1,
    call_count: 0,
    log_count: 0,
    truncated: false,
    failed_step: null,
    steps: [],
  },
})
const history = (subtaskId: string, list: unknown[]) => ({
  subtask_id: subtaskId,
  truncated: false,
  summary,
  list,
})

test('the list parses operations, keeps unknown enums raw and rejects malformed pages', async () => {
  let client = setup()
  const filters = { page: 1, page_size: 20, status: 'attention', actor: 'me' }
  const read = client.fetchQuery(operationsQuery(filters))
  await settle()
  expect(sources[0].url).toBe(
    '/api/operations/stream?actor=me&page=1&page_size=20&status=attention'
  )
  sources[0].frame('snapshot', {
    ...page,
    list: [
      {
        ...operation,
        status: 'future_status',
        action: 'website.future',
        target_type: 'gateway',
        // One malformed summary key never hides the others.
        summary: { sections: ['domains'], record_counts: 'many', extra: 1 },
      },
      {
        ...operation,
        id: 'op-2',
        target_type: 'provider',
        target_kind: 'certificate',
      },
    ],
  })
  const data = await read
  expect(data.list[0]).toMatchObject({
    status: 'future_status',
    action: 'website.future',
    target_type: 'gateway',
    summary: { sections: ['domains'] },
  })
  expect(data.list[0].summary.record_counts).toBeUndefined()
  expect(data.list[0].target_kind).toBeUndefined()
  expect(data.list[1].target_kind).toBe('certificate')
  for (const value of [
    { ...page, list: [{ ...operation, subtask_count: -1 }] },
    { ...page, attention: '0' },
    { ...page, list: null },
    { ...page, page: 2 },
  ]) {
    client = setup()
    const failed = client.fetchQuery(operationsQuery())
    await settle()
    sources[0].frame('snapshot', value)
    await expect(failed).rejects.toBeInstanceOf(ApiProtocolError)
  }
})

test('header and overview reads take one snapshot and close the stream', async () => {
  const client = setup()
  const query = recentOperationsQuery(6)
  expect(query.queryKey).toContain('once')
  const observer = new QueryObserver(client, query)
  const stop = observer.subscribe(() => undefined)
  await settle()
  expect(sources.map((source) => source.url)).toEqual([
    '/api/operations/stream?page=1&page_size=6',
  ])
  sources[0].frame('snapshot', { ...page, page_size: 6 })
  await settle()
  expect(observer.getCurrentResult().data?.list).toHaveLength(1)
  // Closed while still observed: no live subscription.
  expect(sources[0].closed).toBe(true)
  stop()
})

test('one detail stream carries the task, the expanded records and new attempts', async () => {
  const client = setup()
  const header = new QueryObserver(
    client,
    operationDetailQuery('op-1', 'sub-1')
  )
  const records = new QueryObserver(
    client,
    subtaskHistoryQuery('op-1', 'sub-1')
  )
  const stops = [header, records].map((observer) =>
    observer.subscribe(() => undefined)
  )
  await settle()
  expect(sources.map((source) => source.url)).toEqual([
    '/api/operations/op-1/stream?subtask_id=sub-1',
  ])
  const source = sources[0]
  source.frame('snapshot', detail)
  source.frame('history', history('sub-1', [attempt(1, '10')]))
  await settle()
  expect(header.getCurrentResult().data?.subtasks).toHaveLength(2)
  expect(
    records.getCurrentResult().data?.list.map((item) => item.attempt)
  ).toEqual([1])

  // A new attempt goes on top and replaces the summary.
  source.raw('attempt', {
    subtask_id: 'sub-1',
    item: attempt(2, '11', 'failed'),
    summary: { ...summary, attempt_count: 2, failure_count: 1 },
  })
  await settle()
  let data = records.getCurrentResult().data
  expect(data?.list.map((item) => item.attempt)).toEqual([2, 1])
  expect(data?.summary).toMatchObject({ attempt_count: 2, failure_count: 1 })

  // The same attempt again (e.g. replayed after a reconnect) is not duplicated.
  source.raw('attempt', {
    subtask_id: 'sub-1',
    item: attempt(2, '11', 'failed'),
    summary: { ...summary, attempt_count: 2, failure_count: 1 },
  })
  // Records of another subtask and malformed events change nothing.
  source.raw('attempt', {
    subtask_id: 'sub-2',
    item: attempt(9, '19'),
    summary,
  })
  source.raw('attempt', { subtask_id: 'sub-1', item: 'broken' })
  await settle()
  data = records.getCurrentResult().data
  expect(data?.list.map((item) => item.attempt)).toEqual([2, 1])
  expect(records.getCurrentResult().isError).toBe(false)
  expect(header.getCurrentResult().isError).toBe(false)

  // A `history` frame replaces the records as a whole.
  source.frame('history', history('sub-1', [attempt(3, '12')]))
  await settle()
  expect(
    records.getCurrentResult().data?.list.map((item) => item.attempt)
  ).toEqual([3])
  expect(sources).toHaveLength(1)
  stops.forEach((stop) => stop())
  await settle()
  expect(source.closed).toBe(true)
})

test('the records keep the newest 100 attempts', async () => {
  const client = setup()
  const records = new QueryObserver(
    client,
    subtaskHistoryQuery('op-1', 'sub-1')
  )
  const stop = records.subscribe(() => undefined)
  await settle()
  sources[0].frame(
    'history',
    history(
      'sub-1',
      Array.from({ length: historyRecordLimit }, (_, index) =>
        attempt(historyRecordLimit - index, String(1000 - index))
      )
    )
  )
  // Each SSE event is its own browser task; the first frame settles first.
  await settle()
  sources[0].raw('attempt', {
    subtask_id: 'sub-1',
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

test('expanding another subtask releases the old connection first and keeps the task on screen', async () => {
  const client = setup()
  const header = new QueryObserver(client, operationDetailQuery('op-1', null))
  const stopHeader = header.subscribe(() => undefined)
  await settle()
  sources[0].frame('snapshot', detail)
  await settle()
  expect(header.getCurrentResult().data?.id).toBe('op-1')

  // Expand sub-1: the URL changes in one commit.
  log = []
  header.setOptions(operationDetailQuery('op-1', 'sub-1'))
  const records = new QueryObserver(
    client,
    subtaskHistoryQuery('op-1', 'sub-1')
  )
  const stopRecords = records.subscribe(() => undefined)
  await settle()
  expect(log).toEqual([
    'close /api/operations/op-1/stream',
    'open /api/operations/op-1/stream?subtask_id=sub-1',
  ])
  // No skeleton: the task stays while the new connection opens.
  let result = header.getCurrentResult()
  expect(result.isPending).toBe(false)
  expect(result.isPlaceholderData).toBe(true)
  expect(result.data?.id).toBe('op-1')

  // Expand sub-2 instead.
  log = []
  stopRecords()
  header.setOptions(operationDetailQuery('op-1', 'sub-2'))
  const next = new QueryObserver(client, subtaskHistoryQuery('op-1', 'sub-2'))
  const stopNext = next.subscribe(() => undefined)
  await settle()
  expect(log).toEqual([
    'close /api/operations/op-1/stream?subtask_id=sub-1',
    'open /api/operations/op-1/stream?subtask_id=sub-2',
  ])
  expect(sources.filter((source) => !source.closed)).toHaveLength(1)
  result = header.getCurrentResult()
  expect(result.data?.id).toBe('op-1')
  sources.at(-1)?.frame('snapshot', { ...detail, status: 'completed' })
  await settle()
  expect(header.getCurrentResult().data?.status).toBe('completed')
  expect(header.getCurrentResult().isPlaceholderData).toBe(false)

  // Another task never shows this task's data as a placeholder.
  stopNext()
  header.setOptions(operationDetailQuery('op-2', null))
  await settle()
  expect(header.getCurrentResult().data).toBeUndefined()
  stopHeader()
})

test('unknown record values and malformed details degrade per attempt', async () => {
  const client = setup()
  const read = client.fetchQuery(subtaskHistoryQuery('op-1', 'sub-1'))
  await settle()
  sources[0].frame(
    'history',
    history('sub-1', [
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

const traceUrl = (attemptId: string) =>
  `/api/operations/op-1/attempts/${attemptId}/trace/stream`
const traceSnapshot = { state: 'available', steps: [] }

test('a trace is a cached one-shot read, opened only once its attempt is visible', async () => {
  const client = setup()
  const observer = new QueryObserver(
    client,
    taskAttemptTraceQuery('op-1', '48213', false)
  )
  const stop = observer.subscribe(() => undefined)
  await settle()
  expect(observer.getCurrentResult().fetchStatus).toBe('idle')
  expect(sources).toEqual([])
  observer.setOptions(taskAttemptTraceQuery('op-1', '48213', true))
  await settle()
  expect(sources.map((source) => source.url)).toEqual([traceUrl('48213')])
  sources[0].frame('snapshot', traceSnapshot)
  await settle()
  expect(observer.getCurrentResult().data?.state).toBe('available')
  expect(sources[0].closed).toBe(true)
  // Traces never change: a cached trace is never read again.
  await client.fetchQuery(taskAttemptTraceQuery('op-1', '48213'))
  expect(sources).toHaveLength(1)
  stop()
  const broken = client.fetchQuery(taskAttemptTraceQuery('op-1', '48214'))
  await settle()
  sources[1].frame('snapshot', { ...traceSnapshot, steps: 'broken' })
  await expect(broken).rejects.toBeInstanceOf(ApiProtocolError)
})

test('at most two trace reads are open at once; queued reads wait in order and cancel cleanly', async () => {
  const client = setup()
  const ids = ['1', '2', '3', '4', '5']
  const observers = ids.map(
    (id) => new QueryObserver(client, taskAttemptTraceQuery('op-1', id))
  )
  const stops = observers.map((observer) => observer.subscribe(() => undefined))
  await settle()
  expect(
    observers.map((observer) => observer.getCurrentResult().fetchStatus)
  ).toEqual(Array(5).fill('fetching'))
  const urls = () => sources.map((source) => source.url)
  expect(urls()).toEqual([traceUrl('1'), traceUrl('2')])

  // Closing a queued attempt (the sheet unmounting it) never opens its read.
  stops[2]()
  sources[0].frame('snapshot', traceSnapshot)
  await settle()
  expect(urls()).toEqual([traceUrl('1'), traceUrl('2'), traceUrl('4')])
  expect(observers[0].getCurrentResult().data?.state).toBe('available')

  // Cancelling an open read frees its slot as well.
  stops[1]()
  await settle()
  expect(urls().at(-1)).toBe(traceUrl('5'))
  expect(urls()).toHaveLength(4)
  for (const source of sources.slice(2)) source.frame('snapshot', traceSnapshot)
  await settle()
  expect(
    [0, 3, 4].map((index) => observers[index].getCurrentResult().status)
  ).toEqual(['success', 'success', 'success'])
  expect(observers[2].getCurrentResult().data).toBeUndefined()
  stops.forEach((stop) => stop())
})
