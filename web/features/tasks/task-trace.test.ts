import { createElement, type ReactElement } from 'react'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { renderToStaticMarkup } from 'react-dom/server'
import { expect, test } from 'vitest'
import { taskAttemptTraceQuery } from './data'
import {
  AttemptTrace,
  AttemptTraceView,
  NodeMessageDetail,
  RedactedText,
} from './task-attempt-trace'
import {
  formatTraceClock,
  formatTraceTime,
  formatTraceDuration,
  httpStatusTone,
  isConfigVersionField,
  splitRedacted,
} from './task-trace'
import {
  invalidItem,
  taskAttemptTraceSchema,
  taskHistorySchema,
  type TaskAttemptTrace,
  type TaskAttemptTraceSummary,
} from './types'

// build/task-trace/contract.md §1: history item summary.
const summarySample = {
  state: 'available',
  attempt_id: '48213',
  step_count: 6,
  call_count: 3,
  log_count: 4,
  truncated: false,
  failed_step: { code: 'find_zone', label: '查找远程托管区域' },
  steps: [
    {
      code: 'load_state',
      label: '加载同步状态',
      outcome: 'completed',
      duration_ms: 9,
      call_count: 0,
      count: 1,
    },
    {
      code: 'open_credential',
      label: '解密凭据',
      outcome: 'completed',
      duration_ms: 0,
      call_count: 0,
      count: 1,
    },
    {
      code: 'validate_ttl',
      label: '校验 TTL',
      outcome: 'completed',
      duration_ms: 0,
      call_count: 0,
      count: 1,
    },
    {
      code: 'find_zone',
      label: '查找远程托管区域',
      outcome: 'failed',
      duration_ms: 5012,
      call_count: 1,
      count: 1,
    },
    {
      code: 'record_failure',
      label: '记录失败',
      outcome: 'completed',
      duration_ms: 4,
      call_count: 0,
      count: 1,
    },
  ],
}
// §2.4 http, §2.6 dns, §2.7 node_message and §2.8 log samples.
const httpSample = {
  method: 'GET',
  url: 'https://alidns.aliyuncs.com/?AccessKeyId=[REDACTED]&Action=DescribeDomainRecords&DomainName=example.com&Format=JSON&PageNumber=1&PageSize=100&Signature=[REDACTED]&SignatureMethod=HMAC-SHA1&SignatureNonce=[REDACTED]&SignatureVersion=1.0&Timestamp=2026-10-14T14%3A16%3A44Z&Version=2015-01-09',
  scheme: 'https',
  host: 'alidns.aliyuncs.com',
  path: '/',
  query: [
    { name: 'AccessKeyId', value: '[REDACTED]', redacted: true },
    { name: 'Action', value: 'DescribeDomainRecords', redacted: false },
  ],
  request_headers: [
    { name: 'accept', value: 'application/json', redacted: false },
  ],
  request_body: null,
  status: 400,
  transport_error: '',
  response_headers: [
    {
      name: 'content-type',
      value: 'application/json;charset=utf-8',
      redacted: false,
    },
    { name: 'x-acs-request-id', value: '5E7C…', redacted: false },
  ],
  response_body: {
    content_type: 'application/json',
    format: 'json',
    bytes: 312,
    excerpt:
      '{\n  "Code": "SignatureDoesNotMatch",\n  "HostId": "alidns.aliyuncs.com",\n  "Message": "… [REDACTED] …",\n  "RequestId": "5E7C…"\n}',
    truncated: false,
    omitted_reason: '',
  },
}
const dnsSample = {
  resolver: 'dns.alidns.com',
  transport: 'doh_json',
  name: 'flexedge-1a2b3c4d.example.com',
  type: 'CNAME',
  rcode: 'NOERROR',
  answers: [
    {
      name: 'flexedge-1a2b3c4d.example.com',
      type: 'CNAME',
      ttl: 600,
      data: 'edge-hk.cdn.example.net',
    },
  ],
  truncated: false,
}
const nodeSample = {
  direction: 'received',
  message: 'ApplyResult',
  request_id: 'release-probe-3',
  object_count: 0,
  bytes: 214,
  fields: [
    { name: 'applied', value: 'false' },
    { name: 'failed_phase', value: 'validate' },
  ],
}
const logSample = {
  at: '2026-10-14T14:16:49.138000+00',
  offset_ms: 5015,
  level: 'warning',
  message: 'Cloudflare API 连接失败，15 秒后重试',
  repeat_count: 1,
}
// §2.3 call.
const callSample = {
  index: 0,
  kind: 'http',
  operation: 'cloudflare.find_zone',
  subject: 'example.com',
  started_at: '2026-10-14T14:16:44.135210+00',
  offset_ms: 12,
  duration_ms: 5003,
  outcome: 'transport_error',
  repeat_count: 1,
  repeat_total_ms: 5003,
  last_started_at: '2026-10-14T14:16:44.135210+00',
  provider_request_id: '',
  provider_code: '',
  error: 'connect_failed',
  http: {
    ...httpSample,
    status: 0,
    transport_error: 'connect_failed',
    response_body: null,
  },
  dns: null,
  node_message: null,
}
// §2.2 step.
const stepSample = {
  index: 3,
  code: 'find_zone',
  label: '查找远程托管区域',
  subject: 'example.com',
  started_at: '2026-10-14T14:16:44.135001+00',
  offset_ms: 12,
  duration_ms: 5012,
  outcome: 'failed',
  error: 'Cloudflare API 连接失败',
  related_task: null,
  call_count: 1,
  omitted_calls: 0,
  calls: [callSample],
  logs: [logSample],
}
// §2.1 envelope.
const traceSample = {
  attempt_id: '48213',
  task_id: '6f3c',
  resource_type: 'dns_zone',
  resource_id: '9a1e',
  kind: 'dns_sync',
  state: 'available',
  attempt: 3,
  outcome: 'failed',
  error: 'Cloudflare API 连接失败',
  started_at: '2026-10-14T14:16:44.123456+00',
  emitted_at: '2026-10-14T14:16:49.150034+00',
  duration_ms: 5026,
  worker_type: 'dns-sync',
  capture: {
    truncated: false,
    degraded: false,
    bytes: 6120,
    omitted_steps: 0,
    omitted_calls: 0,
    omitted_logs: 0,
    omitted_body_bytes: 0,
    redactions: 2,
  },
  steps: [stepSample],
}
// §4: an abridged certificate step (absent fields take their empty value).
const abridgedStep = {
  index: 5,
  code: 'validation',
  label: '证书颁发机构验证',
  outcome: 'failed',
  error: 'DNS 验证失败：Incorrect TXT record',
  calls: [
    {
      kind: 'http',
      operation: 'acme.new_nonce',
      repeat_count: 31,
      http: {
        method: 'HEAD',
        url: 'https://acme-v02.api.letsencrypt.org/acme/new-nonce',
        status: 200,
        response_headers: [
          { name: 'replay-nonce', value: '[REDACTED]', redacted: true },
        ],
      },
    },
    {
      kind: 'http',
      operation: 'acme.authorization',
      repeat_count: 30,
      http: {
        method: 'POST',
        url: 'https://acme-v02.api.letsencrypt.org/acme/authz/1234/5678',
        request_body: {
          format: 'jws_decoded',
          excerpt: '{\n  "payload": "(POST-as-GET)"\n}',
        },
        status: 200,
        response_body: {
          format: 'json',
          excerpt: '{\n  "status": "invalid",\n  "token": "[REDACTED]"\n}',
        },
      },
    },
  ],
}
const attemptBase = {
  attempt: 3,
  outcome: 'failed',
  error: 'Cloudflare API 连接失败',
  started_at: '2026-10-14T14:16:44.123456+00',
  emitted_at: '2026-10-14T14:16:49.150034+00',
  duration_ms: 5026,
  next_attempt_at: '',
  detail: { kind: 'none', truncated: false },
}
const historyOf = (...list: unknown[]) => ({
  truncated: false,
  summary: {
    attempt_count: list.length,
    failure_count: list.length,
    first_failure_at: '',
    last_success_at: '',
    next_attempt_at: '',
  },
  list,
})

