import { Fragment, useState, type ReactNode } from 'react'
import { useQuery } from '@tanstack/react-query'
import {
  ArrowDownLeft,
  ArrowUpRight,
  ChevronRight,
  CircleCheck,
  CircleDashed,
  CircleMinus,
  CirclePause,
  CircleXmark,
  EyeSlash,
} from '@gravity-ui/icons'
import { Segment } from '@heroui-pro/react'
import {
  Accordion,
  Button,
  Chip,
  Disclosure,
  Link,
  Skeleton,
  Table,
} from '@heroui/react'
import { ApiBusinessError } from '@/lib/api-response'
import { formatBytes } from '@/lib/format'
import { cn } from '@/lib/utils'
import { CopyButton } from '@/components/copy-button'
import { QueryNotice } from '@/components/page'
import { taskAttemptTraceQuery } from './data'
import { taskHref } from './task-display'
import {
  bodyFormats,
  callOutcomes,
  consoleTimeZoneLabel,
  formatTraceClock,
  formatTraceTime,
  formatTraceDuration,
  httpStatusTone,
  httpTarget,
  logLevels,
  omittedReasons,
  splitRedacted,
  isConfigVersionField,
  taskNotFoundCode,
  stepHasDetail,
  stepOutcomes,
  transportErrors,
  workerTypes,
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
  type TracePair,
  type TraceStep,
} from './types'

const toneText = {
  success: 'text-success',
  warning: 'text-warning',
  danger: 'text-danger',
  accent: 'text-accent',
  default: 'text-muted',
} as const

/** Trace of one attempt inside its history row: the step strip from the
 * history summary, and on demand the full trace read once from its own
 * one-shot snapshot (only while expanded; collapsing cancels a pending read).
 * `trace: null` (a backend without traces) renders nothing. */
export function AttemptTrace({
  taskId,
  trace,
}: {
  taskId: string
  trace: TaskAttemptTraceSummary
}) {
  const [expanded, setExpanded] = useState(false)
  if (trace === null) return null
  if (isInvalid(trace))
    return (
      <p className='m-0 text-xs text-muted'>
        执行详情无法显示：数据格式无法识别。
      </p>
    )
  if (trace.state !== 'available' || !trace.attempt_id)
    return <p className='m-0 text-xs text-muted'>此次尝试没有执行详情</p>
  const hiddenSteps =
    trace.step_count -
    trace.steps.reduce((total, step) => total + step.count, 0)
  return (
    <div className='flex min-w-0 flex-col gap-1'>
      {trace.failed_step && (
        <p className='m-0 truncate text-xs text-danger'>
          失败步骤：{trace.failed_step.label || trace.failed_step.code}
        </p>
      )}
      {trace.steps.length > 0 && (
        <ol
          aria-label='执行步骤'
          className='m-0 flex list-none items-center gap-x-1 overflow-x-auto p-0 pb-0.5 text-xs whitespace-nowrap'
        >
          {trace.steps.map((step, index) => {
            const failed = step.outcome === 'failed'
            return (
              <li
                key={index}
                className={cn(
                  'flex shrink-0 items-center gap-1',
                  failed ? 'text-danger' : 'text-muted'
                )}
              >
                {index > 0 && (
                  <ChevronRight aria-hidden className='size-3 text-muted' />
                )}
                <StepOutcomeIcon outcome={step.outcome} />
                <span
                  className={failed ? 'font-medium' : 'text-foreground'}
                  title={step.code}
                >
                  {step.label || step.code}
                </span>
                {step.count > 1 && (
                  <span className='tabular-nums'>×{step.count}</span>
                )}
                <span className='tabular-nums'>
                  {formatTraceDuration(step.duration_ms)}
                </span>
              </li>
            )
          })}
          {(trace.truncated || hiddenSteps > 0) && (
            <li className='flex shrink-0 items-center gap-1 text-muted'>
              <ChevronRight aria-hidden className='size-3' />
              {hiddenSteps > 0 ? `另有 ${hiddenSteps} 个步骤` : '更多步骤'}
            </li>
          )}
        </ol>
      )}
      <Disclosure isExpanded={expanded} onExpandedChange={setExpanded}>
        <div className='flex min-w-0 flex-wrap items-center gap-x-2'>
          <Disclosure.Heading>
            <Button
              slot='trigger'
              size='sm'
              variant='ghost'
              className='h-7 px-1.5 text-xs'
            >
              {expanded ? '收起执行详情' : '查看执行详情'}
              <Disclosure.Indicator />
            </Button>
          </Disclosure.Heading>
          <span className='text-xs text-muted tabular-nums'>
            {trace.step_count} 个步骤 · {trace.call_count} 次调用 ·{' '}
            {trace.log_count} 条日志
          </span>
        </div>
        <Disclosure.Content>
          {expanded && (
            <div className='pt-1'>
              <AttemptTraceView taskId={taskId} attemptId={trace.attempt_id} />
            </div>
          )}
        </Disclosure.Content>
      </Disclosure>
    </div>
  )
}

