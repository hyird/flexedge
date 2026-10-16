import { useState, type ReactNode } from 'react'
import { useQuery } from '@tanstack/react-query'
import { ListCheck } from '@gravity-ui/icons'
import { Button, Disclosure, Skeleton } from '@heroui/react'
import { useDisplaySeconds } from '@/lib/display-clock'
import { formatDate, parseDate } from '@/lib/format'
import { cn } from '@/lib/utils'
import { navigationItemFor } from '@/components/layout/navigation'
import { CopyButton } from '@/components/copy-button'
import { EmptyState, QueryNotice } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { subtaskHistoryQuery } from './data'
import { TaskAttemptDetail } from './task-attempt-detail'
import { AttemptTimeline, RedactedText } from './task-attempt-trace'
import {
  formatAttemptDuration,
  formatRetryCountdown,
  groupAttempts,
} from './task-history'
import { known, type SubtaskHistory, type TaskAttempt } from './types'

const outcomes = {
  completed: { label: '成功', tone: 'success' },
  failed: { label: '失败', tone: 'danger' },
} as const
const outcomeLabels = [outcomes.completed.label, outcomes.failed.label]
// One attempt (or folded run) per row, separated by a hairline.
const attemptRow = 'flex min-w-0 flex-col gap-2 py-3 first:pt-0 last:pb-0'
const resourceMenuPaths: Record<string, string> = {
  website: '/websites',
  certificate: '/certificates',
  dns_zone: '/dns-zones',
  provider: '/providers',
  node: '/nodes',
}

/** 执行记录 of the expanded subtask: the `history` event of the task detail
 * stream, kept current by its `attempt` events (no refresh button; the
 * stream recovers by itself). Each attempt shows its execution timeline;
 * full traces are read per attempt once on screen. `showErrors` is off while
 * the sheet already reports the shared stream's failure. */
export function SubtaskHistoryPanel({
  operationId,
  subtaskId,
  resourceType,
  showErrors = true,
}: {
  operationId: string
  subtaskId: string
  resourceType: string
  showErrors?: boolean
}) {
  const Icon =
    navigationItemFor(resourceMenuPaths[resourceType] ?? '/tasks')?.icon ??
    ListCheck
  const history = useQuery({
    ...subtaskHistoryQuery(operationId, subtaskId),
    meta: { inlineError: true },
  })
  const data = history.data
  return (
    <div className='flex min-w-0 flex-col gap-3'>
      {showErrors && (
        <QueryNotice
          query={history}
          title='执行记录加载失败'
          onRetry={() => history.refetch()}
        />
      )}
      {data ? (
        <>
          <HistorySummary summary={data.summary} />
          <AttemptList
            operationId={operationId}
            history={data}
            icon={<Icon aria-hidden />}
          />
        </>
      ) : (
        history.isPending && (
          <div
            role='status'
            aria-label='正在加载执行记录'
            className='flex flex-col gap-3'
          >
            <Skeleton className='h-[10.125rem] w-full rounded-xl sm:h-28' />
            {Array.from({ length: 2 }, (_, index) => (
              <Skeleton key={index} className='h-16 w-full rounded-lg' />
            ))}
          </div>
        )
      )}
    </div>
  )
}

const summaryValue = 'text-[13px] leading-5 font-medium tabular-nums'

function HistorySummary({ summary }: { summary: SubtaskHistory['summary'] }) {
  return (
    <dl className='grid grid-cols-2 gap-x-4 gap-y-3 rounded-xl bg-surface-secondary p-3 sm:grid-cols-3'>
      <SummaryItem label='累计尝试'>
        <span className={summaryValue}>{summary.attempt_count}</span>
      </SummaryItem>
      <SummaryItem label='失败次数'>
        <span
          className={cn(
            summaryValue,
            summary.failure_count > 0 && 'text-danger'
          )}
        >
          {summary.failure_count}
        </span>
      </SummaryItem>
      <SummaryItem label='下次重试'>
        {summary.next_attempt_at ? (
          <RetryCountdown
            value={summary.next_attempt_at}
            className={cn(summaryValue, 'text-warning')}
          />
        ) : (
          <span className={cn(summaryValue, 'text-muted')}>—</span>
        )}
      </SummaryItem>
      <SummaryItem label='首次失败'>
        <SummaryTime value={summary.first_failure_at} />
      </SummaryItem>
      <SummaryItem label='最近成功'>
        <SummaryTime value={summary.last_success_at} />
      </SummaryItem>
    </dl>
  )
}

