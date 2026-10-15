import { QueryClient, QueryObserver } from '@tanstack/react-query'
import { afterEach, expect, test } from 'vitest'
import { ApiProtocolError } from '@/lib/api-response'
import { setLiveQueryQueryClient } from '@/lib/live-query'
import {
  latestTaskVersionQuery,
  taskAttemptTraceQuery,
  taskDetailQuery,
  taskHistoryQuery,
  tasksQuery,
} from './data'
import type { Task } from './types'

const clients: QueryClient[] = []
let nextSnapshot: unknown
const urls: string[] = []
// While `holdFrames` is set, each stream's snapshot waits in `heldFrames`
// until the test sends it, so reads stay open.
let holdFrames = false
const heldFrames: (() => void)[] = []
class Source extends EventTarget {
  close() {}
}
afterEach(() => {
  holdFrames = false
  heldFrames.splice(0).forEach((send) => send())
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
  setLiveQueryQueryClient(client, undefined, (url) => {
    urls.push(url)
    const source = new Source()
    const data = nextSnapshot
    const send = () =>
      source.dispatchEvent(
        new MessageEvent('snapshot', {
          data: JSON.stringify({ code: 0, message: '', data }),
        })
      )
    if (holdFrames) heldFrames.push(send)
    else queueMicrotask(send)
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

const summary = {
  attempt_count: 1,
  failure_count: 1,
  first_failure_at: '2026-09-08T00:00:00Z',
  last_success_at: '',
  next_attempt_at: '',
}
const legacyAttempt = {
  attempt: 1,
  outcome: 'failed',
  error: 'timeout',
  started_at: '',
  emitted_at: '2026-09-08T00:00:00Z',
  duration_ms: 0,
  next_attempt_at: '',
  detail: { kind: 'none', truncated: false },
}

test('detail and history own their revision parameters and validate their responses', async () => {
  const client = setup()
  nextSnapshot = task
  expect(await client.fetchQuery(taskDetailQuery(task))).toEqual(task)
  respond({ truncated: false, summary, list: [legacyAttempt] })
  expect((await client.fetchQuery(taskHistoryQuery(task))).list[0].error).toBe(
    'timeout'
  )
  // Publish ids are shared by every node: resource_id selects this one.
  expect(urls.at(-1)).toBe(
    '/api/tasks/task-1/history/stream?resource_id=website-1&version=3'
  )
  respond({ ...task, failures: -1 })
  await expect(client.fetchQuery(taskDetailQuery(task))).rejects.toBeInstanceOf(
    ApiProtocolError
  )
  // Only the attempt envelope and summary are strict; enums and details
  // degrade per attempt (see below).
  for (const value of [
    {
      truncated: false,
      summary,
      list: [{ ...legacyAttempt, duration_ms: -1 }],
    },
    { truncated: false, summary, list: [{ ...legacyAttempt, attempt: 0 }] },
    { truncated: false, summary: { ...summary, attempt_count: -1 }, list: [] },
    { truncated: false, summary, list: null },
  ]) {
    respond(value)
    await expect(
      client.fetchQuery(taskHistoryQuery({ ...task, version: 9 }))
    ).rejects.toBeInstanceOf(ApiProtocolError)
  }
})

test('unknown history values and malformed details degrade per attempt', async () => {
  const nodeDetail = {
    kind: 'node_apply',
    truncated: false,
    node_id: 'node-1',
    node_name: 'edge-1',
    agent_version: '1.2.0',
    source: 'replay',
    result: 'rolled_back',
    phase: 'warmup',
    error_code: 'E1',
    retryable: true,
  }
  const domainsDetail = {
    kind: 'website_domains',
    truncated: false,
    expected_target: 'edge.example.com',
    domain_count: 1,
    checked_count: 1,
    domains: [
      {
        hostname: 'www.example.com',
        query_name: 'www.example.com',
        expected: '',
        resolved: [],
        status: 'pending_dns',
        error: '',
      },
    ],
  }
  respond({
    truncated: false,
    summary: { ...summary, attempt_count: 6 },
    list: [
      { ...legacyAttempt, attempt: 6, outcome: 'cancelled' },
      { ...legacyAttempt, attempt: 5, detail: nodeDetail },
      { ...legacyAttempt, attempt: 4, detail: domainsDetail },
      {
        ...legacyAttempt,
        attempt: 3,
        detail: { kind: 'dns_sync', truncated: false, created: [] },
      },
      { ...legacyAttempt, attempt: 2, detail: { kind: 'future_kind' } },
      { ...legacyAttempt, attempt: 1, outcome: 'completed' },
    ],
  })
  const history = await setup().fetchQuery(taskHistoryQuery(task))
  expect(history.list.map((item) => [item.attempt, item.outcome])).toEqual([
    [6, 'cancelled'],
    [5, 'failed'],
    [4, 'failed'],
    [3, 'failed'],
    [2, 'failed'],
    [1, 'completed'],
  ])
  // Unknown enum values stay as raw strings inside an otherwise valid detail.
  expect(history.list[1].detail).toEqual(nodeDetail)
  expect(history.list[2].detail).toEqual(domainsDetail)
  // A detail that does not parse only loses that attempt's detail.
  expect(history.list[3].detail).toEqual({ kind: 'invalid' })
  expect(history.list[4].detail).toEqual({ kind: 'invalid' })
  expect(history.list[5].detail).toEqual({ kind: 'none', truncated: false })
})

test('missing selections do not start requests; node tasks have history', () => {
  const client = setup()
  const observers = [
    new QueryObserver(client, taskDetailQuery(null)),
    new QueryObserver(client, taskHistoryQuery(null)),
    new QueryObserver(
      client,
      taskHistoryQuery({ id: 'publish-1', resource_id: 'node-1', version: 1 })
    ),
  ]
  const stops = observers.map((observer) => observer.subscribe(() => undefined))
  expect(
    observers.map((observer) => observer.getCurrentResult().fetchStatus)
  ).toEqual(['idle', 'idle', 'fetching'])
  stops.forEach((stop) => stop())
})

test('latest version resolves the newest record of the task for that resource', async () => {
  const client = setup()
  respond({
    ...page,
    page_size: 100,
    list: [
      { ...task, version: 2 },
      { ...task, version: 5 },
      { ...task, id: 'other', version: 9 },
    ],
  })
  expect(
    await client.fetchQuery(
      latestTaskVersionQuery({ id: task.id, resource_id: task.resource_id })
    )
  ).toBe(5)
  respond({ ...page, page_size: 100, list: [] })
  expect(
    await client.fetchQuery(
      latestTaskVersionQuery({ id: 'missing', resource_id: 'none' })
    )
  ).toBeNull()
})

test('history snapshots carry trace summaries; the full trace is a separate cached read', async () => {
  const client = setup()
  urls.length = 0
  respond({
    truncated: false,
    summary,
    list: [
      {
        ...legacyAttempt,
        trace: {
          state: 'available',
          attempt_id: '48213',
          step_count: 1,
          call_count: 0,
          log_count: 0,
          truncated: false,
          failed_step: null,
          steps: [],
        },
      },
    ],
  })
  const history = await client.fetchQuery(taskHistoryQuery(task))
  expect(history.list[0].trace).toMatchObject({ attempt_id: '48213' })
  // Reading history never opens a trace stream.
  expect(urls).toEqual([
    '/api/tasks/task-1/history/stream?resource_id=website-1&version=3',
  ])
  const trace = {
    attempt_id: '48213',
    task_id: 'task-1',
    resource_type: 'website',
    resource_id: 'website-1',
    kind: 'website_domains',
    state: 'available',
    attempt: 1,
    outcome: 'failed',
    error: 'timeout',
    started_at: '',
    emitted_at: '2026-09-08T00:00:00Z',
    duration_ms: 0,
    worker_type: 'website-dispatch',
    capture: {},
    steps: [],
  }
  respond(trace)
  const query = taskAttemptTraceQuery(task.id, '48213')
  expect((await client.fetchQuery(query)).capture.redactions).toBe(0)
  expect(urls.at(-1)).toBe('/api/tasks/task-1/attempts/48213/trace/stream')
  // Traces never change: a cached trace is never read again.
  await client.fetchQuery(query)
  expect(urls).toHaveLength(2)
  respond({ ...trace, steps: 'broken' })
  await expect(
    client.fetchQuery(taskAttemptTraceQuery(task.id, '48214'))
  ).rejects.toBeInstanceOf(ApiProtocolError)
})

const traceUrl = (attemptId: string) =>
  `/api/tasks/task-1/attempts/${attemptId}/trace/stream`
const traceSnapshot = { state: 'available', capture: {}, steps: [] }
// Runs the queued promise jobs (stream opening, frame parsing, slot
// hand-over) without waiting on the clock.
async function settle() {
  for (let turn = 0; turn < 20; turn += 1) await Promise.resolve()
}

test('a trace is not read until its attempt is visible', async () => {
  const client = setup()
  urls.length = 0
  respond(traceSnapshot)
  const observer = new QueryObserver(
    client,
    taskAttemptTraceQuery(task.id, '48213', false)
  )
  const stop = observer.subscribe(() => undefined)
  await settle()
  expect(observer.getCurrentResult().fetchStatus).toBe('idle')
  expect(urls).toEqual([])
  observer.setOptions(taskAttemptTraceQuery(task.id, '48213', true))
  await settle()
  expect(urls).toEqual([traceUrl('48213')])
  expect(observer.getCurrentResult().data?.state).toBe('available')
  stop()
})

test('at most two trace reads are open at once; queued reads wait in order and cancel cleanly', async () => {
  const client = setup()
  urls.length = 0
  respond(traceSnapshot)
  holdFrames = true
  const ids = ['1', '2', '3', '4', '5']
  const observers = ids.map(
    (id) => new QueryObserver(client, taskAttemptTraceQuery(task.id, id))
  )
  const stops = observers.map((observer) => observer.subscribe(() => undefined))
  await settle()
  // Every query is fetching, but only two streams are open.
  expect(
    observers.map((observer) => observer.getCurrentResult().fetchStatus)
  ).toEqual(Array(5).fill('fetching'))
  expect(urls).toEqual([traceUrl('1'), traceUrl('2')])

  // Closing a queued attempt (the sheet unmounting it) never opens its read.
  stops[2]()
  heldFrames.shift()?.()
  await settle()
  expect(urls).toEqual([traceUrl('1'), traceUrl('2'), traceUrl('4')])
  expect(observers[0].getCurrentResult().data?.state).toBe('available')

  // Cancelling an open read frees its slot as well.
  stops[1]()
  await settle()
  expect(urls.at(-1)).toBe(traceUrl('5'))
  expect(urls).toHaveLength(4)

  heldFrames.splice(0).forEach((send) => send())
  await settle()
  expect(
    [0, 3, 4].map((index) => observers[index].getCurrentResult().status)
  ).toEqual(['success', 'success', 'success'])
  expect(observers[2].getCurrentResult().data).toBeUndefined()
  stops.forEach((stop) => stop())
})
