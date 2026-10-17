import { Fragment, useCallback, useState, type ReactNode } from 'react'
import { useQuery } from '@tanstack/react-query'
import { EyeSlash, ListCheck } from '@gravity-ui/icons'
import { Timeline } from '@heroui-pro/react'
import { Button, Chip, Link, Skeleton } from '@heroui/react'
import { apiErrorCode } from '@/lib/api'
import { cn } from '@/lib/utils'
import { CopyButton } from '@/components/copy-button'
import { taskAttemptTraceQuery } from './data'
import { taskHref } from './task-display'
import {
  callOutcomes,
  formatTraceClock,
  formatTraceDuration,
  httpStatusTone,
  httpTarget,
  isConfigVersionField,
  logLevels,
  observeFirstSight,
  splitRedacted,
  stepOutcomes,
  traceNotFoundCode,
  transportErrors,
} from './task-trace'
import {
  isInvalid,
  known,
  type InvalidItem,
  type TaskAttemptTrace,
  type TaskAttemptTraceSummary,
  type TraceBody,
  type TraceCall,
  type TraceLog,
  type TraceStep,
  type TaskRef,
} from './types'

const toneText = {
  success: 'text-success',
  warning: 'text-warning',
  danger: 'text-danger',
  accent: 'text-accent',
  default: 'text-muted',
} as const

type Summary = Exclude<NonNullable<TaskAttemptTraceSummary>, InvalidItem>

/** Ref + flag that turns true once the element has been on screen; it never
 * turns back, so scrolling away neither cancels nor repeats a read. */
function useFirstSight() {
  const [seen, setSeen] = useState(false)
  const ref = useCallback(
    (element: HTMLElement | null) =>
      element ? observeFirstSight(element, () => setSeen(true)) : undefined,
    []
  )
  return [ref, seen] as const
}

/** Vertical execution timeline of one attempt. Steps render at once from the
 * history summary; the attempt's full trace (calls, logs, step errors) is read
 * once the timeline is on screen and fills in under each step. Attempts
 * without a trace render nothing. */
export function AttemptTimeline({
  task,
  trace,
  icon = <ListCheck aria-hidden />,
}: {
  task: TaskRef
  trace: TaskAttemptTraceSummary
  icon?: ReactNode
}) {
  if (trace === null) return null
  if (isInvalid(trace))
    return <p className='m-0 text-xs text-muted'>执行详情无法识别</p>
  if (trace.state !== 'available' || !trace.attempt_id || !trace.step_count)
    return null
  return <TraceTimeline task={task} summary={trace} icon={icon} />
}

function TraceTimeline({
  task,
  summary,
  icon,
}: {
  task: TaskRef
  summary: Summary
  icon: ReactNode
}) {
  const [ref, seen] = useFirstSight()
  const query = useQuery({
    ...taskAttemptTraceQuery(task, summary.attempt_id, seen),
    meta: { inlineError: true },
  })
  const full =
    query.data?.state === 'available' && query.data.steps.length > 0
      ? query.data
      : null
  const notFound = apiErrorCode(query.error) === traceNotFoundCode
  return (
    <div ref={ref} className='flex min-w-0 flex-col gap-1.5'>
      {full ? (
        <FullSteps steps={full.steps} icon={icon} />
      ) : (
        <SummarySteps
          summary={summary}
          loading={query.isPending && !query.isError}
          icon={icon}
        />
      )}
      {query.isError && (
        <p className='m-0 flex items-center gap-1 text-xs text-danger'>
          {notFound ? '执行详情已过期' : '执行详情加载失败'}
          {!notFound && (
            <Button
              size='sm'
              variant='tertiary'
              className='h-6 min-w-0 px-1.5 text-xs'
              isPending={query.isFetching}
              onPress={() => void query.refetch()}
            >
              重试
            </Button>
          )}
        </p>
      )}
    </div>
  )
}

