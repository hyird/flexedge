import { createElement } from 'react'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { renderToStaticMarkup } from 'react-dom/server'
import { expect, test } from 'vitest'
import { describeStatus } from '@/lib/status'
import { taskAttemptTraceQuery } from './data'
import { tasksSearchSchema } from './search'
import { AttemptTimeline } from './task-attempt-trace'
import {
  parseTaskKey,
  recentTaskOrder,
  taskHref,
  taskNote,
  taskStatuses,
  taskTarget,
  taskTitle,
} from './task-display'
import { taskHistorySchema, taskAttemptTraceSchema, type Task } from './types'

const task: Task = {
  resource_type: 'dns_zone',
  resource_id: 'zone-1',
  resource_kind: '',
  name: 'a-z.xin',
  operation: 'sync',
  status: 'completed',
  error: '',
  next_attempt_at: '',
  updated_at: '2026-10-15T08:00:00.000000+00',
  last_success_at: '2026-10-15T08:00:00.000000+00',
  attempt_count: 1,
  failure_count: 0,
  node_offline: false,
}

test('titles name the task type and the object; unknown codes stay raw', () => {
  expect(taskTitle(task)).toBe('DNS同步 · a-z.xin')
  expect(taskTitle({ ...task, operation: 'delete' })).toBe(
    '删除托管域 · a-z.xin'
  )
  for (const [resource_type, operation, label] of [
    ['website', 'apply', '网站检查'],
    ['certificate', 'issue', '证书签发'],
    ['certificate', 'renew', '证书续期'],
    ['provider', 'verify', '服务商验证'],
    ['node', 'publish', '节点配置下发'],
    ['gateway', 'purge', 'gateway'],
  ])
    expect(taskTitle({ resource_type, operation, name: '对象' })).toBe(
      `${label} · 对象`
    )
  expect(describeStatus(taskStatuses, 'queued').tone).toBe('default')
  expect(describeStatus(taskStatuses, 'future')).toEqual({
    label: 'future',
    tone: 'default',
  })
})

test('a task is addressed as <resource_type>:<resource_id> in the URL', () => {
  const ref = { resourceType: 'node', resourceId: 'node-1' }
  expect(taskHref(ref)).toBe('/tasks?task=node%3Anode-1')
  expect(parseTaskKey('node:node-1')).toEqual(ref)
  for (const value of ['node', 'gateway:x', 'node:', ':x', 'node:a b'])
    expect(parseTaskKey(value)).toBeUndefined()
})

test('tasks link to their resource pages; providers open the list of resource_kind searched for the account', () => {
  expect(
    taskTarget({ ...task, resource_type: 'website', resource_id: 'w 1' })
  ).toEqual({ href: '/websites', state: { website: { id: 'w 1' } } })
  expect(taskTarget(task)).toEqual({ href: '/dns-zones/zone-1' })
  expect(
    taskTarget({ ...task, resource_type: 'node', resource_id: 'n1' })
  ).toEqual({ href: '/nodes/n1' })
  expect(
    taskTarget({
      ...task,
      resource_type: 'provider',
      resource_kind: 'certificate',
      name: 'ZeroSSL',
    })
  ).toEqual({
    href: '/providers?q=ZeroSSL',
    state: { providers: { tab: 'certificate' } },
  })
  expect(
    taskTarget({ ...task, resource_type: 'provider', resource_kind: 'dns' })
  ).toEqual({
    href: '/providers?q=a-z.xin',
    state: { providers: { tab: 'dns' } },
  })
  expect(taskTarget({ ...task, resource_type: 'gateway' })).toBeUndefined()
})

test('each task has at most one note, in priority order', () => {
  expect(taskNote(task)).toBeNull()
  const retrying = {
    ...task,
    status: 'retrying',
    failure_count: 2,
    next_attempt_at: '2026-10-15T08:01:00.000000+00',
  }
  expect(taskNote(retrying)).toEqual({
    kind: 'retry',
    at: '2026-10-15T08:01:00.000000+00',
  })
  expect(taskNote({ ...retrying, node_offline: true })).toEqual({
    kind: 'offline',
  })
  expect(taskNote({ ...task, status: 'completed' })).toBeNull()
  expect(taskNote({ ...task, status: 'failed' })).toBeNull()
})

test('recent tasks use execution time regardless of status', () => {
  const at = (minute: number) => `2026-10-15T08:0${minute}:00.000000+00`
  const list = [
    { ...task, resource_id: 'old-done', updated_at: at(1) },
    { ...task, resource_id: 'new-done', updated_at: at(5) },
    {
      ...task,
      resource_id: 'old-queued',
      status: 'queued',
      updated_at: at(0),
    },
    {
      ...task,
      resource_id: 'new-running',
      status: 'running',
      updated_at: at(3),
    },
  ]
  expect(recentTaskOrder(list).map((item) => item.resource_id)).toEqual([
    'new-done',
    'new-running',
    'old-done',
    'old-queued',
  ])
})

test('search schema keeps valid filters and the open task, drops invalid ones', () => {
  expect(
    tasksSearchSchema.parse({
      status: 'attention',
      type: 'certificate',
      task: 'certificate:cert-1',
    })
  ).toMatchObject({
    status: 'attention',
    type: 'certificate',
    task: 'certificate:cert-1',
  })
  expect(tasksSearchSchema.parse({ status: 'failed' }).status).toBe('failed')
  expect(
    tasksSearchSchema.parse({
      status: 'merged',
      type: 'cluster',
      task: 'op-1',
      action: 'website.update',
    })
  ).toEqual({})
})

test('a trace step of another task links to that task', () => {
  const summary = taskHistorySchema.parse({
    summary: {
      attempt_count: 1,
      failure_count: 0,
      first_failure_at: '',
      last_success_at: '',
    },
    list: [
      {
        id: '7',
        attempt: 1,
        operation: 'issue',
        outcome: 'completed',
        error: '',
        started_at: '',
        emitted_at: '',
        duration_ms: 0,
        next_attempt_at: '',
        detail: { kind: 'none', truncated: false },
        trace: {
          state: 'available',
          attempt_id: '7',
          step_count: 2,
          steps: [{ code: 'dns_challenge', label: '写入DNS验证记录' }],
        },
      },
    ],
  }).list[0].trace
  const ref = { resourceType: 'certificate', resourceId: 'cert-1' }
  const client = new QueryClient()
  client.setQueryData(
    taskAttemptTraceQuery(ref, '7').queryKey,
    taskAttemptTraceSchema.parse({
      state: 'available',
      steps: [
        {
          code: 'dns_challenge',
          label: '写入DNS验证记录',
          related_task: {
            resource_type: 'dns_zone',
            resource_id: 'zone-1',
            label: 'DNS同步 · a-z.xin',
          },
        },
        { code: 'validate', label: '证书颁发机构验证' },
      ],
    })
  )
  const html = renderToStaticMarkup(
    createElement(
      QueryClientProvider,
      { client },
      createElement(AttemptTimeline, { task: ref, trace: summary })
    )
  )
  expect(html).toContain('关联任务：DNS同步 · a-z.xin')
  expect(html).toContain('href="/tasks?task=dns_zone%3Azone-1"')
  expect(html).toContain('证书颁发机构验证')
  expect(html.match(/关联任务/g)).toHaveLength(1)
})
