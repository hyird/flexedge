import { createElement, type ReactElement } from 'react'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { renderToStaticMarkup } from 'react-dom/server'
import { expect, test } from 'vitest'
import { taskAttemptTraceQuery } from './data'
import { AttemptTimeline, RedactedText } from './task-attempt-trace'
import {
  formatTraceClock,
  formatTraceTime,
  formatTraceDuration,
  httpStatusTone,
  isConfigVersionField,
  observeFirstSight,
  splitRedacted,
} from './task-trace'
import {
  invalidItem,
  taskHistorySchema,
  taskAttemptTraceSchema,
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
  failed_step: { code: 'find_zone', label: '查找远程托管域名' },
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
      label: '查找远程托管域名',
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
  label: '查找远程托管域名',
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
const ref = { resourceType: 'dns_zone', resourceId: '9a1e' }
const attemptBase = {
  id: '48213',
  attempt: 3,
  operation: 'sync',
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
  },
  list,
})

function parseTrace(value: unknown) {
  return taskAttemptTraceSchema.parse(value)
}

test('trace schemas read the contract samples', () => {
  const history = taskHistorySchema.parse(
    historyOf({ ...attemptBase, trace: summarySample })
  )
  // Only what the timeline shows; totals and the failed step stay on the wire.
  expect(history.list[0].trace).toEqual({
    state: 'available',
    attempt_id: '48213',
    step_count: 6,
    steps: summarySample.steps,
  })

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
  expect(trace).not.toHaveProperty('capture')
  const [first, second, abridged] = trace.steps
  expect(first).toEqual({
    code: 'find_zone',
    label: '查找远程托管域名',
    subject: 'example.com',
    duration_ms: 5012,
    outcome: 'failed',
    error: 'Cloudflare API 连接失败',
    related_task: null,
    calls: [
      {
        kind: 'http',
        operation: 'cloudflare.find_zone',
        duration_ms: 5003,
        outcome: 'transport_error',
        repeat_count: 1,
        provider_request_id: '',
        provider_code: '',
        error: 'connect_failed',
        http: {
          method: 'GET',
          url: httpSample.url,
          host: 'alidns.aliyuncs.com',
          path: '/',
          request_body: null,
          status: 0,
          transport_error: 'connect_failed',
          response_body: null,
        },
        dns: null,
        node_message: null,
      },
    ],
    logs: [
      {
        at: logSample.at,
        level: 'warning',
        message: logSample.message,
        repeat_count: 1,
      },
    ],
  })
  expect(second).toMatchObject({
    calls: [
      {
        dns: {
          resolver: 'dns.alidns.com',
          rcode: 'NOERROR',
          answers: dnsSample.answers,
        },
        http: {
          status: 400,
          response_body: { excerpt: httpSample.response_body.excerpt },
        },
      },
      {
        node_message: {
          direction: 'received',
          message: 'ApplyResult',
          fields: nodeSample.fields,
        },
      },
    ],
  })
  expect(abridged).toMatchObject({
    subject: '',
    related_task: null,
    logs: [],
    calls: [
      {
        repeat_count: 31,
        outcome: '',
        http: { method: 'HEAD', request_body: null, transport_error: '' },
      },
      {
        repeat_count: 30,
        http: {
          request_body: {
            excerpt: abridgedStep.calls[1].http.request_body!.excerpt,
          },
          response_body: {
            excerpt: abridgedStep.calls[1].http.response_body!.excerpt,
          },
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
          { ...logSample, repeat_count: 0 },
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
    http: { response_body: { excerpt: httpSample.response_body.excerpt } },
  })
  expect(step.logs).toEqual([
    {
      at: logSample.at,
      level: 'debug',
      message: logSample.message,
      repeat_count: 1,
    },
    invalidItem,
  ])
  expect(broken).toEqual(invalidItem)
  expect(intact).toEqual(parseTrace(traceSample).steps[0])

  // History: a malformed summary hides only that attempt's trace; an absent
  // one (backend without traces) is null; summary items that do not parse
  // drop out of the timeline.
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
    {
      state: 'expired',
      attempt_id: '48213',
      step_count: 6,
      steps: [summarySample.steps[0]],
    },
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

function render(element: ReactElement, client = new QueryClient()) {
  const html = renderToStaticMarkup(
    createElement(QueryClientProvider, { client }, element)
  )
  client.clear()
  return html
}

const summary = taskHistorySchema.parse(
  historyOf({ ...attemptBase, trace: summarySample })
).list[0].trace

/** Timeline of the sample attempt with `steps` as its cached full trace. */
function renderTimeline(steps?: unknown[]) {
  const client = new QueryClient()
  if (steps)
    client.setQueryData(
      taskAttemptTraceQuery(ref, '48213').queryKey,
      parseTrace({ ...traceSample, steps })
    )
  return render(
    createElement(AttemptTimeline, { task: ref, trace: summary }),
    client
  )
}

test('the timeline renders from the summary before the full trace arrives', () => {
  const html = renderTimeline()
  for (const label of [
    '加载同步状态',
    '解密凭据',
    '校验 TTL',
    '查找远程托管域名',
    '记录失败',
  ])
    expect(html).toContain(label)
  expect(html).toContain('5.01 s')
  // One accessible list with an outcome marker per stored step, including
  // before the full trace arrives. Attribute order is owned by Pro Timeline.
  expect(html.match(/<ol\b[^>]*aria-label="执行步骤"/g)).toHaveLength(1)
  expect(html.match(/role="img"/g)).toHaveLength(5)
  expect(html).not.toContain('另有')
  expect(html).toContain('role="img" aria-label="失败"')
  expect(html).not.toContain('sr-only')
  // Only the step that made a call holds a placeholder for its details.
  expect(html.match(/class="skeleton /g)).toHaveLength(1)
  // No toggles, counts or captions.
  expect(html).not.toContain('aria-expanded')
  expect(html).not.toContain('执行详情')
  expect(html).not.toContain('失败步骤')

  // Merged steps retain their actual repeat count, without omission hints.
  const merged = taskHistorySchema.parse(
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
  const mergedHtml = render(
    createElement(AttemptTimeline, { task: ref, trace: merged })
  )
  expect(mergedHtml).toContain('×4')
  expect(mergedHtml).toContain('5.10 s')
  expect(mergedHtml).not.toContain('另有')
  expect(mergedHtml).toMatch(/text-danger[^"]*">检测域名解析/)
})

test('the full trace fills in the failed step error, calls and logs', () => {
  const html = renderTimeline([
    stepSample,
    {
      ...stepSample,
      code: 'list_records',
      label: '拉取远程记录',
      outcome: 'completed',
      error: '',
      calls: [
        {
          ...callSample,
          outcome: 'http_error',
          provider_request_id: '5E7C-request',
          provider_code: 'SignatureDoesNotMatch',
          error: '',
          http: httpSample,
        },
        { ...callSample, kind: 'dns', http: httpSample, dns: dnsSample },
      ],
      logs: [{ ...logSample, repeat_count: 3 }],
    },
  ])
  expect(html).toMatch(/text-danger">Cloudflare API 连接失败<\/p>/)
  // Transport failures read as short text instead of a status 0.
  expect(html).toContain('无法建立连接')
  // HTTP: method, host + path, status, provider ids, the response excerpt.
  expect(html).toContain('alidns.aliyuncs.com/')
  expect(html).toContain('>400<')
  expect(html).toContain('5E7C-request')
  expect(html).toContain('SignatureDoesNotMatch')
  expect(html).toContain('&quot;HostId&quot;: &quot;alidns.aliyuncs.com&quot;')
  // DNS: resolver, query, rcode and answers.
  expect(html).toContain('dns.alidns.com · flexedge-1a2b3c4d.example.com CNAME')
  expect(html).toContain('NOERROR')
  expect(html).toContain('edge-hk.cdn.example.net')
  // Logs: console clock, short level, folded repeats.
  expect(html).toContain('22:16:49.138')
  expect(html).toContain('警告')
  expect(html).toContain('×3')
  // No query/header tables, no placeholders, no toggles.
  expect(html).not.toContain('x-acs-request-id')
  expect(html).not.toContain('Action=DescribeDomainRecords')
  expect(html).not.toContain('skeleton')
  expect(html).not.toContain('aria-expanded')
  expect(html).not.toContain('已隐藏 2 处')
})
test('capture and omission metadata never add notes or hide saved excerpts', () => {
  for (const reason of [
    'secret_material',
    'binary',
    'too_large',
    'budget',
    'policy',
    'quota',
  ]) {
    const client = new QueryClient()
    client.setQueryData(
      taskAttemptTraceQuery(ref, '48213').queryKey,
      parseTrace({
        ...traceSample,
        capture: { truncated: true, degraded: true, redactions: 99 },
        steps: [
          {
            ...stepSample,
            omitted_calls: 8,
            calls: [
              {
                ...callSample,
                kind: 'http',
                http: {
                  ...httpSample,
                  request_body: {
                    format: 'omitted',
                    omitted_reason: reason,
                    excerpt: '',
                  },
                  response_body: {
                    format: 'omitted',
                    omitted_reason: reason,
                    truncated: true,
                    excerpt: 'saved response [REDACTED]',
                  },
                },
              },
              {
                ...callSample,
                kind: 'dns',
                http: null,
                dns: { ...dnsSample, truncated: true },
              },
            ],
          },
        ],
      })
    )
    const html = render(
      createElement(AttemptTimeline, { task: ref, trace: summary }),
      client
    )
    expect(html).toContain('saved response')
    expect(html).toContain('edge-hk.cdn.example.net')
    expect(html).toContain('15 秒后重试')
    expect(html).toContain('Cloudflare API 连接失败')
    expect(html.split('已隐藏').length - 1).toBe(1)
    for (const note of [
      '截断',
      '不完整',
      '另有',
      '未保存',
      '未解析',
      '预算',
      '不保存',
      '…',
    ])
      expect(html).not.toContain(note)
    expect(html).not.toContain('>请求<')
  }
})

test('node message fields never render release identifiers', () => {
  const release = '5a1ae4e9-be8a-42dc-9285-4f7f6c68906a'
  const html = renderTimeline([
    {
      ...stepSample,
      calls: [
        {
          ...callSample,
          kind: 'node_message',
          http: null,
          node_message: {
            ...nodeSample,
            direction: 'sent',
            message: 'DesiredState',
            fields: [{ name: 'release_id', value: release }],
          },
        },
        {
          ...callSample,
          kind: 'node_message',
          http: null,
          node_message: {
            ...nodeSample,
            fields: [
              { name: 'release_id', value: release },
              ...nodeSample.fields,
              { name: 'manifest_digest', value: 'sha256-6727a0b3' },
            ],
          },
        },
      ],
    },
  ])
  expect(html).toContain('DesiredState')
  expect(html).toContain('ApplyResult')
  expect(html).not.toContain('release_id')
  expect(html).not.toContain(release)
  expect(html).not.toContain('manifest_digest')
  expect(html).not.toContain('6727a0b3')
  expect(html).toContain('failed_phase=')
  expect(html).toContain('validate')
})

test('redaction markers render as badges, never as raw text', () => {
  const html = render(createElement(RedactedText, { value: httpSample.url }))
  expect(html).not.toContain('[REDACTED]')
  expect(html.split('已隐藏').length - 1).toBe(3)
  expect(html).toContain('Action=DescribeDomainRecords')

  const timeline = renderTimeline([
    {
      ...stepSample,
      subject: 'key=[REDACTED]',
      error: 'token [REDACTED] rejected',
      calls: [{ ...callSample, error: '', http: httpSample }],
      logs: [{ ...logSample, message: 'nonce [REDACTED] reused' }],
    },
  ])
  expect(timeline).not.toContain('[REDACTED]')
  expect(timeline).toContain('rejected')
  expect(timeline).toContain('reused')
  expect(timeline).not.toContain('已隐藏 2 处')
  expect(timeline.split('已隐藏').length - 1).toBe(4)
})

test('attempts without a trace render nothing', () => {
  const none: TaskAttemptTraceSummary = {
    state: 'none',
    attempt_id: '48213',
    step_count: 0,
    steps: [],
  }
  for (const trace of [none, null, { ...none, state: 'available' }])
    expect(render(createElement(AttemptTimeline, { task: ref, trace }))).toBe(
      ''
    )
  expect(
    render(
      createElement(AttemptTimeline, {
        task: ref,
        trace: invalidItem,
      })
    )
  ).toContain('执行详情无法识别')
})

test('a timeline reports first sight once, then stops observing', () => {
  const observers: {
    notify: IntersectionObserverCallback
    disconnected: boolean
  }[] = []
  const original = globalThis.IntersectionObserver
  // Node has no IntersectionObserver; this stand-in records its callback.
  globalThis.IntersectionObserver = class {
    state
    constructor(notify: IntersectionObserverCallback) {
      this.state = { notify, disconnected: false }
      observers.push(this.state)
    }
    observe() {}
    disconnect() {
      this.state.disconnected = true
    }
  } as never
  try {
    let seen = 0
    const stop = observeFirstSight({} as Element, () => {
      seen += 1
    })
    const [observer] = observers
    observer.notify([{ isIntersecting: false }] as never, {} as never)
    expect(seen).toBe(0)
    expect(observer.disconnected).toBe(false)
    observer.notify([{ isIntersecting: true }] as never, {} as never)
    expect(seen).toBe(1)
    expect(observer.disconnected).toBe(true)
    stop()
  } finally {
    globalThis.IntersectionObserver = original
  }
})