/** Label, merged count, subject and duration on one line. */
function StepHeading({
  label,
  outcome,
  durationMs,
  count = 1,
  subject = '',
}: {
  label: string
  outcome: string
  durationMs: number
  count?: number
  subject?: string
}) {
  return (
    <div className='flex min-w-0 items-baseline gap-2 text-[13px] leading-5'>
      <span
        className={cn(
          'min-w-0 shrink truncate',
          outcome === 'failed' && 'font-medium text-danger',
          (outcome === 'skipped' || outcome === 'interrupted') && 'text-muted'
        )}
      >
        {label}
      </span>
      {count > 1 && (
        <span className='shrink-0 text-xs text-muted tabular-nums'>
          ×{count}
        </span>
      )}
      {subject && (
        <span className='max-w-[45%] min-w-0 shrink-[2] truncate font-mono text-muted'>
          <RedactedText value={subject} />
        </span>
      )}
      <span className='ms-auto shrink-0 ps-2 text-xs text-muted tabular-nums'>
        {formatTraceDuration(durationMs)}
      </span>
    </div>
  )
}

/** Timeline from the history summary while the full trace is not here yet:
 * steps that made calls or failed hold a skeleton where details will go. */
function SummarySteps({
  summary,
  loading,
  icon,
}: {
  summary: Summary
  loading: boolean
  icon: ReactNode
}) {
  return (
    <Timeline aria-label='执行步骤' size='sm' density='compact'>
      {summary.steps.map((step, position) => (
        <Timeline.Item
          key={position}
          status={known(stepOutcomes, step.outcome)?.tone ?? 'muted'}
          className='min-w-0'
        >
          <Timeline.Marker
            role='img'
            aria-hidden={false}
            aria-label={
              known(stepOutcomes, step.outcome)?.label ??
              (step.outcome || '未知')
            }
          >
            {icon}
          </Timeline.Marker>
          <Timeline.Content className='gap-1.5'>
            <StepHeading
              label={step.label || step.code}
              outcome={step.outcome}
              durationMs={step.duration_ms}
              count={step.count}
            />
            {loading && (step.call_count > 0 || step.outcome === 'failed') && (
              <Skeleton className='h-5 w-3/5 rounded-md' />
            )}
          </Timeline.Content>
        </Timeline.Item>
      ))}
    </Timeline>
  )
}

function FullSteps({
  steps,
  icon,
}: {
  steps: TaskAttemptTrace['steps']
  icon: ReactNode
}) {
  return (
    <Timeline aria-label='执行步骤' size='sm' density='compact'>
      {steps.map((step, position) =>
        isInvalid(step) ? (
          <Timeline.Item key={position} status='muted' className='min-w-0'>
            <Timeline.Marker>{icon}</Timeline.Marker>
            <Timeline.Content>
              <span className='text-xs leading-5 text-muted'>无法显示</span>
            </Timeline.Content>
          </Timeline.Item>
        ) : (
          <Timeline.Item
            key={position}
            status={known(stepOutcomes, step.outcome)?.tone ?? 'muted'}
            className='min-w-0'
          >
            <Timeline.Marker
              role='img'
              aria-hidden={false}
              aria-label={
                known(stepOutcomes, step.outcome)?.label ??
                (step.outcome || '未知')
              }
            >
              {icon}
            </Timeline.Marker>
            <Timeline.Content className='gap-1.5'>
              <StepHeading
                label={step.label || step.code}
                outcome={step.outcome}
                durationMs={step.duration_ms}
                subject={step.subject}
              />
              <StepBody step={step} />
            </Timeline.Content>
          </Timeline.Item>
        )
      )}
    </Timeline>
  )
}

/** A step of another task (a certificate waiting for its zone's DNS sync)
 * links to that task's detail sheet. */
