import { createElement } from 'react'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { renderToStaticMarkup } from 'react-dom/server'
import { expect, test } from 'vitest'
import { taskAttemptTraceQuery, taskHistoryQuery } from './data'
import { TaskAttemptDetail } from './task-attempt-detail'
import {
  formatAttemptDuration,
  formatRetryCountdown,
  groupAttempts,
} from './task-history'
import { TaskHistoryPanel } from './task-history-panel'
import {
  taskAttemptTraceSchema,
  taskHistorySchema,
  type TaskAttempt,
} from './types'

function attempt(
  number: number,
  outcome: TaskAttempt['outcome'],
  error = ''
): TaskAttempt {
  return {
    id: String(number),
    attempt: number,
    operation: 'sync',
    outcome,
    error,
    started_at: `2026-10-14T00:0${number}:00Z`,
    emitted_at: `2026-10-14T00:0${number}:01Z`,
    duration_ms: 1000,
    detail: { kind: 'none', truncated: false },
    trace: null,
  }
}

const runs = (list: TaskAttempt[]) =>
  groupAttempts(list).map((group) => group.attempts.map((item) => item.attempt))

const ref = { resourceType: 'dns_zone', resourceId: 'zone-1' }
const recordsKey = taskHistoryQuery(ref).queryKey
const panel = createElement(TaskHistoryPanel, { task: ref })
/** Parsed like the wire `history` event so degraded shapes are the real ones. */
const parseHistory = (value: object) => taskHistorySchema.parse(value)

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

