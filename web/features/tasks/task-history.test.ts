import { createElement, type ReactElement } from 'react'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { renderToStaticMarkup } from 'react-dom/server'
import { expect, test, vi } from 'vitest'
import { taskAttemptTraceQuery, taskHistoryQuery } from './data'
import { TaskAttemptDetail } from './task-attempt-detail'
import { AttemptTraceView } from './task-attempt-trace'
import {
  certificateProgress,
  certificateStages,
  formatAttemptDuration,
  formatRetryCountdown,
  groupAttempts,
} from './task-history'
import { TaskHistoryPanel } from './task-history-panel'
import {
  taskAttemptTraceSchema,
  taskHistorySchema,
  type Task,
  type TaskAttempt,
} from './types'

// The shared display clock has no server snapshot; server rendering only
// needs the label.
vi.mock('@/components/relative-time', () => ({
  RelativeTime: ({ value }: { value?: string }) =>
    createElement('time', null, value),
}))

function attempt(
  number: number,
  outcome: TaskAttempt['outcome'],
  error = ''
): TaskAttempt {
  return {
    attempt: number,
    outcome,
    error,
    started_at: `2026-10-14T00:0${number}:00Z`,
    emitted_at: `2026-10-14T00:0${number}:01Z`,
    duration_ms: 1000,
    next_attempt_at: '',
    detail: { kind: 'none', truncated: false },
    trace: null,
  }
}

const runs = (list: TaskAttempt[]) =>
  groupAttempts(list).map((group) => group.attempts.map((item) => item.attempt))

test('consecutive identical failures collapse into one run, newest first', () => {
  const list = [
    attempt(7, 'failed', 'timeout'),
    attempt(6, 'failed', 'timeout'),
    attempt(5, 'failed', 'timeout'),
    attempt(4, 'failed', 'refused'),
    attempt(3, 'completed'),
    attempt(2, 'failed', 'refused'),
    attempt(1, 'completed'),
  ]
  expect(runs(list)).toEqual([[7, 6, 5], [4], [3], [2], [1]])
})

test('failures without an error text and successes never collapse', () => {
  expect(
    runs([
      attempt(4, 'failed'),
      attempt(3, 'failed'),
      attempt(2, 'completed'),
      attempt(1, 'completed'),
    ])
  ).toEqual([[4], [3], [2], [1]])
  expect(runs([])).toEqual([])
})

test('durations are compact and unknown for records without a start time', () => {
  const at = (duration_ms: number, started_at = '2026-10-14T00:00:00Z') =>
    formatAttemptDuration({ started_at, duration_ms })
  expect(at(0)).toBe('0 毫秒')
  expect(at(878)).toBe('878 毫秒')
  expect(at(1000)).toBe('1 秒')
  expect(at(12_480)).toBe('12.4 秒')
  expect(at(60_000)).toBe('1 分')
  expect(at(125_000)).toBe('2 分 5 秒')
  expect(at(3_780_000)).toBe('1 小时 3 分')
  expect(at(7_200_000)).toBe('2 小时')
  expect(at(0, '')).toBe('—')
})

test('certificate progress stops at the failed stage in execution order', () => {
  expect(certificateStages.map(([id]) => id)).toEqual([
    'prepare',
    'account',
    'order',
    'authorization',
    'dns_challenge',
    'validation',
    'cleanup',
    'finalize',
    'download',
    'persist',
  ])
  expect(certificateProgress('completed')).toEqual({
    current: 10,
    failedAt: -1,
  })
  expect(certificateProgress('prepare')).toEqual({ current: 0, failedAt: 0 })
  expect(certificateProgress('validation')).toEqual({ current: 5, failedAt: 5 })
  // Unknown stages (newer backend) show no stepper position.
  expect(certificateProgress('future_stage')).toEqual({
    current: 0,
    failedAt: -1,
  })
})

test('retry countdown ticks in seconds under a minute and never goes negative', () => {
  const now = Date.parse('2026-10-14T14:00:00Z')
  expect(formatRetryCountdown(now + 14_200, now)).toBe('15 秒后')
  expect(formatRetryCountdown(now + 1, now)).toBe('1 秒后')
  expect(formatRetryCountdown(now + 59_000, now)).toBe('59 秒后')
  expect(formatRetryCountdown(now + 5 * 60_000, now)).toBe('5分钟后')
  expect(formatRetryCountdown(now, now)).toBe('已到重试时间')
  expect(formatRetryCountdown(now - 90_000, now)).toBe('已到重试时间')
})

