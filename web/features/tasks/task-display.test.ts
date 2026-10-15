import { createElement } from 'react'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { renderToStaticMarkup } from 'react-dom/server'
import { expect, test } from 'vitest'
import { taskAttemptTraceQuery } from './data'
import { tasksSearchSchema } from './search'
import { SubtaskLinkContext } from './subtask-link-context'
import { AttemptTimeline } from './task-attempt-trace'
import { SubtaskList } from './task-detail-sheet'
import {
  describeOperationStatus,
  formatOperationSummary,
  operationHref,
  operationTitle,
  operationTarget,
  subtaskNote,
  subtaskRows,
  subtaskTitle,
} from './task-display'
import {
  operationDetailSchema,
  subtaskHistorySchema,
  taskAttemptTraceSchema,
  type Subtask,
} from './types'

const subtask: Subtask = {
  id: 'sub-1',
  parent_id: '',
  kind: 'node_publish',
  resource_type: 'node',
  resource_id: 'node-1',
  resource_name: '北京',
  resource_deleted: false,
  status: 'completed',
  warning: '',
  node_offline: false,
  created_at: '2026-10-15T08:00:00.000000+00',
  running_since: '',
  finished_at: '2026-10-15T08:00:02.400000+00',
  duration_ms: 2400,
  attempt_count: 1,
  failure_count: 0,
  recovered: false,
  next_attempt_at: '',
  error: '',
}

test('titles name the action or kind and the object; unknown codes stay raw', () => {
  expect(
    operationTitle({ action: 'website.update', target_name: '品牌官网' })
  ).toBe('保存网站配置 · 品牌官网')
  expect(operationTitle({ action: 'website.purge', target_name: 'x' })).toBe(
    'website.purge · x'
  )
  expect(subtaskTitle(subtask)).toBe('节点发布 · 北京')
  expect(subtaskTitle({ kind: 'cache_purge', resource_name: 'y' })).toBe(
    'cache_purge · y'
  )
  expect(describeOperationStatus('partial_failed')).toEqual({
    label: '部分失败',
    tone: 'danger',
  })
  expect(describeOperationStatus('queued').tone).toBe('default')
  expect(describeOperationStatus('future')).toEqual({
    label: 'future',
    tone: 'default',
  })
  expect(operationHref('op 1')).toBe('/tasks?task=op+1')
})

test('the change summary is one short line', () => {
  expect(
    formatOperationSummary({
      target_type: 'website',
      summary: { sections: ['domains', 'https', 'future'] },
    })
  ).toBe('变更：域名、HTTPS、future')
  expect(
    formatOperationSummary({
      target_type: 'cluster',
      summary: { sections: ['dns'] },
    })
  ).toBe('变更：接入域名')
  expect(
    formatOperationSummary({
      target_type: 'dns_zone',
      summary: { record_counts: { created: 2, updated: 1, deleted: 0 } },
    })
  ).toBe('新增 2 · 修改 1')
  expect(
    formatOperationSummary({
      target_type: 'provider',
      summary: { credentials_changed: true },
    })
  ).toBe('已更换凭据')
  expect(formatOperationSummary({ target_type: 'dns_zone', summary: {} })).toBe(
    ''
  )
})

test('targets link to their resource pages; providers open the tab of target_kind', () => {
  const target = (
    target_type: string,
    target_id: string,
    target_name: string,
    extra: { target_deleted?: boolean; target_kind?: string } = {}
  ) =>
    operationTarget({
      target_type,
      target_id,
      target_name,
      target_deleted: false,
      ...extra,
    })
  expect(target('website', 'w 1', '官网')).toEqual({ href: '/websites/w%201' })
  expect(target('cluster', 'c1', '北方')?.href).toBe('/clusters/c1')
  expect(
    target('website', 'w1', '官网', { target_deleted: true })
  ).toBeUndefined()
  expect(
    target('provider', 'p1', 'ZeroSSL', { target_kind: 'certificate' })
  ).toEqual({
    href: '/providers',
    state: { providers: { tab: 'certificate', q: 'ZeroSSL' } },
  })
  // The name no longer decides the tab: only `target_kind` does.
  expect(
    target('provider', 'p2', 'zerossl', { target_kind: 'dns' })?.state
  ).toEqual({ providers: { tab: 'dns', q: 'zerossl' } })
  expect(target('provider', 'p3', 'x')?.state).toEqual({
    providers: { tab: 'dns', q: 'x' },
  })
  expect(target('gateway', 'g1', 'x')).toBeUndefined()
})

