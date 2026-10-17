import { z } from 'zod'
import { collectionPageSchema } from '@/lib/pagination'

// Task center wire contract (GoEdge model): one task per (resource_type,
// resource_id); re-initiating work resets the same task and its history is
// that task's attempts. Times are `YYYY-MM-DDTHH:MM:SS.ffffff+00` or '' when
// unknown or not applicable.
//
// Everything must survive a newer backend: enums are open (unknown values are
// kept as the raw string, shown raw and muted, see `known`).
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

export const taskStatusValues = [
  'queued',
  'running',
  'retrying',
  'failed',
  'completed',
] as const
export const taskResourceTypes = [
  'cache',
  'website',
  'dns_zone',
  'certificate',
  'provider',
  'node',
] as const
export type TaskResourceType = (typeof taskResourceTypes)[number]

/** One task (`/api/tasks` list row, `GET /api/tasks/:type/:id` and the
 * detail stream's `snapshot`). */
export const taskSchema = z.object({
  resource_type: open(taskResourceTypes),
  resource_id: z.string(),
  // Provider tasks: the provider's kind (selects the /providers/{dns|
  // certificate} list); '' for every other type.
  resource_kind: open(['', 'dns', 'certificate']),
  name: z.string(),
  // Open codes; `taskOperations` names the known ones.
  operation: z.string(),
  status: open(taskStatusValues),
  error: z.string(),
  next_attempt_at: z.string(),
  updated_at: z.string(),
  last_success_at: z.string(),
  attempt_count: count,
  failure_count: count,
  node_offline: z.boolean(),
})
export const taskPageSchema = collectionPageSchema.extend({
  list: z.array(taskSchema),
  active: count,
  failed: count,
})

export type Task = z.infer<typeof taskSchema>
export type TaskPage = z.infer<typeof taskPageSchema>
export type TaskStatus = Task['status']

/** Identity of a task: the resource it belongs to. */
export type TaskRef = { resourceType: string; resourceId: string }

export function taskRefOf(
  task: Pick<Task, 'resource_type' | 'resource_id'>
): TaskRef {
  return { resourceType: task.resource_type, resourceId: task.resource_id }
}

// Execution records (`attempt_dto`): every retained attempt of the task,
// newest first, `attempt` numbered within the task. Records without a
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
  // Path parameter of the trace GET read only; never shown.
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
  // Links a step to another task (a certificate waiting for its DNS zone's
  // sync); steps without one show no link.
  related_task: z
    .object({
      resource_type: text,
      resource_id: text,
      label: text,
    })
    .nullable()
    .default(null),
  calls: lenientList(traceCallSchema),
  logs: lenientList(traceLogSchema),
})
/** Full trace of one attempt
 * (`GET /api/tasks/:type/:id/attempts/:attempt_id/trace`). */
export const taskAttemptTraceSchema = z.object({
  state: open(['available', 'none']),
  steps: lenientList(traceStepSchema),
})

const taskAttemptSchema = z.object({
  // `sys_sync_event` id as a decimal string (= `trace.attempt_id`).
  id: z.string(),
  attempt: z.number().int().positive(),
  // Node tasks mix `publish` and `configure` attempts.
  operation: z.string(),
  // `progressed`: a staged attempt advanced (certificate DNS-01 records
  // written) and the task waits for the DNS sync before continuing.
  outcome: open(['completed', 'failed', 'progressed']),
  error: z.string(),
  started_at: z.string(),
  emitted_at: z.string(),
  duration_ms: z.number().int().nonnegative(),
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
})
/** The detail stream's `history` event, newest first. */
export const taskHistorySchema = z.object({
  summary: historySummarySchema,
  list: z.array(taskAttemptSchema),
})
/** The detail stream's `attempt` event (plain JSON). */
export const taskAttemptEventSchema = z.object({
  item: z.object({ id: z.string() }).loose(),
  summary: historySummarySchema,
})
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