function StepBody({ step }: { step: TraceStep }) {
  const related = step.related_task
  return (
    <>
      {step.error && (
        <p className='m-0 text-xs leading-5 break-words whitespace-pre-wrap text-danger'>
          <RedactedText value={step.error} />
        </p>
      )}
      {related?.resource_type && related.resource_id && (
        <Link
          className='w-fit text-xs'
          href={taskHref({
            resourceType: related.resource_type,
            resourceId: related.resource_id,
          })}
        >
          关联任务：{related.label || '查看任务'}
        </Link>
      )}
      {step.calls.length > 0 && (
        <ul className='m-0 flex list-none flex-col gap-1.5 p-0'>
          {step.calls.map((call, position) => (
            <li key={position} className='flex min-w-0 flex-col gap-1'>
              {isInvalid(call) ? (
                <span className='text-xs text-muted'>无法显示</span>
              ) : (
                <TraceCallItem call={call} />
              )}
            </li>
          ))}
        </ul>
      )}
      {step.logs.length > 0 && <LogLines logs={step.logs} />}
    </>
  )
}

function KindChip({ children }: { children: ReactNode }) {
  return (
    <Chip
      size='sm'
      variant='secondary'
      className='h-5 shrink-0 px-1.5 font-mono text-[11px]'
    >
      {children}
    </Chip>
  )
}

// Compact monospace block; long content scrolls inside it.
const codeBlock =
  'm-0 max-h-48 overflow-auto rounded-md bg-default px-2 py-1.5 font-mono text-[11px] leading-4'

/** Short Chinese text of a call that got no HTTP status or DNS answer. */
function TransportFailure({ call }: { call: TraceCall }) {
  const code = call.http?.transport_error || call.error
  return (
    <span className='text-danger' title={code || undefined}>
      {known(transportErrors, code) ??
        known(callOutcomes, call.outcome) ??
        '请求失败'}
    </span>
  )
}

/** One call: a single line (kind, target, provider ids, result, duration,
 * folds, copy of the full URL), then its bodies, answers or fields. */
function TraceCallItem({ call }: { call: TraceCall }) {
  const { http, dns, node_message: message } = call
  let kind: ReactNode = call.kind || '调用'
  let target: ReactNode = call.operation
  let result: ReactNode =
    call.outcome && call.outcome !== 'ok' ? (
      <TransportFailure call={call} />
    ) : null
  if (message) {
    kind =
      message.direction === 'sent'
        ? '发送'
        : message.direction === 'received'
          ? '接收'
          : message.direction || '消息'
    target = message.message
    result =
      message.object_count > 0 ? (
        <span className='text-muted'>{message.object_count} 个对象</span>
      ) : null
  } else if (dns) {
    kind = 'DNS'
    target = `${dns.resolver ? `${dns.resolver} · ` : ''}${dns.name} ${dns.type}`
    result = dns.rcode ? (
      <span
        className={dns.rcode === 'NOERROR' ? 'text-success' : 'text-danger'}
      >
        {dns.rcode}
      </span>
    ) : (
      <TransportFailure call={call} />
    )
  } else if (http) {
    // A DNS query that failed in transport may carry only its DoH request.
    kind = call.kind === 'dns' ? 'DNS' : http.method || 'HTTP'
    target = <RedactedText value={httpTarget(http)} />
    result = http.status ? (
      <span className={toneText[httpStatusTone(http.status)]}>
        {http.status}
      </span>
    ) : (
      <TransportFailure call={call} />
    )
  }
  const fields =
    message?.fields.filter((field) => !isConfigVersionField(field.name)) ?? []
  // A DNS query shows its answers, not the DoH bodies behind them.
  const requestBody = (!dns && http?.request_body) || null
  const responseBody = (!dns && http?.response_body) || null
  return (
    <>
      <div className='flex min-h-7 min-w-0 items-center gap-2 text-[13px]'>
        <KindChip>{kind}</KindChip>
        <span className='min-w-0 flex-1 truncate font-mono'>{target}</span>
        {result && (
          <span className='shrink-0 whitespace-nowrap tabular-nums'>
            {result}
          </span>
        )}
        <span className='shrink-0 whitespace-nowrap text-muted tabular-nums'>
          {formatTraceDuration(call.duration_ms)}
        </span>
        {call.repeat_count > 1 && (
          <span className='shrink-0 text-muted tabular-nums'>
            ×{call.repeat_count}
          </span>
        )}
        {http?.url && <CopyButton value={http.url} label='复制请求地址' />}
      </div>
      {(call.provider_request_id || call.provider_code) && (
        <p className='m-0 flex min-w-0 flex-wrap gap-x-3 font-mono text-[13px] text-muted'>
          {call.provider_request_id && (
            <span className='break-all'>{call.provider_request_id}</span>
          )}
          {call.provider_code && (
            <span className='break-all text-danger'>{call.provider_code}</span>
          )}
        </p>
      )}
      {requestBody && <BodyExcerpt label='请求' body={requestBody} />}
      {responseBody && (
        <BodyExcerpt label={requestBody ? '响应' : ''} body={responseBody} />
      )}
      {dns && dns.answers.length > 0 && (
        <ol className={cn(codeBlock, 'list-none')}>
          {dns.answers.map((answer, index) => (
            <li key={index} className='break-all'>
              {answer.name} {answer.type} {answer.ttl}{' '}
              <RedactedText value={answer.data} />
            </li>
          ))}
        </ol>
      )}
      {fields.length > 0 && (
        <p className='m-0 flex min-w-0 flex-wrap gap-x-3 font-mono text-[11px] leading-4'>
          {fields.map((field, index) => (
            <span key={index} className='min-w-0 break-all'>
              <span className='text-muted'>{field.name}=</span>
              <RedactedText value={field.value} />
            </span>
          ))}
        </p>
      )}
    </>
  )
}