/** Countdown to a scheduled retry on the shared 1 s display clock. It only
 * re-renders this label and never reads data. `note` words it for a subtask
 * row (`12 秒后重试`, `即将重试`). */
export function RetryCountdown({
  value,
  note = false,
  className,
}: {
  value: string
  note?: boolean
  className?: string
}) {
  const now = useDisplaySeconds()
  const at = parseDate(value)?.getTime()
  let label = at === undefined ? '—' : formatRetryCountdown(at, now)
  if (note && at !== undefined) label = at <= now ? '即将重试' : `${label}重试`
  return (
    <time dateTime={value} title={formatDate(value)} className={className}>
      {label}
    </time>
  )
}

function SummaryItem({
  label,
  children,
}: {
  label: string
  children: ReactNode
}) {
  return (
    <div className='flex min-w-0 flex-col gap-0.5'>
      <dt className='text-xs text-muted'>{label}</dt>
      <dd className='m-0 flex min-w-0'>{children}</dd>
    </div>
  )
}

function SummaryTime({ value }: { value: string }) {
  if (!value) return <span className={cn(summaryValue, 'text-muted')}>—</span>
  return (
    <time dateTime={value} className={cn(summaryValue, 'truncate')}>
      {formatDate(value)}
    </time>
  )
}

function AttemptList({
  operationId,
  history,
  icon,
}: {
  operationId: string
  history: SubtaskHistory
  icon: ReactNode
}) {
  if (!history.list.length) return <EmptyState size='sm' title='暂无执行记录' />
  const groups = groupAttempts(history.list)
  return (
    <ol
      aria-label='执行记录'
      className='m-0 flex list-none flex-col divide-y divide-border p-0'
    >
      {groups.map((group) =>
        group.attempts.length > 1 ? (
          <AttemptRun
            key={group.key}
            operationId={operationId}
            attempts={group.attempts}
            icon={icon}
          />
        ) : (
          <li key={group.key} className={attemptRow}>
            <AttemptBody
              operationId={operationId}
              attempt={group.attempts[0]}
              icon={icon}
            />
          </li>
        )
      )}
    </ol>
  )
}

function AttemptHeader({
  title,
  outcome,
  aside,
}: {
  title: string
  outcome: TaskAttempt['outcome']
  aside: ReactNode
}) {
  // Outcomes from a newer backend show their raw value in a neutral chip.
  const shown = known(outcomes, outcome) ?? {
    label: outcome || '未知结果',
    tone: 'default' as const,
  }
  return (
    <div className='flex min-w-0 flex-wrap items-center justify-between gap-x-3 gap-y-1'>
      <div className='flex shrink-0 items-center gap-2'>
        <h3 className='subsection-title whitespace-nowrap'>{title}</h3>
        <StatusChip
          label={shown.label}
          tone={shown.tone}
          reserve={outcomeLabels}
        />
      </div>
      <span className='text-sm whitespace-nowrap text-muted tabular-nums'>
        {aside}
      </span>
    </div>
  )
}

/** Start time (the record time for records without one) and duration. */
function AttemptMeta({
  attempt,
  extra,
}: {
  attempt: TaskAttempt
  extra?: string
}) {
  const value = attempt.started_at || attempt.emitted_at
  return (
    <>
      <time dateTime={value}>{formatDate(value)}</time>
      {attempt.started_at && ` · ${formatAttemptDuration(attempt)}`}
      {extra && ` · ${extra}`}
    </>
  )
}