test('identical errors never fold across a restarted attempt sequence', () => {
  expect(
    runs([
      { ...attempt(1, 'failed', 'timeout'), id: '101' },
      { ...attempt(3, 'failed', 'timeout'), id: '100' },
      { ...attempt(2, 'failed', 'timeout'), id: '99' },
      { ...attempt(1, 'failed', 'timeout'), id: '98' },
    ])
  ).toEqual([[1], [3, 2, 1]])
  expect(
    runs([attempt(5, 'failed', 'timeout'), attempt(3, 'failed', 'timeout')])
  ).toEqual([[5], [3]])
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
  const base = {
    operation: 'sync',
    error: '',
    started_at: '2026-10-14T00:00:00Z',
    emitted_at: '2026-10-14T00:00:01Z',
    duration_ms: 1000,
  }
  // Parsed like the wire snapshot so the degraded shapes are the real ones.
  const history = parseHistory({
    truncated: false,
    summary: {
      attempt_count: 4,
      failure_count: 2,
      first_failure_at: '',
      last_success_at: '',
    },
    list: [
      {
        ...base,
        id: '4',
        attempt: 4,
        outcome: 'cancelled',
        detail: { kind: 'none', truncated: false },
      },
      {
        ...base,
        id: '3',
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
        id: '2',
        attempt: 2,
        outcome: 'failed',
        error: 'broken detail',
        detail: { kind: 'dns_sync', truncated: false, created: [] },
      },
      {
        ...base,
        id: '1',
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
  client.setQueryData(recordsKey, history)
  const html = renderToStaticMarkup(
    createElement(QueryClientProvider, { client }, panel)
  )
  // Every attempt renders in one list, titled with its operation.
  for (const number of [4, 3, 2, 1])
    expect(html).not.toContain(`第 ${number} 次`)
  expect(html).toContain('aria-label="执行记录"')
  // Unknown outcome / phase / domain status show their raw values.
  expect(html).toContain('cancelled')
  expect(html).toContain('warmup')
  expect(html).toContain('pending_dns')
  // Only attempt 2 loses its detail; its error and the others stay intact.
  expect(html.split('明细无法识别').length - 1).toBe(1)
  expect(html).toContain('broken detail')
  expect(html).toContain('edge-1')
  expect(html).toContain('www.example.com')
  client.clear()
})

test('failed provider verifications show zones as not obtained, not zero', () => {
  const detail: TaskAttempt['detail'] = {
    kind: 'provider',
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
test('truncated detail keeps facts and generation errors without omission captions', () => {
  const detail = parseHistory({
    summary: {
      attempt_count: 1,
      failure_count: 0,
      first_failure_at: '',
      last_success_at: '',
    },
    list: [
      {
        ...attempt(1, 'completed'),
        detail: {
          kind: 'provider',
          truncated: true,
          provider: 'cloudflare',
          provider_kind: 'dns',
          zone_count: 12,
          zones: [],
          directory_url: '',
          external_account: '',
        },
      },
    ],
  }).list[0].detail
  const html = renderToStaticMarkup(
    createElement(TaskAttemptDetail, { detail, outcome: 'completed' })
  )
  expect(html).toContain('12 个')
  expect(html).not.toContain('省略')
  expect(detail).not.toHaveProperty('truncated')
  expect(
    renderToStaticMarkup(
      createElement(TaskAttemptDetail, {
        detail: { kind: 'none', truncated: true },
        outcome: 'failed',
      })
    )
  ).toContain('明细生成失败')
})

test('history and timeline show absolute times in the same console time, without captions', () => {
  const startedAt = '2026-10-14T14:16:44.123456+00'
  const history = parseHistory({
    truncated: true,
    summary: {
      attempt_count: 1,
      failure_count: 1,
      first_failure_at: startedAt,
      last_success_at: '',
    },
    list: [
      {
        id: '48213',
        attempt: 1,
        operation: 'configure',
        outcome: 'failed',
        error: 'apply failed',
        started_at: startedAt,
        emitted_at: '2026-10-14T14:16:49.150034+00',
        duration_ms: 5026,
        detail: { kind: 'none', truncated: false },
        trace: {
          state: 'available',
          attempt_id: '48213',
          step_count: 1,
          steps: [{ code: 'validate', label: '校验配置', outcome: 'failed' }],
        },
      },
    ],
  })
  const trace = taskAttemptTraceSchema.parse({
    state: 'available',
    steps: [
      {
        code: 'validate',
        label: '校验配置',
        outcome: 'failed',
        logs: [
          {
            at: '2026-10-14T14:16:49.138000+00',
            level: 'error',
            message: 'nginx -t failed',
          },
        ],
      },
    ],
  })
  const client = new QueryClient()
  client.setQueryData(recordsKey, history)
  client.setQueryData(taskAttemptTraceQuery(ref, '48213').queryKey, trace)
  // Machine-readable attributes dropped: only formatted times remain.
  const html = renderToStaticMarkup(
    createElement(QueryClientProvider, { client }, panel)
  ).replace(/dateTime="[^"]*"/g, '')
  // 14:16:44 UTC is 22:16:44 console time in the history and the timeline.
  expect(html).toContain('2026-10-14 22:16:44')
  expect(html).toContain('22:16:49.138')
  expect(html).toContain('nginx -t failed')
  expect(html).not.toContain('14:16:4')
  expect(html).not.toMatch(/UTC/)
  // Values only: no explanatory captions, toggles or prefixes.
  for (const caption of [
    '保留 7 天',
    '没有待执行的重试',
    '没有失败',
    '尚未成功',
    '开始于',
    '计划于',
    '更早的',
    '执行详情',
    '展开全文',
    '仅列出',
    '部分条目已省略',
  ])
    expect(html).not.toContain(caption)
  expect(html).not.toContain('仅列出最近 1 次')
  client.clear()
})

test('a folded failure run shows only its newest timeline until opened', () => {
  const history = parseHistory({
    truncated: false,
    summary: {
      attempt_count: 3,
      failure_count: 3,
      first_failure_at: '',
      last_success_at: '',
    },
    list: [3, 2, 1].map((number) => ({
      ...attempt(number, 'failed', 'timeout'),
      trace: {
        state: 'available',
        attempt_id: String(48_000 + number),
        step_count: 1,
        steps: [{ code: 'find_zone', label: `查找区域 ${number}` }],
      },
    })),
  })
  const client = new QueryClient()
  client.setQueryData(recordsKey, history)
  const html = renderToStaticMarkup(
    createElement(QueryClientProvider, { client }, panel)
  )
  expect(html).not.toContain('第 1–3 次')
  expect(html).toContain('>同步</h3>')
  expect(html).toContain('展开 3 次尝试')
  expect(html).toContain('查找区域 3')
  expect(html).not.toContain('查找区域 2')
  expect(html).not.toContain('查找区域 1')
  client.clear()
})