/** Small outcome icon named for assistive technology. It carries its own
 * label instead of a visually hidden (absolutely positioned) span: those
 * resolve against the sheet dialog rather than the scrolling body, enlarge
 * the dialog's hidden overflow and let focus or scroll-into-view move the
 * whole sheet. */
function StepOutcomeIcon({ outcome }: { outcome: string }) {
  const shown = known(stepOutcomes, outcome)
  const Icon =
    outcome === 'completed'
      ? CircleCheck
      : outcome === 'failed'
        ? CircleXmark
        : outcome === 'skipped'
          ? CircleMinus
          : outcome === 'interrupted'
            ? CirclePause
            : CircleDashed
  return (
    <Icon
      role='img'
      aria-label={shown?.label ?? (outcome || '未知结果')}
      className={cn('size-3.5 shrink-0', toneText[shown?.tone ?? 'default'])}
    />
  )
}

/** Full trace of one attempt, read once from its one-shot snapshot. */
export function AttemptTraceView({
  taskId,
  attemptId,
}: {
  taskId: string
  attemptId: string
}) {
  const query = useQuery({
    ...taskAttemptTraceQuery(taskId, attemptId),
    meta: { inlineError: true },
  })
  if (query.isPending)
    return (
      <div
        role='status'
        aria-label='正在加载执行详情'
        className='flex flex-col gap-2 rounded-lg border border-border p-2'
      >
        <Skeleton className='h-4 w-56 rounded-md' />
        {Array.from({ length: 4 }, (_, index) => (
          <Skeleton key={index} className='h-7 w-full rounded-md' />
        ))}
      </div>
    )
  if (query.isError)
    return (
      <QueryNotice
        query={query}
        title={
          query.error instanceof ApiBusinessError &&
          query.error.code === taskNotFoundCode
            ? '执行详情不存在或已过期'
            : '执行详情加载失败'
        }
        onRetry={() => query.refetch()}
      />
    )
  const trace = query.data
  if (trace.state !== 'available')
    return <p className='m-0 text-xs text-muted'>此次尝试没有执行详情</p>
  return (
    <section
      aria-label={`第 ${trace.attempt} 次尝试的执行详情`}
      className='flex min-w-0 flex-col rounded-lg border border-border'
    >
      <p className='m-0 flex flex-wrap gap-x-2 border-b border-border px-2.5 py-1.5 text-xs text-muted'>
        <span>
          {known(workerTypes, trace.worker_type) ??
            (trace.worker_type || '执行者未知')}
        </span>
        {trace.started_at && (
          <span>
            开始于{' '}
            <time
              dateTime={trace.started_at}
              className='font-mono text-foreground tabular-nums'
            >
              {formatTraceTime(trace.started_at)}
            </time>
            （{consoleTimeZoneLabel}）
          </span>
        )}
        <span className='tabular-nums'>
          耗时 {formatTraceDuration(trace.duration_ms)}
        </span>
      </p>
      {trace.steps.length ? (
        <TraceSteps steps={trace.steps} />
      ) : (
        <p className='m-0 px-2.5 py-2 text-xs text-muted'>没有记录到步骤。</p>
      )}
      <CaptureNotes capture={trace.capture} />
    </section>
  )
}

const invalidNotice = (what: string) => `${what}无法显示：数据格式无法识别。`