test('subtasks render in creation order with children one level in', () => {
  const rows = subtaskRows([
    { ...subtask, id: 'issue', kind: 'certificate_issue' },
    { ...subtask, id: 'publish' },
    { ...subtask, id: 'challenge', parent_id: 'issue' },
    { ...subtask, id: 'cleanup', parent_id: 'issue' },
    { ...subtask, id: 'grandchild', parent_id: 'challenge' },
    { ...subtask, id: 'orphan', parent_id: 'gone' },
  ])
  expect(rows.map((row) => [row.subtask.id, row.depth])).toEqual([
    ['issue', 0],
    ['challenge', 1],
    ['grandchild', 1],
    ['cleanup', 1],
    ['publish', 0],
    ['orphan', 0],
  ])
})

test('each subtask has at most one extra note, in priority order', () => {
  expect(subtaskNote(subtask)).toBeNull()
  expect(
    subtaskNote({
      ...subtask,
      status: 'retrying',
      next_attempt_at: '2026-10-15T08:01:00.000000+00',
    })
  ).toEqual({ kind: 'retry', at: '2026-10-15T08:01:00.000000+00' })
  expect(
    subtaskNote({ ...subtask, status: 'retrying', node_offline: true })
  ).toEqual({ kind: 'offline' })
  expect(
    subtaskNote({
      ...subtask,
      status: 'merged',
      merged_into: {
        operation_id: 'op-2',
        subtask_id: 'sub-9',
        action: 'website.update',
        target_name: '品牌官网',
        status: 'running',
      },
    })
  ).toEqual({
    kind: 'merged',
    operationId: 'op-2',
    subtaskId: 'sub-9',
    label: '保存网站配置 · 品牌官网',
  })
  expect(
    subtaskNote({
      ...subtask,
      status: 'cancelled',
      closed_by: {
        operation_id: 'op-3',
        action: 'website.delete',
        target_name: '品牌官网',
      },
    })
  ).toEqual({ kind: 'closed', label: '删除网站 · 品牌官网', verb: '取消' })
  // Merged without a receiver: 「因 … 合并」; neither object: no note.
  expect(
    subtaskNote({
      ...subtask,
      status: 'merged',
      closed_by: {
        operation_id: 'op-4',
        action: 'website.update',
        target_name: '品牌官网',
      },
    })
  ).toEqual({ kind: 'closed', label: '保存网站配置 · 品牌官网', verb: '合并' })
  expect(subtaskNote({ ...subtask, status: 'merged' })).toBeNull()
  expect(subtaskNote({ ...subtask, status: 'cancelled' })).toBeNull()
  expect(
    subtaskNote({
      ...subtask,
      status: 'attention_required',
      warning: 'dns_conflict',
    })
  ).toEqual({ kind: 'warning', label: '存在冲突记录' })
  expect(
    subtaskNote({
      ...subtask,
      status: 'attention_required',
      warning: 'future_warning',
    })
  ).toEqual({ kind: 'warning', label: 'future_warning' })
  expect(
    subtaskNote({ ...subtask, recovered: true, failure_count: 2 })
  ).toEqual({ kind: 'recovered', failures: 2 })
})