test('unknown values and a malformed detail only affect their own attempt', () => {
  const task: Task = {
    id: 'task-1',
    resource_type: 'website',
    resource_id: 'website-1',
    name: 'Website',
    operation: 'apply',
    version: 3,
    status: 'failed',
    error: '',
    failures: 3,
    updated_at: '2026-10-14T00:00:00Z',
    next_attempt_at: '',
  }
  const base = {
    error: '',
    started_at: '2026-10-14T00:00:00Z',
    emitted_at: '2026-10-14T00:00:01Z',
    duration_ms: 1000,
    next_attempt_at: '',
  }
  // Parsed like the wire snapshot so the degraded shapes are the real ones.
  const history = taskHistorySchema.parse({
    truncated: false,
    summary: {
      attempt_count: 4,
      failure_count: 2,
      first_failure_at: '',
      last_success_at: '',
      next_attempt_at: '',
    },
    list: [
      {
        ...base,
        attempt: 4,
        outcome: 'cancelled',
        detail: { kind: 'none', truncated: false },
      },
      {
        ...base,
        attempt: 3,
        outcome: 'failed',
        error: 'apply failed',
        detail: {
          kind: 'node_apply',
          truncated: false,
          node_id: 'node-1',
          node_name: 'edge-1',
          agent_version: '1.2.0',
          source: 'apply_result',
          result: 'failed',
          phase: 'warmup',
          error_code: 'E1',
          retryable: true,
        },
      },
      {
        ...base,
        attempt: 2,
        outcome: 'failed',
        error: 'broken detail',
        detail: { kind: 'dns_sync', truncated: false, created: [] },
      },
      {
        ...base,
        attempt: 1,
        outcome: 'completed',
        detail: {
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
        },
      },
    ],
  })
  const client = new QueryClient()
  client.setQueryData(taskHistoryQuery(task).queryKey, history)
  const html = renderToStaticMarkup(
    createElement(
      QueryClientProvider,
      { client },
      createElement(TaskHistoryPanel, { task })
    )
  )
  // Every attempt renders, in a plain list without timeline markers.
  for (const number of [4, 3, 2, 1]) expect(html).toContain(`第 ${number} 次`)
  expect(html).toContain('aria-label="执行记录"')
  expect(html).not.toContain('timeline')
  // Unknown outcome / phase / domain status show their raw values.
  expect(html).toContain('cancelled')
  expect(html).toContain('warmup')
  expect(html).toContain('pending_dns')
  // Only attempt 2 loses its detail; its error and the others stay intact.
  expect(html.split('明细无法显示').length - 1).toBe(1)
  expect(html).toContain('broken detail')
  expect(html).toContain('edge-1')
  expect(html).toContain('www.example.com')
  client.clear()
})

test('failed provider verifications show zones as not obtained, not zero', () => {
  const detail: TaskAttempt['detail'] = {
    kind: 'provider',
    truncated: false,
    provider: 'cloudflare',
    provider_kind: 'dns',
    zone_count: 0,
    zones: [],
    directory_url: '',
    external_account: '',
  }
  const render = (outcome: string) =>
    renderToStaticMarkup(createElement(TaskAttemptDetail, { detail, outcome }))
  // A completed verification that sees no zones genuinely reports zero.
  expect(render('completed')).toContain('0 个')
  for (const outcome of ['failed', 'cancelled']) {
    const html = render(outcome)
    expect(html).toContain('未取得')
    expect(html).not.toContain('0 个')
  }
})

test('history and trace show absolute times in the same console time', () => {
  const task: Task = {
    id: 'task-1',
    resource_type: 'node',
    resource_id: 'node-1',
    name: 'edge-1',
    operation: 'apply',
    version: 7,
    status: 'retrying',
    error: '',
    failures: 1,
    updated_at: '2026-10-14T14:16:49+00',
    next_attempt_at: '',
  }
  const startedAt = '2026-10-14T14:16:44.123456+00'
  const history = taskHistorySchema.parse({
    truncated: false,
    summary: {
      attempt_count: 1,
      failure_count: 1,
      first_failure_at: startedAt,
      last_success_at: '',
      next_attempt_at: '2026-10-14T14:17:44+00',
    },
    list: [
      {
        attempt: 1,
        outcome: 'failed',
        error: 'apply failed',
        started_at: startedAt,
        emitted_at: '2026-10-14T14:16:49.150034+00',
        duration_ms: 5026,
        next_attempt_at: '2026-10-14T14:17:44+00',
        detail: { kind: 'none', truncated: false },
        trace: {
          state: 'available',
          attempt_id: '48213',
          step_count: 0,
          call_count: 0,
          log_count: 0,
          truncated: false,
          failed_step: null,
          steps: [],
        },
      },
    ],
  })
  const trace = taskAttemptTraceSchema.parse({
    attempt_id: '48213',
    task_id: task.id,
    kind: 'node_apply',
    state: 'available',
    attempt: 1,
    started_at: startedAt,
    duration_ms: 5026,
    worker_type: 'web',
    steps: [],
  })
  const client = new QueryClient()
  client.setQueryData(taskHistoryQuery(task).queryKey, history)
  client.setQueryData(taskAttemptTraceQuery(task.id, '48213').queryKey, trace)
  // RelativeTime is mocked to echo its raw value; drop those echoes and the
  // machine-readable attributes so only formatted absolute times remain.
  const visible = (element: ReactElement) =>
    renderToStaticMarkup(
      createElement(QueryClientProvider, { client }, element)
    )
      .replace(/<time>[^<]*<\/time>/g, '')
      .replace(/dateTime="[^"]*"/g, '')
  const historyHtml = visible(createElement(TaskHistoryPanel, { task }))
  const traceHtml = visible(
    createElement(AttemptTraceView, { taskId: task.id, attemptId: '48213' })
  )
  // 14:16:44 UTC is 22:16:44 console time in both views.
  expect(historyHtml).toContain('2026-10-14 22:16:44')
  expect(historyHtml).toContain('2026-10-14 22:17:44')
  expect(traceHtml).toContain('2026-10-14 22:16:44.123')
  expect(traceHtml).toContain('（UTC+8）')
  for (const html of [historyHtml, traceHtml]) {
    expect(html).not.toContain('14:16:44')
    expect(html).not.toMatch(/UTC(?!\+8)/)
  }
  client.clear()
})