/** Steps in execution order; each step with calls, logs or an error expands
 * (its body renders only while expanded). */
function TraceSteps({ steps }: { steps: TaskAttemptTrace['steps'] }) {
  const [expandedKeys, setExpandedKeys] = useState<Set<string | number>>(
    () => new Set()
  )
  return (
    <Accordion
      hideSeparator
      allowsMultipleExpanded
      expandedKeys={expandedKeys}
      onExpandedChange={(keys) => setExpandedKeys(new Set(keys))}
      className='flex flex-col divide-y divide-border'
    >
      {steps.map((step, position) => {
        if (isInvalid(step))
          return (
            <p key={position} className='m-0 px-2.5 py-2 text-xs text-muted'>
              {invalidNotice('此步骤')}
            </p>
          )
        const id = String(position)
        if (!stepHasDetail(step))
          return (
            <div key={id} className='flex items-center gap-2 px-2.5 py-2'>
              <StepLine step={step} />
              <span aria-hidden className='size-4 shrink-0' />
            </div>
          )
        return (
          <Accordion.Item key={id} id={id}>
            <Accordion.Heading>
              <Accordion.Trigger className='w-full min-w-0 gap-2 px-2.5 py-2 text-[13px] font-normal'>
                <StepLine step={step} expanded={expandedKeys.has(id)} />
                <Accordion.Indicator />
              </Accordion.Trigger>
            </Accordion.Heading>
            <Accordion.Panel>
              <Accordion.Body className='px-2.5 pt-0 pb-3 text-foreground'>
                {expandedKeys.has(id) && <StepDetail step={step} />}
              </Accordion.Body>
            </Accordion.Panel>
          </Accordion.Item>
        )
      })}
    </Accordion>
  )
}

/** One-line step row; the error preview hides once the step is expanded
 * (its body shows the full error). */
function StepLine({
  step,
  expanded = false,
}: {
  step: TraceStep
  expanded?: boolean
}) {
  const shown = known(stepOutcomes, step.outcome)
  return (
    <span className='flex min-w-0 flex-1 flex-col gap-0.5'>
      <span className='flex min-w-0 items-center gap-2'>
        <span className='w-5 shrink-0 font-mono text-[11px] text-muted tabular-nums'>
          {step.index + 1}
        </span>
        <StepOutcomeIcon outcome={step.outcome} />
        <span
          className={cn(
            'min-w-0 shrink truncate',
            step.outcome === 'failed' && 'font-medium text-danger'
          )}
        >
          {step.label || step.code}
        </span>
        {step.subject && (
          <span className='max-w-[45%] min-w-0 shrink-[2] truncate font-mono text-xs text-muted'>
            <RedactedText value={step.subject} />
          </span>
        )}
        <span className='ms-auto flex shrink-0 items-center gap-2 text-xs text-muted tabular-nums'>
          <span className='hidden sm:inline'>
            +{formatTraceDuration(step.offset_ms)}
          </span>
          <span className='min-w-14 text-end whitespace-nowrap text-foreground'>
            {formatTraceDuration(step.duration_ms)}
          </span>
          <span
            aria-hidden
            className={cn('w-7', toneText[shown?.tone ?? 'default'])}
          >
            {shown?.label ?? '未知'}
          </span>
        </span>
      </span>
      {step.error && !expanded && (
        <span className='truncate ps-7 text-xs text-danger'>
          <RedactedText value={step.error} />
        </span>
      )}
    </span>
  )
}