test('subtask rows show status, the fixed note slot and duration on one line', () => {
  const operation = operationDetailSchema.parse({
    id: 'op-1',
    action: 'website.update',
    target_type: 'website',
    target_id: 'w1',
    target_name: '品牌官网',
    target_deleted: false,
    actor_type: 'user',
    actor_name: 'QA',
    created_at: '2026-10-15T08:00:00.000000+00',
    updated_at: '2026-10-15T08:00:00.000000+00',
    finished_at: '',
    status: 'retrying',
    subtask_count: 4,
    settled_count: 2,
    failed_count: 1,
    next_attempt_at: '',
    error: '',
    summary: {},
    subtasks: [
      { ...subtask, kind: 'dns_sync', resource_name: 'a-z.xin' },
      {
        ...subtask,
        id: 'sub-2',
        status: 'retrying',
        finished_at: '',
        duration_ms: 0,
        next_attempt_at: '2026-10-15T08:01:00.000000+00',
      },
      {
        ...subtask,
        id: 'sub-3',
        parent_id: 'sub-1',
        kind: 'website_check',
        resource_name: '品牌官网',
        status: 'attention_required',
        warning: 'domain_unverified',
      },
      { ...subtask, id: 'sub-4', recovered: true, failure_count: 3 },
    ],
  })
  const html = renderToStaticMarkup(
    createElement(
      QueryClientProvider,
      { client: new QueryClient() },
      createElement(SubtaskList, {
        operation,
        expanded: null,
        onExpandedChange: () => undefined,
        streamFailed: false,
      })
    )
  )
  expect(html).toContain('DNS 同步 · a-z.xin')
  expect(html).toContain('节点发布 · 北京')
  expect(html).toContain('重试中')
  expect(html).toMatch(/重试|即将重试/)
  expect(html).toContain('域名未生效')
  expect(html).toContain('失败 3 次后成功')
  expect(html).toContain('2.4 秒')
  // The child row is indented one level.
  expect(html.match(/ps-6/g)).toHaveLength(1)
  // Collapsed rows read no execution records.
  expect(html).not.toContain('执行记录')
})

test('a trace step links to another subtask of the open task only', () => {
  const summary = subtaskHistorySchema.parse({
    subtask_id: 'sub-1',
    summary: {
      attempt_count: 1,
      failure_count: 0,
      first_failure_at: '',
      last_success_at: '',
      next_attempt_at: '',
    },
    list: [
      {
        attempt: 1,
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
          step_count: 1,
          steps: [{ code: 'dns_challenge', label: '写入验证记录' }],
        },
      },
    ],
  }).list[0].trace
  const client = new QueryClient()
  client.setQueryData(
    taskAttemptTraceQuery('op-1', '7').queryKey,
    taskAttemptTraceSchema.parse({
      state: 'available',
      steps: [
        {
          code: 'dns_challenge',
          label: '写入验证记录',
          related_task: { subtask_id: 'sub-2', label: 'DNS 验证记录' },
        },
        {
          code: 'cleanup',
          label: '清理验证记录',
          related_task: { subtask_id: 'elsewhere', label: '其他任务' },
        },
      ],
    })
  )
  const render = (context: boolean) =>
    renderToStaticMarkup(
      createElement(
        QueryClientProvider,
        { client },
        createElement(
          SubtaskLinkContext,
          {
            value: context
              ? { has: (id: string) => id === 'sub-2', open: () => undefined }
              : null,
          },
          createElement(AttemptTimeline, {
            operationId: 'op-1',
            trace: summary,
          })
        )
      )
    )
  const html = render(true)
  expect(html).toContain('关联：DNS 验证记录')
  expect(html).not.toContain('其他任务')
  expect(render(false)).not.toContain('关联')
})

test('search schema keeps valid filters and the open task, drops invalid ones', () => {
  expect(
    tasksSearchSchema.parse({
      status: 'attention',
      action: 'website.update',
      target_type: 'certificate',
      actor: 'system',
      days: 7,
      task: 'op-1',
    })
  ).toMatchObject({
    status: 'attention',
    action: 'website.update',
    target_type: 'certificate',
    actor: 'system',
    days: 7,
    task: 'op-1',
  })
  expect(
    tasksSearchSchema.parse({
      status: 'recovered',
      action: 'website.purge',
      target_type: 'gateway',
      actor: 'someone',
      days: 3,
      task: '',
    })
  ).toEqual({})
})
