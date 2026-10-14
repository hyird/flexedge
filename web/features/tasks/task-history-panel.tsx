import { useEffect, useState, type ReactNode } from 'react'
import { useQuery } from '@tanstack/react-query'
import { ArrowRotateLeft, CircleCheck } from '@gravity-ui/icons'
import { Segment } from '@heroui-pro/react'
import { Alert, Button, Disclosure, Skeleton } from '@heroui/react'
import { formatDate, formatPercent, parseDate } from '@/lib/format'
import { cn } from '@/lib/utils'
import { CopyButton } from '@/components/copy-button'
import { EmptyState, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { taskHistoryQuery } from './data'
import { TaskAttemptDetail } from './task-attempt-detail'
import { AttemptTrace, RedactedText } from './task-attempt-trace'
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

type HistoryFilter = 'all' | 'failed'

/** "执行记录" tab: a one-shot snapshot of the task's attempts, read when the
 * tab opens and again only on the manual refresh button. Relative times tick
 * on the shared local display clock and never read data. */
export function TaskHistoryPanel({ task }: { task: Task }) {
  const history = useQuery({
    ...taskHistoryQuery(task),
    meta: { inlineError: true },
  })
  const [filter, setFilter] = useState<HistoryFilter>('all')
  const data = history.data
  return (
    <>
      <QueryNotice
        query={history}
        title='执行记录加载失败'
        onRetry={() => history.refetch()}
      />
      {data ? (
        <HistorySummary
          summary={data.summary}
          node={task.resource_type === 'node'}
        />
      ) : (
        history.isPending && (
          <Skeleton className='h-[7.5rem] w-full rounded-xl sm:h-[8.25rem]' />
        )
      )}
      {(data || history.isPending) && (
        <div className='flex flex-wrap items-center justify-between gap-2'>
          <Segment
            size='sm'
            aria-label='筛选执行记录'
            selectedKey={filter}
            onSelectionChange={(key) => setFilter(key as HistoryFilter)}
            isDisabled={!data?.list.length}
          >
            <Segment.Item id='all'>全部</Segment.Item>
            <Segment.Item id='failed'>只看失败</Segment.Item>
          </Segment>
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
        <AttemptList
          taskId={task.id}
          history={data}
          filter={filter}
          onShowAll={() => setFilter('all')}
        />
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

function HistorySummary({
  summary,
  node,
}: {
  summary: TaskHistory['summary']
  node: boolean
}) {
  return (
    <dl className='grid grid-cols-2 gap-x-4 gap-y-3 rounded-xl bg-surface-secondary p-3 sm:grid-cols-3'>
      <SummaryItem label='累计尝试'>
        <span className='text-base font-semibold tabular-nums'>
          {summary.attempt_count}
        </span>
        <span className='text-xs text-muted'>保留 7 天</span>
      </SummaryItem>
      <SummaryItem label='失败次数'>
        <span
          className={cn(
            'text-base font-semibold tabular-nums',
            summary.failure_count > 0 && 'text-danger'
          )}
        >
          {summary.failure_count}
        </span>
        <span className='text-xs text-muted'>
          {summary.attempt_count
            ? `占 ${formatPercent(summary.failure_count / summary.attempt_count, 0)}`
            : '—'}
        </span>
      </SummaryItem>
      <SummaryItem label='下次重试'>
        {summary.next_attempt_at ? (
          <>
            <RetryCountdown
              value={summary.next_attempt_at}
              className='text-base font-semibold text-warning'
            />
            <span className='text-xs text-muted'>
              {formatDate(summary.next_attempt_at)}
            </span>
          </>
        ) : (
          <>
            <span className='text-base font-semibold text-muted'>—</span>
            <span className='text-xs text-muted'>
              {node ? '由节点自行退避重试' : '没有待执行的重试'}
            </span>
          </>
        )}
      </SummaryItem>
      <SummaryItem label='首次失败'>
        <TimePair value={summary.first_failure_at} empty='没有失败' />
      </SummaryItem>
      <SummaryItem label='最近成功'>
        <TimePair value={summary.last_success_at} empty='尚未成功' />
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
    <time dateTime={value} className={className}>
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
      <dd className='m-0 flex min-w-0 flex-col'>{children}</dd>
    </div>
  )
}

/** Absolute time on the first line and relative time on the second; both
 * lines are always present so the summary never changes height. */
function TimePair({ value, empty }: { value: string; empty: string }) {
  if (!value)
    return (
      <>
        <span className='text-sm text-muted'>—</span>
        <span className='text-xs text-muted'>{empty}</span>
      </>
    )
  return (
    <>
      <span className='text-sm tabular-nums'>{formatDate(value)}</span>
      <RelativeTime value={value} className='text-xs text-muted' />
    </>
  )
}

function AttemptList({
  taskId,
  history,
  filter,
  onShowAll,
}: {
  taskId: string
  history: TaskHistory
  filter: HistoryFilter
  onShowAll: () => void
}) {
  if (!history.list.length)
    return (
      <EmptyState
        size='sm'
        title='暂无执行记录'
        description='任务开始执行后，每次尝试的结果会记录在这里，保留 7 天。'
      />
    )
  const groups = groupAttempts(history.list, {
    failuresOnly: filter === 'failed',
  })
  const omitted = history.summary.attempt_count - history.list.length
  return (
    <>
      {history.truncated && (
        <Alert status='accent'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>只列出最近 {history.list.length} 次尝试</Alert.Title>
            <Alert.Description>
              {omitted > 0
                ? `更早的 ${omitted} 次尝试未列出；上方统计包含全部保留记录。`
                : '更早的尝试未列出；上方统计包含全部保留记录。'}
            </Alert.Description>
          </Alert.Content>
        </Alert>
      )}
      {groups.length ? (
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
      ) : (
        <EmptyState
          size='sm'
          icon={<CircleCheck />}
          title='没有失败的尝试'
          description='列出的执行记录中没有失败的尝试。'
          action={
            <Button size='sm' variant='tertiary' onPress={onShowAll}>
              查看全部记录
            </Button>
          }
        />
      )}
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
    <div className='flex min-w-0 items-center justify-between gap-3'>
      <div className='flex min-w-0 items-center gap-2'>
        <h3 className='m-0 truncate text-sm font-medium text-foreground'>
          {title}
        </h3>
        <StatusChip
          label={shown.label}
          tone={shown.tone}
          reserve={outcomeLabels}
        />
      </div>
      <span className='shrink-0 text-xs text-muted tabular-nums'>{aside}</span>
    </div>
  )
}

/** Start time (absolute + relative). Without a start time (records written
 * before the upgrade, node heartbeat confirmations) the record time is shown;
 * only pre-upgrade records also lack any detail. */
function AttemptTime({ attempt }: { attempt: TaskAttempt }) {
  const value = attempt.started_at || attempt.emitted_at
  const legacy =
    !attempt.started_at &&
    attempt.detail.kind === 'none' &&
    !attempt.detail.truncated
  return (
    <div className='flex flex-wrap items-center gap-x-2 text-xs text-muted'>
      <span>
        {attempt.started_at ? '开始于' : '记录于'}{' '}
        <time dateTime={value} className='text-foreground tabular-nums'>
          {formatDate(value)}
        </time>
      </span>
      <RelativeTime value={value} />
      {legacy && <span>· 升级前的记录，没有开始时间与执行明细</span>}
    </div>
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
        aside={`耗时 ${formatAttemptDuration(attempt)}`}
      />
      <AttemptTime attempt={attempt} />
      {attempt.error && <AttemptError error={attempt.error} />}
      {attempt.next_attempt_at && (
        <p className='m-0 text-xs text-muted'>
          计划于{' '}
          <span className='text-foreground tabular-nums'>
            {formatDate(attempt.next_attempt_at)}
          </span>{' '}
          重试
        </p>
      )}
      <TaskAttemptDetail detail={attempt.detail} outcome={attempt.outcome} />
      <AttemptTrace taskId={taskId} trace={attempt.trace} />
    </>
  )
}

/** Error text clamped to two lines, expandable and copyable. */
function AttemptError({ error, prefix }: { error: string; prefix?: string }) {
  const [expanded, setExpanded] = useState(false)
  const long = error.length > 120 || error.includes('\n')
  return (
    <div className='flex min-w-0 items-start gap-1 rounded-lg bg-danger-soft px-2.5 py-1.5'>
      <div className='flex min-w-0 flex-1 flex-col items-start gap-0.5'>
        <p
          className={cn(
            'm-0 text-xs leading-5 break-words whitespace-pre-wrap text-danger',
            !expanded && 'line-clamp-2'
          )}
        >
          {prefix && <span className='font-medium'>{prefix}</span>}
          <RedactedText value={error} />
        </p>
        {long && (
          <Button
            size='sm'
            variant='ghost'
            className='h-6 min-w-0 px-1 text-xs'
            aria-expanded={expanded}
            onPress={() => setExpanded(!expanded)}
          >
            {expanded ? '收起错误' : '展开全文'}
          </Button>
        )}
      </div>
      <CopyButton value={error} label='复制错误信息' />
    </div>
  )
}

/** Consecutive failures with the same error, collapsed into one entry that
 * expands to the individual attempts (newest first). The newest attempt's
 * detail and trace represent the run; older ones keep their own trace. */
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
        aside={`共 ${attempts.length} 次`}
      />
      <div className='flex flex-wrap items-center gap-x-2 text-xs text-muted'>
        <span>
          最近一次{' '}
          <span className='text-foreground tabular-nums'>
            {formatDate(newest.started_at || newest.emitted_at)}
          </span>
        </span>
        <RelativeTime value={newest.started_at || newest.emitted_at} />
      </div>
      <AttemptError
        error={newest.error}
        prefix={`连续 ${attempts.length} 次：`}
      />
      {newest.next_attempt_at && (
        <p className='m-0 text-xs text-muted'>
          计划于{' '}
          <span className='text-foreground tabular-nums'>
            {formatDate(newest.next_attempt_at)}
          </span>{' '}
          重试
        </p>
      )}
      <TaskAttemptDetail detail={newest.detail} outcome={newest.outcome} />
      <AttemptTrace taskId={taskId} trace={newest.trace} />
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
            <ol
              aria-label={`第 ${oldest.attempt}–${newest.attempt} 次尝试`}
              className='m-0 mt-1 flex list-none flex-col gap-3 border-l border-border p-0 pl-3'
            >
              {attempts.map((attempt, index) => (
                <li
                  key={`${attempt.attempt}:${attempt.emitted_at}`}
                  className='flex min-w-0 flex-col gap-1.5'
                >
                  <div className='flex min-w-0 items-center justify-between gap-3'>
                    <span className='text-xs font-medium'>
                      第 {attempt.attempt} 次
                    </span>
                    <span className='text-xs text-muted tabular-nums'>
                      耗时 {formatAttemptDuration(attempt)}
                    </span>
                  </div>
                  <AttemptTime attempt={attempt} />
                  {index > 0 && (
                    <AttemptTrace taskId={taskId} trace={attempt.trace} />
                  )}
                </li>
              ))}
            </ol>
          </Disclosure.Body>
        </Disclosure.Content>
      </Disclosure>
    </li>
  )
}