function parseTrace(value: unknown) {
  return taskAttemptTraceSchema.parse(value)
}

test('trace schemas parse the contract samples', () => {
  const history = taskHistorySchema.parse(
    historyOf({ ...attemptBase, trace: summarySample })
  )
  expect(history.list[0].trace).toEqual(summarySample)

  const trace = parseTrace({
    ...traceSample,
    steps: [
      stepSample,
      {
        ...stepSample,
        index: 4,
        calls: [
          { ...callSample, kind: 'dns', http: httpSample, dns: dnsSample },
          {
            ...callSample,
            kind: 'node_message',
            http: null,
            node_message: nodeSample,
          },
        ],
      },
      abridgedStep,
    ],
  })
  expect(trace.capture.redactions).toBe(2)
  const [first, second, abridged] = trace.steps
  expect(first).toEqual(stepSample)
  expect(second).toMatchObject({
    calls: [{ dns: dnsSample, http: httpSample }, { node_message: nodeSample }],
  })
  expect(abridged).toMatchObject({
    subject: '',
    offset_ms: 0,
    related_task: null,
    logs: [],
    calls: [
      {
        repeat_count: 31,
        outcome: '',
        http: {
          method: 'HEAD',
          query: [],
          request_body: null,
          transport_error: '',
        },
      },
      {
        repeat_count: 30,
        http: {
          request_body: { format: 'jws_decoded', bytes: 0, omitted_reason: '' },
          response_body: { format: 'json', truncated: false },
        },
      },
    ],
  })
  // `state: none` with an empty step list (attempt without a stored trace).
  expect(
    parseTrace({ ...traceSample, state: 'none', steps: [] }).steps
  ).toEqual([])
})

