import { z } from 'zod'
import { collectionPageSchema } from '@/lib/pagination'

// service/domains/task/task_types.h and task_service.h define the wire contract.
export const taskSchema = z.object({
  id: z.string(),
  resource_type: z.enum([
    'provider',
    'dns_zone',
    'certificate',
    'website',
    'node',
  ]),
  resource_id: z.string(),
  name: z.string(),
  operation: z.string(),
  version: z.number().int().nonnegative(),
  status: z.enum([
    'queued',
    'running',
    'retrying',
    'completed',
    'recovered',
    'failed',
    'superseded',
  ]),
  error: z.string(),
  failures: z.number().int().nonnegative(),
  updated_at: z.string(),
  next_attempt_at: z.string(),
})
export const taskPageSchema = collectionPageSchema.extend({
  list: z.array(taskSchema),
  active: z.number().int().nonnegative(),
  failed: z.number().int().nonnegative(),
})

// service/domains/task (`task_attempt_dto`) defines the history contract. Times are
// `YYYY-MM-DDTHH:MM:SS.ffffff+00` or '' when unknown. Records written before
// the upgrade have no start time, a zero duration and `detail.kind: 'none'`.
// Every detail carries `truncated`: items beyond the size limits were omitted
// (the `*_count` fields stay exact).
//
// History must survive a newer backend: closed enums accept any other string
// (shown raw and muted, see `known`), and a detail that does not parse becomes
// `{ kind: 'invalid' }` so only that attempt loses its detail.
const count = z.number().int().nonnegative()

/** Value outside the known set, kept as the raw wire string. */
type Unknown = string & {}
function open<const T extends readonly [string, ...string[]]>(values: T) {
  return z.union([z.enum(values), z.string()]) as unknown as z.ZodType<
    T[number] | Unknown
  >
}

/** Narrows an open enum to its known set; `undefined` for unknown values. */
export function known<K extends string, V>(
  map: Readonly<Record<K, V>>,
  value: string
): V | undefined {
  return Object.hasOwn(map, value) ? map[value as K] : undefined
}

const dnsRecordSchema = z.object({
  name: z.string(),
  type: z.string(),
  line_code: z.string(),
  content: z.string(),
  ttl: count,
  priority: z.number().int().nullable(),
  proxied: z.boolean(),
})
const detailBase = { truncated: z.boolean() }
const taskAttemptDetailSchema = z.discriminatedUnion('kind', [
  z.object({ kind: z.literal('none'), ...detailBase }),
  z.object({
    kind: z.literal('dns_sync'),
    ...detailBase,
    created: z.array(dnsRecordSchema),
    updated: z.array(
      z.object({ before: dnsRecordSchema, after: dnsRecordSchema })
    ),
    deleted: z.array(dnsRecordSchema),
    created_count: count,
    updated_count: count,
    deleted_count: count,
    unchanged_count: count,
    conflicts: z.array(
      z.object({
        name: z.string(),
        type: z.string(),
        line_code: z.string(),
        local_content: z.string(),
        remote_content: z.string(),
      })
    ),
    conflict_count: count,
  }),
  z.object({
    kind: z.literal('certificate'),
    ...detailBase,
    operation: open(['issue', 'renew']),
    provider: z.string(),
    domains: z.array(z.string()),
    // `completed` or the stage the attempt failed in (see certificateStages).
    stage: z.string(),
    issuer: z.string(),
    not_before: z.string(),
    not_after: z.string(),
    serial_number: z.string(),
    fingerprint_sha256: z.string(),
  }),
  z.object({
    kind: z.literal('website_domains'),
    ...detailBase,
    expected_target: z.string(),
    domain_count: count,
    checked_count: count,
    domains: z.array(
      z.object({
        hostname: z.string(),
        query_name: z.string(),
        expected: z.string(),
        resolved: z.array(z.string()),
        status: open(['verified', 'invalid', 'unverified']),
        error: z.string(),
      })
    ),
  }),
  z.object({
    kind: z.literal('provider'),
    ...detailBase,
    provider: z.string(),
    provider_kind: open(['dns', 'certificate']),
    zone_count: count,
    zones: z.array(z.object({ name: z.string(), status: z.string() })),
    directory_url: z.string(),
    external_account: open(['', 'reused', 'fetched', 'not_required']),
  }),
  z.object({
    kind: z.literal('node_apply'),
    ...detailBase,
    node_id: z.string(),
    node_name: z.string(),
    agent_version: z.string(),
    source: open(['apply_result', 'heartbeat']),
    result: open(['applied', 'failed']),
    phase: open(['', 'stage', 'validate', 'activate']),
    error_code: z.string(),
    retryable: z.boolean(),
  }),
])
const invalidDetail = { kind: 'invalid' } as const