function AttemptBody({
  operationId,
  attempt,
  icon,
}: {
  operationId: string
  attempt: TaskAttempt
  icon: ReactNode
}) {
  return (
    <>
      <AttemptHeader
        title={`第 ${attempt.attempt} 次`}
        outcome={attempt.outcome}
        aside={<AttemptMeta attempt={attempt} />}
      />
      {attempt.error && <AttemptError error={attempt.error} />}
      <TaskAttemptDetail detail={attempt.detail} outcome={attempt.outcome} />
      <AttemptTimeline
        operationId={operationId}
        trace={attempt.trace}
        icon={icon}
      />
    </>
  )
}

/** Full error text (long text scrolls inside), copyable. */
function AttemptError({ error }: { error: string }) {
  return (
    <div className='flex min-w-0 items-start gap-1 rounded-lg bg-danger-soft px-2.5 py-1.5'>
      <p className='m-0 max-h-32 min-w-0 flex-1 overflow-auto text-xs leading-5 break-words whitespace-pre-wrap text-danger'>
        <RedactedText value={error} />
      </p>
      <CopyButton value={error} label='复制错误信息' />
    </div>
  )
}

/** Consecutive failures with the same error, folded into one entry. The
 * newest attempt's detail and timeline represent the run; opening the run
 * lists every attempt, older ones with their own timeline. */
function AttemptRun({
  operationId,
  attempts,
  icon,
}: {
  operationId: string
  attempts: TaskAttempt[]
  icon: ReactNode
}) {
  const [expanded, setExpanded] = useState(false)
  const newest = attempts[0]
  const oldest = attempts[attempts.length - 1]
  return (
    <li className={attemptRow}>
      <AttemptHeader
        title={`第 ${oldest.attempt}–${newest.attempt} 次`}
        outcome='failed'
        aside={
          <AttemptMeta attempt={newest} extra={`共 ${attempts.length} 次`} />
        }
      />
      <AttemptError error={newest.error} />
      <TaskAttemptDetail detail={newest.detail} outcome={newest.outcome} />
      <AttemptTimeline
        operationId={operationId}
        trace={newest.trace}
        icon={icon}
      />
      <Disclosure isExpanded={expanded} onExpandedChange={setExpanded}>
        <Disclosure.Heading>
          <Button
            slot='trigger'
            size='sm'
            variant='tertiary'
            className='h-7 px-1.5 text-xs'
          >
            {expanded ? '收起' : `展开 ${attempts.length} 次尝试`}
            <Disclosure.Indicator />
          </Button>
        </Disclosure.Heading>
        <Disclosure.Content>
          <Disclosure.Body>
            {/* Older timelines mount only while open, so their traces are
                read only once they can be seen. */}
            {expanded && (
              <ol
                aria-label={`第 ${oldest.attempt}–${newest.attempt} 次尝试`}
                className='m-0 mt-1 flex list-none flex-col gap-3 p-0 pl-3'
              >
                {attempts.map((attempt, index) => (
                  <li
                    key={`${attempt.attempt}:${attempt.emitted_at}`}
                    className='flex min-w-0 flex-col gap-1.5'
                  >
                    <div className='flex min-w-0 items-center justify-between gap-3 text-xs'>
                      <span className='font-medium'>
                        第 {attempt.attempt} 次
                      </span>
                      <span className='text-muted tabular-nums'>
                        <AttemptMeta attempt={attempt} />
                      </span>
                    </div>
                    {index > 0 && (
                      <AttemptTimeline
                        operationId={operationId}
                        trace={attempt.trace}
                        icon={icon}
                      />
                    )}
                  </li>
                ))}
              </ol>
            )}
          </Disclosure.Body>
        </Disclosure.Content>
      </Disclosure>
    </li>
  )
}