test('unknown values and malformed items degrade locally', () => {
  const trace = parseTrace({
    ...traceSample,
    worker_type: 'future-worker',
    steps: [
      {
        ...stepSample,
        outcome: 'paused',
        calls: [
          { ...callSample, kind: 'grpc', outcome: 'throttled', http: null },
          // Wrong types: only this call is lost.
          { ...callSample, http: 'oops' },
          { ...callSample, repeat_count: 0 },
          'not a call',
          {
            ...callSample,
            http: {
              ...httpSample,
              response_body: {
                ...httpSample.response_body,
                format: 'cbor',
                omitted_reason: 'quota',
              },
            },
          },
        ],
        logs: [
          { ...logSample, level: 'debug' },
          { ...logSample, offset_ms: -1 },
        ],
      },
      { ...stepSample, calls: 'none' },
      stepSample,
    ],
  })
  const [step, broken, intact] = trace.steps
  expect(step).toMatchObject({ outcome: 'paused' })
  if ('invalid' in step) throw new Error('step should parse')
  expect(
    step.calls.map((call) => ('invalid' in call ? 'invalid' : call.kind))
  ).toEqual(['grpc', 'invalid', 'invalid', 'invalid', 'http'])
  expect(step.calls[4]).toMatchObject({
    http: { response_body: { format: 'cbor', omitted_reason: 'quota' } },
  })
  expect(step.logs).toEqual([{ ...logSample, level: 'debug' }, invalidItem])
  expect(broken).toEqual(invalidItem)
  expect(intact).toEqual(stepSample)

  // History: a malformed summary hides only that attempt's trace; an absent
  // one (backend without traces) is null; summary items that do not parse
  // drop out of the strip.
  const history = taskHistorySchema.parse(
    historyOf(
      { ...attemptBase, trace: { ...summarySample, step_count: 'six' } },
      { ...attemptBase, attempt: 2 },
      {
        ...attemptBase,
        attempt: 1,
        trace: {
          ...summarySample,
          state: 'expired',
          steps: [summarySample.steps[0], { code: 7 }],
        },
      }
    )
  )
  expect(history.list.map((item) => item.trace)).toEqual([
    invalidItem,
    null,
    { ...summarySample, state: 'expired', steps: [summarySample.steps[0]] },
  ])
  // The attempt envelope itself stays strict.
  expect(() =>
    taskAttemptTraceSchema.parse({ ...traceSample, steps: 'x' })
  ).toThrow()
})

test('trace display helpers', () => {
  expect(splitRedacted('a=[REDACTED]&b=[REDACTED]')).toEqual([
    'a=',
    null,
    '&b=',
    null,
  ])
  expect(splitRedacted('[REDACTED]')).toEqual([null])
  expect(splitRedacted('plain')).toEqual(['plain'])
  expect([200, 204, 301, 404, 503, 0].map(httpStatusTone)).toEqual([
    'success',
    'success',
    'default',
    'danger',
    'danger',
    'danger',
  ])
  expect([0, 12, 999, 5012, 12_400, 125_000].map(formatTraceDuration)).toEqual([
    '0 ms',
    '12 ms',
    '999 ms',
    '5.01 s',
    '12.4 s',
    '2 min 5 s',
  ])
  // Console time (Asia/Shanghai) like `formatDate`, milliseconds cut as
  // written; every offset form the backend writes lands on the same instant.
  expect(formatTraceTime('2026-10-14T14:16:49.138956+00')).toBe(
    '2026-10-14 22:16:49.138'
  )
  for (const value of [
    '2026-10-14T14:16:49.138456+00',
    '2026-10-14T14:16:49.138Z',
    '2026-10-14T14:16:49.138+00:00',
    '2026-10-14T22:16:49.138+0800',
    '2026-10-14T22:16:49.138',
  ])
    expect(formatTraceClock(value)).toBe('22:16:49.138')
  expect(formatTraceTime('2026-10-14T16:00:00+00')).toBe(
    '2026-10-15 00:00:00.000'
  )
  expect(formatTraceClock('')).toBe('')
  expect(formatTraceClock('14:16:49')).toBe('')
  expect(
    ['release_id', 'node_spec_revision', 'manifest_digest', 'generation'].map(
      isConfigVersionField
    )
  ).toEqual([true, true, true, true])
  expect(
    ['applied', 'failed_phase', 'error_code', 'agent_version'].map(
      isConfigVersionField
    )
  ).toEqual([false, false, false, false])
})