// Attempt traces (build/task-trace/contract.md, docs/task-center.md
// "执行跟踪"). Every string is redacted server-side; `[REDACTED]` marks a
// hidden value. Like history, traces degrade locally: absent fields take
// their empty value (older or abridged payloads), a field of the wrong type
// fails only the enclosing item, and list items that do not parse become
// `{ invalid: true }` so one bad call never hides its step or the trace.
const text = z.string().default('')
const amount = count.default(0)
const flag = z.boolean().default(false)

/** List item that did not parse; rendered as a short local notice. */
export const invalidItem = { invalid: true } as const
export type InvalidItem = typeof invalidItem
export function isInvalid(item: object): item is InvalidItem {
  return 'invalid' in item
}
function lenientList<T extends z.ZodType>(item: T) {
  return z
    .array(z.unknown())
    .default([])
    .transform((values) =>
      values.map((value): z.output<T> | InvalidItem => {
        const result = item.safeParse(value)
        return result.success ? result.data : invalidItem
      })
    )
}

export const traceStepOutcome = open([
  'completed',
  'failed',
  'skipped',
  'interrupted',
]).default('')
const traceStepSummarySchema = z.object({
  code: text,
  label: text,
  outcome: traceStepOutcome,
  duration_ms: amount,
  call_count: amount,
  // Adjacent steps with the same code merge into one summary item.
  count: z.number().int().positive().default(1),
})
const traceSummarySchema = z.object({
  state: open(['available', 'none']),
  // Path parameter of the trace stream only; never shown.
  attempt_id: text,
  step_count: amount,
  call_count: amount,
  log_count: amount,
  truncated: flag,
  failed_step: z.object({ code: text, label: text }).nullable().default(null),
  // A summary item that does not parse is left out of the step strip; the
  // full trace still lists every step.
  steps: lenientList(traceStepSummarySchema).transform((steps) =>
    steps.flatMap((step) => (isInvalid(step) ? [] : [step]))
  ),
})

