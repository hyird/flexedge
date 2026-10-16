import { z } from 'zod'
import { collectionPageSchema } from '@/lib/pagination'

// build/operation-tasks/contract.md defines the task center wire contract: a
// task is one user operation (or one system action), its subtasks are the
// background work it caused. Times are `YYYY-MM-DDTHH:MM:SS.ffffff+00` or ''
// when unknown or not applicable.
//
// Everything must survive a newer backend: enums are open (unknown values are
// kept as the raw string, shown raw and muted, see `known`), and a summary key
// that does not parse is dropped on its own.
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

export const operationStatusValues = [
  'queued',
  'running',
  'retrying',
  'failed',
  'partial_failed',
  'attention_required',
  'completed',
  'merged',
  'cancelled',
] as const
export const operationTargetTypes = [
  'website',
  'cluster',
  'node',
  'dns_zone',
  'certificate',
  'provider',
] as const
const operationStatus = open(operationStatusValues)

// Contract §5.4: every key optional, unknown keys ignored, one malformed key
// never hides the others.
const operationSummarySchema = z
  .object({
    sections: z.array(z.string()).optional().catch(undefined),
    record_counts: z
      .object({ created: count, updated: count, deleted: count })
      .optional()
      .catch(undefined),
    credentials_changed: z.boolean().optional().catch(undefined),
  })
  .catch({})

/** `operation_dto` (contract §2.3): one row of the task list. */
export const operationSchema = z.object({
  id: z.string(),
  // Open codes (contract §5.1); `operationActions` names the known ones.
  action: z.string(),
  target_type: open(operationTargetTypes),
  target_id: z.string(),
  target_name: z.string(),
  target_deleted: z.boolean(),
  // Only for `target_type = provider` (contract §2.3); open like the enums.
  target_kind: open(['dns', 'certificate']).optional(),
  actor_type: open(['user', 'system']),
  actor_name: z.string(),
  created_at: z.string(),
  updated_at: z.string(),
  finished_at: z.string(),
  status: operationStatus,
  subtask_count: count,
  settled_count: count,
  failed_count: count,
  next_attempt_at: z.string(),
  error: z.string(),
  summary: operationSummarySchema,
})
export const operationPageSchema = collectionPageSchema.extend({
  list: z.array(operationSchema),
  active: count,
  attention: count,
})

/** `subtask_dto` (contract §3.4). */
export const subtaskSchema = z.object({
  id: z.string(),
  parent_id: z.string(),
  // Open codes (contract §5.3); `subtaskKinds` names the known ones.
  kind: z.string(),
  resource_type: open([
    'dns_zone',
    'certificate',
    'website',
    'provider',
    'node',
  ]),
  resource_id: z.string(),
  resource_name: z.string(),
  resource_deleted: z.boolean(),
  status: operationStatus,
  warning: open(['', 'dns_conflict', 'dns_unpushed', 'domain_unverified']),
  node_offline: z.boolean(),
  created_at: z.string(),
  running_since: z.string(),
  finished_at: z.string(),
  duration_ms: count,
  attempt_count: count,
  failure_count: count,
  recovered: z.boolean(),
  next_attempt_at: z.string(),
  error: z.string(),
  merged_into: z
    .object({
      operation_id: z.string(),
      subtask_id: z.string(),
      action: z.string(),
      target_name: z.string(),
      status: operationStatus,
    })
    .optional(),
  closed_by: z
    .object({
      operation_id: z.string(),
      action: z.string(),
      target_name: z.string(),
    })
    .optional(),
})
/** `operation_detail_dto` (contract §3.3): the detail stream's `snapshot`. */
export const operationDetailSchema = operationSchema.extend({
  subtasks: z.array(subtaskSchema),
})

export type Operation = z.infer<typeof operationSchema>
export type OperationPage = z.infer<typeof operationPageSchema>
export type OperationDetail = z.infer<typeof operationDetailSchema>
export type OperationStatus = Operation['status']
export type OperationSummary = Operation['summary']
export type Subtask = z.infer<typeof subtaskSchema>