function StepDetail({ step }: { step: TraceStep }) {
  const related = step.related_task
  return (
    <div className='flex min-w-0 flex-col gap-3'>
      {step.error && <TraceError error={step.error} />}
      <Facts
        items={[
          ['步骤代码', <Code key='code'>{step.code || '—'}</Code>],
          [
            '开始',
            <TimeWithOffset
              key='start'
              at={step.started_at}
              offset={step.offset_ms}
            />,
          ],
          ...(related
            ? ([
                [
                  '关联任务',
                  <Link
                    key='related'
                    href={taskHref({
                      id: related.task_id,
                      resource_id: related.resource_id,
                    })}
                    className='text-xs'
                  >
                    查看关联的 {related.label || '任务'}
                  </Link>,
                ],
              ] satisfies [string, ReactNode][])
            : []),
        ]}
      />
      {(step.calls.length > 0 || step.omitted_calls > 0) && (
        <section className='flex min-w-0 flex-col gap-1'>
          <h5 className='m-0 text-xs font-medium text-muted'>
            外部调用 {step.call_count} 次
          </h5>
          <ul className='m-0 flex list-none flex-col divide-y divide-border rounded-md border border-border p-0'>
            {step.calls.map((call, position) => (
              <TraceCallItem key={position} call={call} />
            ))}
          </ul>
          {step.omitted_calls > 0 && (
            <p className='m-0 text-xs text-muted'>
              另有 {step.omitted_calls} 次调用超出记录上限，未保存。
            </p>
          )}
        </section>
      )}
      {step.logs.length > 0 && <TraceLogs logs={step.logs} />}
    </div>
  )
}

