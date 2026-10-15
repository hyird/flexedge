import { useEffect, useState, type ReactNode } from 'react'
import { useQuery } from '@tanstack/react-query'
import { ArrowRotateLeft } from '@gravity-ui/icons'
import { Button, Disclosure, Skeleton } from '@heroui/react'
import { formatDate, parseDate } from '@/lib/format'
import { cn } from '@/lib/utils'
import { CopyButton } from '@/components/copy-button'
import { EmptyState, QueryNotice } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { taskHistoryQuery } from './data'
import { TaskAttemptDetail } from './task-attempt-detail'
import { AttemptTimeline, RedactedText } from './task-attempt-trace'
import {
  formatAttemptDuration,
  formatRetryCountdown,
  groupAttempts,
} from './task-history'
import { known, type Task, type TaskAttempt, type TaskHistory } from './types'

const outcomes = {
  completed: { label: '成功', tone: 'success' },
  failed: { label: '失败', tone: 'danger' },
} as const
const outcomeLabels = [outcomes.completed.label, outcomes.failed.label]
// One attempt (or folded run) per row, separated by a hairline.
const attemptRow = 'flex min-w-0 flex-col gap-2 py-3 first:pt-0 last:pb-0'

/** "执行记录" tab: a one-shot snapshot of the task's attempts, read when the
 * tab opens and again only on the manual refresh button. Each attempt shows
 * its execution timeline; full traces are read per attempt once on screen. */
export function TaskHistoryPanel({ task }: { task: Task }) {
  const history = useQuery({
    ...taskHistoryQuery(task),
    meta: { inlineError: true },
  })
  const data = history.data
  return (
    <>
      <QueryNotice
        query={history}
        title='执行记录加载失败'
        onRetry={() => history.refetch()}
      />
      {data ? (
        <HistorySummary summary={data.summary} />
      ) : (
        history.isPending && (
          <Skeleton className='h-[10.125rem] w-full rounded-xl sm:h-28' />
        )
      )}
      {(data || history.isPending) && (
        <div className='flex justify-end'>
          <Button
            size='sm'
            variant='tertiary'
            isPending={history.isFetching}
            onPress={() => void history.refetch()}
          >
            <ArrowRotateLeft className='size-4' />
            刷新记录
          </Button>
        </div>
      )}
      {data ? (
        <AttemptList taskId={task.id} history={data} />
      ) : (
        history.isPending && (
          <div
            role='status'
            aria-label='正在加载执行记录'
            className='flex flex-col gap-3'
          >
            {Array.from({ length: 3 }, (_, index) => (
              <Skeleton key={index} className='h-16 w-full rounded-lg' />
            ))}
          </div>
        )
      )}
    </>
  )
}

const summaryValue = 'text-[13px] leading-5 font-medium tabular-nums'

function HistorySummary({ summary }: { summary: TaskHistory['summary'] }) {
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

/** Countdown to the scheduled retry on a local timer (1 s under a minute,
 * else 30 s). It only re-renders this label and never reads data; it stops
 * once the retry is due. */
function RetryCountdown({
  value,
  className,
}: {
  value: string
  className?: string
}) {
  const at = parseDate(value)?.getTime()
  const [now, setNow] = useState(Date.now)
  const remaining = at === undefined ? 0 : at - now
  useEffect(() => {
    if (remaining <= 0) return
    const timer = window.setTimeout(
      () => setNow(Date.now()),
      remaining <= 60_000 ? 1000 : 30_000
    )
    return () => window.clearTimeout(timer)
  }, [remaining])
  return (
    <time dateTime={value} title={formatDate(value)} className={className}>
      {at === undefined ? '—' : formatRetryCountdown(at, now)}
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
  taskId,
  history,
}: {
  taskId: string
  history: TaskHistory
}) {
  if (!history.list.length) return <EmptyState size='sm' title='暂无执行记录' />
  const groups = groupAttempts(history.list)
  return (
    <>
      {history.truncated && (
        <p className='m-0 text-xs text-muted'>
          仅列出最近 {history.list.length} 次
        </p>
      )}
      <ol
        aria-label='执行记录'
        className='m-0 flex list-none flex-col divide-y divide-border p-0'
      >
        {groups.map((group) =>
          group.attempts.length > 1 ? (
            <AttemptRun
              key={group.key}
              taskId={taskId}
              attempts={group.attempts}
            />
          ) : (
            <li key={group.key} className={attemptRow}>
              <AttemptBody taskId={taskId} attempt={group.attempts[0]} />
            </li>
          )
        )}
      </ol>
    </>
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
        <h3 className='m-0 text-sm font-medium whitespace-nowrap text-foreground'>
          {title}
        </h3>
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
  taskId,
  attempt,
}: {
  taskId: string
  attempt: TaskAttempt
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
      <AttemptTimeline taskId={taskId} trace={attempt.trace} />
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
  taskId,
  attempts,
}: {
  taskId: string
  attempts: TaskAttempt[]
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
      <AttemptTimeline taskId={taskId} trace={newest.trace} />
      <Disclosure isExpanded={expanded} onExpandedChange={setExpanded}>
        <Disclosure.Heading>
          <Button
            slot='trigger'
            size='sm'
            variant='ghost'
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
                      <AttemptTimeline taskId={taskId} trace={attempt.trace} />
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