// Execution records (`attempt_dto`, contract §3.5): the shape of the former
// task history, `attempt` numbered within the subtask. Records without a
// start time have a zero duration and `detail.kind: 'none'`. A detail that
// does not parse becomes `{ kind: 'invalid' }` so only that attempt loses it.
const dnsRecordSchema = z.object({
  name: z.string(),
  type: z.string(),
  line_code: z.string(),
  content: z.string(),
  ttl: count,
  priority: z.number().int().nullable(),
  proxied: z.boolean(),
})
const taskAttemptDetailSchema = z.discriminatedUnion('kind', [
  z.object({ kind: z.literal('none'), truncated: z.boolean() }),
  z.object({
    kind: z.literal('dns_sync'),
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
    operation: open(['issue', 'renew']),
    provider: z.string(),
    domains: z.array(z.string()),
    // `completed` or the stage the attempt failed in (shown by the trace).
    stage: z.string(),
    issuer: z.string(),
    not_before: z.string(),
    not_after: z.string(),
    serial_number: z.string(),
    fingerprint_sha256: z.string(),
  }),
  z.object({
    kind: z.literal('website_domains'),
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
    provider: z.string(),
    provider_kind: open(['dns', 'certificate']),
    zone_count: count,
    zones: z.array(z.object({ name: z.string(), status: z.string() })),
    directory_url: z.string(),
    external_account: open(['', 'reused', 'fetched', 'not_required']),
  }),
  z.object({
    kind: z.literal('node_apply'),
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

// Attempt traces. Every string is redacted server-side; `[REDACTED]` marks a
// hidden value. Like history, traces degrade locally: absent fields take
// their empty value (older or abridged payloads), a field of the wrong type
// fails only the enclosing item, and list items that do not parse become
// `{ invalid: true }` so one bad call never hides its step or the trace.
const text = z.string().default('')
const amount = count.default(0)

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

const traceStepOutcome = open([
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
// Only what the timeline shows before the full trace arrives; the wire
// summary also carries totals and the first failed step.
const traceSummarySchema = z.object({
  state: open(['available', 'none']),
  // Path parameter of the trace stream only; never shown.
  attempt_id: text,
  step_count: amount,
  // A summary item that does not parse is left out of the timeline; the
  // full trace still lists every step.
  steps: lenientList(traceStepSummarySchema).transform((steps) =>
    steps.flatMap((step) => (isInvalid(step) ? [] : [step]))
  ),
})

// The full trace schemas below read only the fields the timeline shows.
const traceBodySchema = z.object({
  excerpt: text,
})
const traceHttpSchema = z.object({
  method: text,
  url: text,
  host: text,
  path: text,
  request_body: traceBodySchema.nullable().default(null),
  // HTTP status; 0 when the request failed in transport.
  status: amount,
  transport_error: text,
  response_body: traceBodySchema.nullable().default(null),
})
const traceDnsSchema = z.object({
  resolver: text,
  name: text,
  type: text,
  rcode: text,
  answers: z
    .array(z.object({ name: text, type: text, ttl: amount, data: text }))
    .default([]),
})
const traceNodeMessageSchema = z.object({
  direction: open(['sent', 'received']).default(''),
  message: text,
  object_count: amount,
  fields: z.array(z.object({ name: text, value: text })).default([]),
})
const traceCallSchema = z.object({
  kind: open(['http', 'dns', 'node_message']),
  operation: text,
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
  provider_request_id: text,
  provider_code: text,
  error: text,
  http: traceHttpSchema.nullable().default(null),
  dns: traceDnsSchema.nullable().default(null),
  node_message: traceNodeMessageSchema.nullable().default(null),
})
const traceLogSchema = z.object({
  at: text,
  level: open(['info', 'warning', 'error']).default('info'),
  message: text,
  repeat_count: z.number().int().positive().default(1),
})
const traceStepSchema = z.object({
  code: text,
  label: text,
  subject: text,
  duration_ms: amount,
  outcome: traceStepOutcome,
  error: text,
  // `subtask_id` (contract §3.7) links a step to another subtask of the same
  // task; steps without one show no link.
  related_task: z
    .object({
      subtask_id: text,
      label: text,
    })
    .nullable()
    .default(null),
  calls: lenientList(traceCallSchema),
  logs: lenientList(traceLogSchema),
})
/** Full trace of one attempt
 * (`/operations/:id/attempts/:attempt_id/trace/stream`). */
export const taskAttemptTraceSchema = z.object({
  state: open(['available', 'none']),
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
const historySummarySchema = z.object({
  attempt_count: count,
  failure_count: count,
  first_failure_at: z.string(),
  last_success_at: z.string(),
  next_attempt_at: z.string(),
})
/** `subtask_history_dto` (contract §3.5): the `history` event, newest first.
 * `truncated` is kept on the wire only; the UI never mentions it. */
export const subtaskHistorySchema = z.object({
  subtask_id: z.string(),
  summary: historySummarySchema,
  list: z.array(taskAttemptSchema),
})
/** `subtask_attempt_event` (contract §3.6): the `attempt` event, plain JSON. */
export const subtaskAttemptEventSchema = z.object({
  subtask_id: z.string(),
  item: z.object({ trace: z.unknown().optional() }).loose(),
  summary: historySummarySchema,
})
export type SubtaskHistory = z.infer<typeof subtaskHistorySchema>
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