/** Request/response excerpt as stored. Empty bodies have no caption. */
function BodyExcerpt({ label, body }: { label: string; body: TraceBody }) {
  if (!body.excerpt) return null
  return (
    <div className='flex min-w-0 flex-col gap-0.5'>
      {label && <span className='text-[11px] text-muted'>{label}</span>}
      <pre className={cn(codeBlock, 'whitespace-pre')}>
        <RedactedText value={body.excerpt} />
      </pre>
    </div>
  )
}

function LogLines({ logs }: { logs: (TraceLog | InvalidItem)[] }) {
  return (
    <ol className={cn(codeBlock, 'flex list-none flex-col')}>
      {logs.map((log, index) =>
        isInvalid(log) ? (
          <li key={index} className='font-sans text-muted'>
            无法显示
          </li>
        ) : (
          <LogLine key={index} log={log} />
        )
      )}
    </ol>
  )
}

function LogLine({ log }: { log: TraceLog }) {
  const level = known(logLevels, log.level)
  return (
    <li className='flex min-w-0 gap-2'>
      <time dateTime={log.at} className='shrink-0 text-muted tabular-nums'>
        {formatTraceClock(log.at) || '—'}
      </time>
      <span
        className={cn('shrink-0 font-sans', toneText[level?.tone ?? 'warning'])}
      >
        {level?.label ?? (log.level || '未知')}
      </span>
      <span className='min-w-0 break-words whitespace-pre-wrap'>
        <RedactedText value={log.message} />
        {log.repeat_count > 1 && (
          <span className='ms-1 text-muted'>×{log.repeat_count}</span>
        )}
      </span>
    </li>
  )
}

/** Text with every `[REDACTED]` marker shown as an "已隐藏" badge. */
export function RedactedText({ value }: { value: string }) {
  return splitRedacted(value).map((part, index) =>
    part === null ? (
      <Chip
        key={index}
        size='sm'
        variant='soft'
        className='mx-0.5 h-4 gap-0.5 px-1 align-middle font-sans text-[11px] leading-4 font-normal text-muted'
      >
        <EyeSlash aria-hidden className='size-3' />
        <Chip.Label className='px-0'>已隐藏</Chip.Label>
      </Chip>
    ) : (
      <Fragment key={index}>{part}</Fragment>
    )
  )
}