const tracePairSchema = z.object({
  name: text,
  value: text,
  redacted: flag,
})
const traceBodySchema = z.object({
  content_type: text,
  format: open([
    'json',
    'jws_decoded',
    'form',
    'text',
    'pem_summary',
    'omitted',
  ]).default(''),
  bytes: amount,
  excerpt: text,
  truncated: flag,
  omitted_reason: open([
    '',
    'secret_material',
    'binary',
    'too_large',
    'budget',
    'policy',
  ]).default(''),
})
const traceHttpSchema = z.object({
  method: text,
  url: text,
  scheme: text,
  host: text,
  path: text,
  query: z.array(tracePairSchema).default([]),
  request_headers: z.array(tracePairSchema).default([]),
  request_body: traceBodySchema.nullable().default(null),
  // HTTP status; 0 when the request failed in transport.
  status: amount,
  transport_error: text,
  response_headers: z.array(tracePairSchema).default([]),
  response_body: traceBodySchema.nullable().default(null),
})
const traceDnsSchema = z.object({
  resolver: text,
  transport: text,
  name: text,
  type: text,
  rcode: text,
  answers: z
    .array(z.object({ name: text, type: text, ttl: amount, data: text }))
    .default([]),
  truncated: flag,
})
const traceNodeMessageSchema = z.object({
  direction: open(['sent', 'received']).default(''),
  message: text,
  request_id: text,
  object_count: amount,
  bytes: amount,
  fields: z.array(z.object({ name: text, value: text })).default([]),
})
const traceCallSchema = z.object({
  index: amount,
  kind: open(['http', 'dns', 'node_message']),
  operation: text,
  subject: text,
  started_at: text,
  offset_ms: amount,
  duration_ms: amount,
  outcome: open([
    'ok',
    'http_error',
    'transport_error',
    'cancelled',
    'invalid_response',
  ]).default(''),
  // Identical requests within a step (ACME polling, nonces) fold into one.
  repeat_count: z.number().int().positive().default(1),
  repeat_total_ms: amount,
  last_started_at: text,
  provider_request_id: text,
  provider_code: text,
  error: text,
  http: traceHttpSchema.nullable().default(null),
  dns: traceDnsSchema.nullable().default(null),
  node_message: traceNodeMessageSchema.nullable().default(null),
})
const traceLogSchema = z.object({
  at: text,
  offset_ms: amount,
  level: open(['info', 'warning', 'error']).default('info'),
  message: text,
  repeat_count: z.number().int().positive().default(1),
})
const traceStepSchema = z.object({
  index: amount,
  code: text,
  label: text,
  subject: text,
  started_at: text,
  offset_ms: amount,
  duration_ms: amount,
  outcome: traceStepOutcome,
  error: text,
  related_task: z
    .object({
      task_id: z.string(),
      resource_type: text,
      resource_id: z.string(),
      label: text,
    })
    .nullable()
    .default(null),
  call_count: amount,
  omitted_calls: amount,
  calls: lenientList(traceCallSchema),
  logs: lenientList(traceLogSchema),
})
/** Full trace of one attempt (`/tasks/:id/attempts/:attempt_id/trace/stream`). */
export const taskAttemptTraceSchema = z.object({
  attempt_id: text,
  task_id: text,
  resource_type: text,
  resource_id: text,
  kind: text,
  state: open(['available', 'none']),
  attempt: amount,
  outcome: open(['completed', 'failed']).default(''),
  error: text,
  started_at: text,
  emitted_at: text,
  duration_ms: amount,
  worker_type: text,
  capture: z
    .object({
      truncated: flag,
      degraded: flag,
      bytes: amount,
      omitted_steps: amount,
      omitted_calls: amount,
      omitted_logs: amount,
      omitted_body_bytes: amount,
      redactions: amount,
    })
    .prefault({}),
  steps: lenientList(traceStepSchema),
})

const taskAttemptSchema = z.object({
  attempt: z.number().int().positive(),
  outcome: open(['completed', 'failed']),
  error: z.string(),
  started_at: z.string(),
  emitted_at: z.string(),
  duration_ms: z.number().int().nonnegative(),
  next_attempt_at: z.string(),
  detail: taskAttemptDetailSchema.or(
    z.unknown().transform(() => invalidDetail)
  ),
  // Absent before the backend recorded traces; a summary that does not
  // parse only hides this attempt's trace.
  trace: traceSummarySchema
    .nullable()
    .optional()
    .transform((trace) => trace ?? null)
    .or(z.unknown().transform(() => invalidItem)),
})
export const taskHistorySchema = z.object({
  truncated: z.boolean(),
  summary: z.object({
    attempt_count: z.number().int().nonnegative(),
    failure_count: z.number().int().nonnegative(),
    first_failure_at: z.string(),
    last_success_at: z.string(),
    next_attempt_at: z.string(),
  }),
  list: z.array(taskAttemptSchema),
})
export type Task = z.infer<typeof taskSchema>
export type TaskHistory = z.infer<typeof taskHistorySchema>
export type TaskAttempt = z.infer<typeof taskAttemptSchema>
export type TaskAttemptDetail = TaskAttempt['detail']
export type TaskAttemptTraceSummary = TaskAttempt['trace']
export type TaskAttemptTrace = z.infer<typeof taskAttemptTraceSchema>
export type TraceStep = Exclude<TaskAttemptTrace['steps'][number], InvalidItem>
export type TraceCall = Exclude<TraceStep['calls'][number], InvalidItem>
export type TraceLog = Exclude<TraceStep['logs'][number], InvalidItem>
export type TraceBody = NonNullable<
  NonNullable<TraceCall['http']>['request_body']
>
export type TracePair = NonNullable<TraceCall['http']>['query'][number]