test('node message fields never render release identifiers', () => {
  const release = '5a1ae4e9-be8a-42dc-9285-4f7f6c68906a'
  const html = render(
    createElement(NodeMessageDetail, {
      message: {
        ...nodeSample,
        fields: [
          { name: 'release_id', value: release },
          ...nodeSample.fields,
          { name: 'manifest_digest', value: 'sha256-6727a0b3' },
        ],
      },
    })
  ).html
  expect(html).not.toContain('release_id')
  expect(html).not.toContain(release)
  expect(html).not.toContain('manifest_digest')
  expect(html).not.toContain('6727a0b3')
  expect(html).toContain('failed_phase')
  expect(html).toContain('validate')

  // A DesiredState carries only `release_id`: no field list at all.
  const desired = render(
    createElement(NodeMessageDetail, {
      message: {
        ...nodeSample,
        direction: 'sent',
        message: 'DesiredState',
        fields: [{ name: 'release_id', value: release }],
      },
    })
  ).html
  expect(desired).not.toContain(release)
  expect(desired).not.toContain('字段')
})

function render(element: ReactElement, client = new QueryClient()) {
  const html = renderToStaticMarkup(
    createElement(QueryClientProvider, { client }, element)
  )
  return { html, client }
}

test('redaction markers render as badges, never as raw text', () => {
  const { html } = render(
    createElement(RedactedText, { value: httpSample.url })
  )
  expect(html).not.toContain('[REDACTED]')
  expect(html.split('已隐藏').length - 1).toBe(3)
  expect(html).toContain('Action=DescribeDomainRecords')

  // The full trace view: redacted subject and error inside step rows, and the
  // capture footer counts hidden values.
  const client = new QueryClient()
  const trace: TaskAttemptTrace = parseTrace({
    ...traceSample,
    steps: [
      {
        ...stepSample,
        subject: 'key=[REDACTED]',
        error: 'token [REDACTED] rejected',
      },
    ],
  })
  client.setQueryData(taskAttemptTraceQuery('task-1', '48213').queryKey, trace)
  const view = render(
    createElement(AttemptTraceView, { taskId: 'task-1', attemptId: '48213' }),
    client
  ).html
  expect(view).not.toContain('[REDACTED]')
  expect(view).toContain('已隐藏 2 处敏感内容')
  expect(view).toContain('查找远程托管区域')
  expect(view).toContain('rejected')
  // Outcomes are labelled icons, not visually hidden absolute spans: those
  // escape the sheet body's scroll box and let scrolling move the whole sheet.
  expect(view).toContain('role="img" aria-label="失败"')
  expect(view).not.toContain('sr-only')
  client.clear()
})

test('step strip shows merged steps and the failed step; the trace loads on demand only', () => {
  const summary = taskHistorySchema.parse(
    historyOf({
      ...attemptBase,
      trace: {
        ...summarySample,
        step_count: 8,
        steps: [
          ...summarySample.steps.slice(0, 3),
          {
            code: 'check_domain',
            label: '检测域名解析',
            outcome: 'failed',
            duration_ms: 5100,
            call_count: 4,
            count: 4,
          },
        ],
      },
    })
  ).list[0].trace
  const { html, client } = render(
    createElement(AttemptTrace, { taskId: 'task-1', trace: summary })
  )
  expect(html).toContain('失败步骤：查找远程托管区域')
  expect(html).toContain('×4')
  expect(html).toContain('5.10 s')
  // 8 steps in total, 7 listed (three single steps + four merged).
  expect(html).toContain('另有 1 个步骤')
  // The merged failed step is highlighted.
  expect(html).toMatch(/<li class="[^"]*text-danger[^"]*">.*?检测域名解析/)
  expect(html).toContain('查看执行详情')
  expect(html).toContain('8 个步骤 · 3 次调用 · 4 条日志')
  expect(html).toContain('role="img" aria-label="完成"')
  expect(html).not.toContain('sr-only')
  // Collapsed: no trace query exists, so nothing was requested.
  expect(
    client.getQueryCache().findAll({ queryKey: ['tasks', 'trace'] })
  ).toEqual([])

  const none: TaskAttemptTraceSummary = {
    ...summarySample,
    state: 'none',
    steps: [],
  }
  const empty = render(
    createElement(AttemptTrace, { taskId: 'task-1', trace: none })
  ).html
  expect(empty).toContain('此次尝试没有执行详情')
  expect(empty).not.toContain('查看执行详情')
  expect(
    render(createElement(AttemptTrace, { taskId: 'task-1', trace: null })).html
  ).toBe('')
  expect(
    render(
      createElement(AttemptTrace, { taskId: 'task-1', trace: invalidItem })
    ).html
  ).toContain('执行详情无法显示')
})