function TraceCallItem({ call }: { call: TraceCall | InvalidItem }) {
  const [expanded, setExpanded] = useState(false)
  if (isInvalid(call))
    return (
      <li className='px-2 py-1.5 text-xs text-muted'>
        {invalidNotice('此调用')}
      </li>
    )
  return (
    <li className='min-w-0'>
      <Disclosure isExpanded={expanded} onExpandedChange={setExpanded}>
        <Disclosure.Heading>
          <Disclosure.Trigger className='flex w-full min-w-0 items-center gap-2 px-2 py-1.5 text-start text-xs hover:bg-default'>
            <CallLine call={call} />
            <Disclosure.Indicator className='size-3.5 text-muted' />
          </Disclosure.Trigger>
        </Disclosure.Heading>
        <Disclosure.Content>
          {expanded && (
            <div className='border-t border-border p-2'>
              <CallDetail call={call} />
            </div>
          )}
        </Disclosure.Content>
      </Disclosure>
    </li>
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

/** One-line call summary: kind/method, target, result, duration, folds. */
function CallLine({ call }: { call: TraceCall }) {
  const { http, dns, node_message: message } = call
  let kind: ReactNode = call.kind || '调用'
  let target: ReactNode = call.operation || call.subject
  let result: ReactNode = null
  if (message) {
    kind =
      message.direction === 'sent'
        ? '发送'
        : message.direction === 'received'
          ? '接收'
          : message.direction || '消息'
    target = message.message
    result = (
      <span className='text-muted'>
        {message.object_count > 0 && `${message.object_count} 个对象 · `}
        {formatBytes(message.bytes)}
      </span>
    )
  } else if (dns) {
    kind = 'DNS'
    target = `${dns.name} ${dns.type}`
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
  return (
    <span className='flex min-w-0 flex-1 items-center gap-2'>
      <KindChip>{kind}</KindChip>
      <span className='min-w-0 flex-1 truncate font-mono'>{target}</span>
      {call.provider_code && (
        <span className='hidden max-w-32 shrink-0 truncate font-mono text-muted sm:inline'>
          {call.provider_code}
        </span>
      )}
      <span className='flex shrink-0 items-center gap-2 tabular-nums'>
        <span className='min-w-8 text-end whitespace-nowrap'>{result}</span>
        <span className='min-w-14 text-end whitespace-nowrap text-muted'>
          {formatTraceDuration(call.duration_ms)}
        </span>
        {/* Fixed slot so folded and single calls keep their columns. */}
        <span
          className='w-8 text-muted'
          title={
            call.repeat_count > 1
              ? `相同请求共 ${call.repeat_count} 次`
              : undefined
          }
        >
          {call.repeat_count > 1 && `×${call.repeat_count}`}
        </span>
      </span>
    </span>
  )
}

/** Short Chinese text of a call that got no HTTP status. */
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

function CallDetail({ call }: { call: TraceCall }) {
  const { http, dns, node_message: message } = call
  const transport = call.http?.transport_error || call.error
  return (
    <div className='flex min-w-0 flex-col gap-3'>
      <Facts
        items={[
          ['操作', <Code key='operation'>{call.operation || '—'}</Code>],
          ...(call.subject
            ? ([
                [
                  '对象',
                  <Code key='subject'>
                    <RedactedText value={call.subject} />
                  </Code>,
                ],
              ] satisfies [string, ReactNode][])
            : []),
          [
            '开始',
            <TimeWithOffset
              key='start'
              at={call.started_at}
              offset={call.offset_ms}
            />,
          ],
          [
            '耗时',
            call.repeat_count > 1
              ? `${formatTraceDuration(call.duration_ms)}；相同请求共 ${call.repeat_count} 次，合计 ${formatTraceDuration(call.repeat_total_ms)}${call.last_started_at ? `，最后一次 ${formatTraceClock(call.last_started_at)}` : ''}`
              : formatTraceDuration(call.duration_ms),
          ],
          [
            '结果',
            <span key='outcome' className='flex flex-wrap gap-x-2'>
              <span
                className={
                  call.outcome === 'ok' ? 'text-foreground' : 'text-danger'
                }
              >
                {known(callOutcomes, call.outcome) ?? (call.outcome || '未知')}
              </span>
              {transport && (
                <span className='text-danger'>
                  {known(transportErrors, transport) ?? (
                    <RedactedText value={transport} />
                  )}
                  {known(transportErrors, transport) && (
                    <Code className='ms-1 text-muted'>{transport}</Code>
                  )}
                </span>
              )}
            </span>,
          ],
          ...(call.provider_request_id
            ? ([
                [
                  '请求 ID',
                  <span
                    key='request'
                    className='flex min-w-0 items-center gap-1'
                  >
                    <Code className='truncate'>{call.provider_request_id}</Code>
                    <CopyButton
                      value={call.provider_request_id}
                      label='复制请求 ID'
                    />
                  </span>,
                ],
              ] satisfies [string, ReactNode][])
            : []),
          ...(call.provider_code
            ? ([
                ['错误码', <Code key='code'>{call.provider_code}</Code>],
              ] satisfies [string, ReactNode][])
            : []),
        ]}
      />
      {message && <NodeMessageDetail message={message} />}
      {dns && <DnsDetail dns={dns} />}
      {http && <HttpDetail http={http} />}
    </div>
  )
}

function HttpDetail({ http }: { http: NonNullable<TraceCall['http']> }) {
  return (
    <>
      <DetailSection title='请求'>
        <CodeBlock
          label={`${http.method || 'HTTP'} 地址`}
          value={http.url}
          copyLabel='复制请求地址'
          wrap
        />
        {http.query.length > 0 && (
          <PairList title='查询参数' pairs={http.query} />
        )}
        {http.request_headers.length > 0 && (
          <PairList title='请求头' pairs={http.request_headers} />
        )}
        {http.request_body && (
          <BodyBlock title='请求正文' body={http.request_body} />
        )}
      </DetailSection>
      <DetailSection title={http.status ? `响应 ${http.status}` : '响应'}>
        {http.status === 0 && (
          <p className='m-0 text-xs text-danger'>
            没有收到响应
            {http.transport_error &&
              `：${known(transportErrors, http.transport_error) ?? http.transport_error}`}
          </p>
        )}
        {http.response_headers.length > 0 && (
          <PairList title='响应头' pairs={http.response_headers} />
        )}
        {http.response_body && (
          <BodyBlock title='响应正文' body={http.response_body} />
        )}
      </DetailSection>
    </>
  )
}

function DnsDetail({ dns }: { dns: NonNullable<TraceCall['dns']> }) {
  return (
    <DetailSection title='DNS 查询'>
      <Facts
        items={[
          ['解析服务器', <Code key='resolver'>{dns.resolver || '—'}</Code>],
          ['方式', <Code key='transport'>{dns.transport || '—'}</Code>],
          ['查询', <Code key='name'>{`${dns.name} ${dns.type}`}</Code>],
          [
            '应答码',
            dns.rcode ? (
              <Code
                key='rcode'
                className={
                  dns.rcode === 'NOERROR' ? 'text-success' : 'text-danger'
                }
              >
                {dns.rcode}
              </Code>
            ) : (
              <span key='rcode' className='text-danger'>
                查询失败
              </span>
            ),
          ],
        ]}
      />
      {dns.answers.length > 0 ? (
        <Table
          variant='secondary'
          className='[&_td]:px-2 [&_td]:py-1.5 [&_td]:text-xs [&_th]:px-2 [&_th]:py-1.5'
        >
          <Table.ScrollContainer>
            <Table.Content aria-label='DNS 应答' className='min-w-[360px]'>
              <Table.Header>
                <Table.Column isRowHeader>名称</Table.Column>
                <Table.Column>类型</Table.Column>
                <Table.Column>TTL</Table.Column>
                <Table.Column>数据</Table.Column>
              </Table.Header>
              <Table.Body>
                {dns.answers.map((answer, index) => (
                  <Table.Row key={index}>
                    <Table.Cell>
                      <Code>{answer.name}</Code>
                    </Table.Cell>
                    <Table.Cell>
                      <Code>{answer.type}</Code>
                    </Table.Cell>
                    <Table.Cell>
                      <span className='tabular-nums'>{answer.ttl}</span>
                    </Table.Cell>
                    <Table.Cell>
                      <Code className='break-all'>
                        <RedactedText value={answer.data} />
                      </Code>
                    </Table.Cell>
                  </Table.Row>
                ))}
              </Table.Body>
            </Table.Content>
          </Table.ScrollContainer>
        </Table>
      ) : (
        <p className='m-0 text-xs text-muted'>没有应答记录。</p>
      )}
      {dns.truncated && (
        <p className='m-0 text-xs text-muted'>
          应答超出记录上限，只列出前 16 条。
        </p>
      )}
    </DetailSection>
  )
}

/** Node protocol message; release identifiers among its fields stay hidden
 * (the console shows no config version identifiers). */
export function NodeMessageDetail({
  message,
}: {
  message: NonNullable<TraceCall['node_message']>
}) {
  const fields = message.fields.filter(
    (field) => !isConfigVersionField(field.name)
  )
  return (
    <DetailSection title='节点消息'>
      <Facts
        items={[
          [
            '方向',
            <span key='direction' className='flex items-center gap-1'>
              {message.direction === 'sent' ? (
                <ArrowUpRight aria-hidden className='size-3.5 text-muted' />
              ) : message.direction === 'received' ? (
                <ArrowDownLeft aria-hidden className='size-3.5 text-muted' />
              ) : null}
              {message.direction === 'sent'
                ? '发送（控制面 → 节点）'
                : message.direction === 'received'
                  ? '接收（节点 → 控制面）'
                  : message.direction || '未知'}
            </span>,
          ],
          ['消息', <Code key='message'>{message.message || '—'}</Code>],
          ...(message.request_id
            ? ([
                ['请求标识', <Code key='request'>{message.request_id}</Code>],
              ] satisfies [string, ReactNode][])
            : []),
          ...(message.object_count > 0
            ? ([['对象数', `${message.object_count} 个`]] satisfies [
                string,
                ReactNode,
              ][])
            : []),
          ['大小', formatBytes(message.bytes)],
        ]}
      />
      {fields.length > 0 && (
        <PairList
          title='字段'
          pairs={fields.map((field) => ({ ...field, redacted: false }))}
        />
      )}
    </DetailSection>
  )
}

function TraceLogs({ logs }: { logs: (TraceLog | InvalidItem)[] }) {
  const [filter, setFilter] = useState<'all' | 'notable'>('all')
  const many = logs.length > 8
  const shown =
    filter === 'all'
      ? logs
      : logs.filter((log) => isInvalid(log) || log.level !== 'info')
  const copied = logs
    .flatMap((log) =>
      isInvalid(log)
        ? []
        : [
            `${formatTraceClock(log.at)} ${log.level.toUpperCase()} ${log.message}${log.repeat_count > 1 ? ` ×${log.repeat_count}` : ''}`,
          ]
    )
    .join('\n')
  return (
    <section className='flex min-w-0 flex-col gap-1'>
      <div className='flex min-w-0 flex-wrap items-center justify-between gap-2'>
        <h5 className='m-0 text-xs font-medium text-muted'>
          日志 {logs.length} 条
        </h5>
        <span className='flex items-center gap-1'>
          {many && (
            <Segment
              size='sm'
              aria-label='筛选日志'
              selectedKey={filter}
              onSelectionChange={(key) => setFilter(key as 'all' | 'notable')}
            >
              <Segment.Item id='all'>全部</Segment.Item>
              <Segment.Item id='notable'>警告与错误</Segment.Item>
            </Segment>
          )}
          <CopyButton value={copied} label='复制日志' />
        </span>
      </div>
      {shown.length ? (
        <ol className='m-0 flex list-none flex-col gap-0.5 overflow-x-auto rounded-md bg-surface-secondary p-2 font-mono text-xs leading-5'>
          {shown.map((log, index) =>
            isInvalid(log) ? (
              <li key={index} className='font-sans text-muted'>
                {invalidNotice('此日志')}
              </li>
            ) : (
              <LogLine key={index} log={log} />
            )
          )}
        </ol>
      ) : (
        <p className='m-0 text-xs text-muted'>没有警告或错误日志。</p>
      )}
    </section>
  )
}

function LogLine({ log }: { log: TraceLog }) {
  const level = known(logLevels, log.level)
  return (
    <li className='grid grid-cols-[auto_minmax(1.75rem,auto)_minmax(0,1fr)] gap-x-2 sm:grid-cols-[auto_5.5rem_minmax(1.75rem,auto)_minmax(0,1fr)]'>
      <time dateTime={log.at} className='text-muted tabular-nums'>
        {formatTraceClock(log.at) || '—'}
      </time>
      <span className='hidden text-end text-muted tabular-nums sm:inline'>
        +{formatTraceDuration(log.offset_ms)}
      </span>
      <span className={cn('font-sans', toneText[level?.tone ?? 'warning'])}>
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

function CaptureNotes({ capture }: { capture: TaskAttemptTrace['capture'] }) {
  const omitted = [
    capture.omitted_steps > 0 && `${capture.omitted_steps} 个步骤`,
    capture.omitted_calls > 0 && `${capture.omitted_calls} 次调用`,
    capture.omitted_logs > 0 && `${capture.omitted_logs} 条日志`,
    capture.omitted_body_bytes > 0 &&
      `${formatBytes(capture.omitted_body_bytes)} 正文`,
  ].filter(Boolean)
  if (!capture.degraded && !capture.truncated && capture.redactions === 0)
    return null
  return (
    <div className='flex flex-col gap-0.5 border-t border-border px-2.5 py-1.5 text-xs text-muted'>
      {capture.degraded && (
        <p className='m-0 text-warning'>
          记录中途出错，之后的内容未采集；任务结果不受影响。
        </p>
      )}
      {capture.truncated && (
        <p className='m-0'>
          {omitted.length
            ? `超出记录上限，省略了 ${omitted.join('、')}。`
            : '超出记录上限，部分内容已省略。'}
        </p>
      )}
      {capture.redactions > 0 && (
        <p className='m-0 flex items-center gap-1'>
          <EyeSlash aria-hidden className='size-3.5' />
          已隐藏 {capture.redactions} 处敏感内容
        </p>
      )}
    </div>
  )
}

function DetailSection({
  title,
  children,
}: {
  title: string
  children: ReactNode
}) {
  return (
    <section className='flex min-w-0 flex-col gap-1.5'>
      <h6 className='m-0 text-xs font-medium text-foreground'>{title}</h6>
      {children}
    </section>
  )
}

/** Label/value grid for scalar facts. */
function Facts({ items }: { items: [string, ReactNode][] }) {
  return (
    <dl className='m-0 grid grid-cols-[auto_minmax(0,1fr)] gap-x-3 gap-y-1 text-xs'>
      {items.map(([label, value]) => (
        <div key={label} className='contents'>
          <dt className='text-muted'>{label}</dt>
          <dd className='m-0 min-w-0 break-words text-foreground'>{value}</dd>
        </div>
      ))}
    </dl>
  )
}

function Code({
  children,
  className,
}: {
  children: ReactNode
  className?: string
}) {
  return (
    <span className={cn('font-mono text-xs break-all', className)}>
      {children}
    </span>
  )
}

function TimeWithOffset({ at, offset }: { at: string; offset: number }) {
  return (
    <span className='tabular-nums'>
      {at && (
        <time dateTime={at} className='font-mono'>
          {formatTraceClock(at)}
        </time>
      )}
      <span className='text-muted'>
        {at ? '（' : ''}尝试开始后 {formatTraceDuration(offset)}
        {at ? '）' : ''}
      </span>
    </span>
  )
}

/** Name/value pairs (query parameters, headers, message fields). */
function PairList({ title, pairs }: { title: string; pairs: TracePair[] }) {
  return (
    <div className='flex min-w-0 flex-col gap-1'>
      <h6 className='m-0 text-[11px] font-normal text-muted'>{title}</h6>
      <dl className='m-0 grid grid-cols-[minmax(0,max-content)_minmax(0,1fr)] gap-x-3 gap-y-0.5 rounded-md bg-surface-secondary px-2 py-1.5 font-mono text-xs'>
        {pairs.map((pair, index) => (
          <Fragment key={index}>
            <dt className='max-w-48 truncate text-muted' title={pair.name}>
              {pair.name}
            </dt>
            <dd className='m-0 min-w-0 break-all'>
              <RedactedText value={pair.value} />
            </dd>
          </Fragment>
        ))}
      </dl>
    </div>
  )
}

/** Request/response body excerpt: pretty-printed as stored, scrolling inside
 * its own box; omitted bodies explain why. */
function BodyBlock({ title, body }: { title: string; body: TraceBody }) {
  const omitted =
    body.format === 'omitted' || body.omitted_reason !== ''
      ? (known(omittedReasons, body.omitted_reason) ??
        (body.omitted_reason
          ? `未保存（${body.omitted_reason}）`
          : '正文未保存'))
      : null
  return (
    <div className='flex min-w-0 flex-col gap-1'>
      <div className='flex min-w-0 items-center gap-2 text-[11px] text-muted'>
        <span>{title}</span>
        <span className='min-w-0 truncate'>
          {[
            known(bodyFormats, body.format) ?? body.format,
            body.content_type,
            body.bytes > 0 && formatBytes(body.bytes),
          ]
            .filter(Boolean)
            .join(' · ')}
        </span>
        {!omitted && body.excerpt && (
          <span className='ms-auto'>
            <CopyButton value={body.excerpt} label={`复制${title}`} />
          </span>
        )}
      </div>
      {omitted ? (
        <p className='m-0 rounded-md bg-surface-secondary px-2 py-1.5 text-xs text-muted'>
          {omitted}
        </p>
      ) : body.excerpt ? (
        <pre className='m-0 max-h-80 overflow-auto rounded-md bg-surface-secondary p-2 font-mono text-xs leading-5 whitespace-pre'>
          <RedactedText value={body.excerpt} />
        </pre>
      ) : (
        <p className='m-0 text-xs text-muted'>正文为空。</p>
      )}
      {body.truncated && (
        <p className='m-0 text-[11px] text-muted'>
          摘录已截断，原始正文 {formatBytes(body.bytes)}。
        </p>
      )}
    </div>
  )
}

/** Monospace value in a scrolling box with a copy button (full URLs). */
function CodeBlock({
  label,
  value,
  copyLabel,
  wrap = false,
}: {
  label: string
  value: string
  copyLabel: string
  wrap?: boolean
}) {
  return (
    <div className='flex min-w-0 flex-col gap-1'>
      <div className='flex items-center gap-2 text-[11px] text-muted'>
        <span>{label}</span>
        <span className='ms-auto'>
          <CopyButton value={value} label={copyLabel} />
        </span>
      </div>
      <pre
        className={cn(
          'm-0 max-h-40 overflow-auto rounded-md bg-surface-secondary p-2 font-mono text-xs leading-5',
          wrap ? 'break-all whitespace-pre-wrap' : 'whitespace-pre'
        )}
      >
        <RedactedText value={value || '—'} />
      </pre>
    </div>
  )
}

/** Error text of a step: danger, wrapped, copyable. */
function TraceError({ error }: { error: string }) {
  return (
    <div className='flex min-w-0 items-start gap-1 rounded-lg bg-danger-soft px-2.5 py-1.5'>
      <p className='m-0 min-w-0 flex-1 text-xs leading-5 break-words whitespace-pre-wrap text-danger'>
        <RedactedText value={error} />
      </p>
      <CopyButton value={error} label='复制错误信息' />
    </div>
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
