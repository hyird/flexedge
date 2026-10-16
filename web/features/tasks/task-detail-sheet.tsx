import type { ReactNode } from 'react'
import { useQuery } from '@tanstack/react-query'
import { Link as RouterLink } from '@tanstack/react-router'
import { ListCheck } from '@gravity-ui/icons'
import { Timeline } from '@heroui-pro/react'
import { Link, Skeleton } from '@heroui/react'
import { ApiBusinessError } from '@/lib/api-response'
import { useDisplaySeconds } from '@/lib/display-clock'
import { formatDate, parseDate } from '@/lib/format'
import { cn } from '@/lib/utils'
import { navigationItemFor } from '@/components/layout/navigation'
import { FormSheet } from '@/components/form-sheet'
import { EmptyState, QueryNotice } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { operationDetailQuery } from './data'
import {
  actorLabel,
  describeOperationStatus,
  formatOperationSummary,
  operationStatusLabels,
  operationTitle,
  operationTarget,
  subtaskNote,
  subtaskRows,
  subtaskTitle,
} from './task-display'
import { formatDuration, formatElapsed } from './task-history'
import { RetryCountdown } from './task-history-panel'
import type { OperationDetail, Subtask } from './types'

/** Contract §7: the task does not exist (or expired). */
export const operationNotFoundCode = 16701

/** Task detail opened from `/tasks?task=<operation id>`: one live stream
 * supplies the header and the subtask timeline, including failure reasons. */
export function TaskDetailSheet({
  operationId,
  onClose,
}: {
  operationId: string
  onClose: () => void
}) {
  // Errors render inside the sheet (not-found state or QueryNotice).
  const detail = useQuery({
    ...operationDetailQuery(operationId, null),
    meta: { inlineError: true },
  })
  const operation = detail.data
  const code =
    detail.error instanceof ApiBusinessError ? detail.error.code : undefined
  const status = operation && describeOperationStatus(operation.status)
  return (
    <FormSheet
      isOpen
      onClose={onClose}
      size='lg'
      title={operation ? operationTitle(operation) : '任务详情'}
      description={
        status && (
          <StatusChip
            label={status.label}
            tone={status.tone}
            reserve={operationStatusLabels}
          />
        )
      }
    >
      {code === operationNotFoundCode ? (
        <EmptyState title='任务不存在或已过期' />
      ) : (
        <>
          <QueryNotice query={detail} onRetry={() => detail.refetch()} />
          {operation ? (
            <>
              <OperationHeader operation={operation} />
              <SubtaskList operation={operation} />
            </>
          ) : (
            detail.isPending && (
              <div
                role='status'
                aria-label='正在加载任务详情'
                className='flex flex-col gap-3'
              >
                <Skeleton className='h-5 w-64 rounded-md' />
                {Array.from({ length: 4 }, (_, index) => (
                  <Skeleton key={index} className='h-10 w-full rounded-lg' />
                ))}
              </div>
            )
          )}
        </>
      )}
    </FormSheet>
  )
}

/** 发起人 · 时间 · target link, then the one-line change summary. */
function OperationHeader({ operation }: { operation: OperationDetail }) {
  const target = operationTarget(operation)
  const summary = formatOperationSummary(operation)
  return (
    <div className='flex min-w-0 flex-col gap-1 text-sm'>
      <p className='m-0 flex min-w-0 items-center gap-1.5 overflow-x-auto whitespace-nowrap text-muted'>
        <span>{actorLabel(operation)}</span>
        <span aria-hidden>·</span>
        <time dateTime={operation.created_at} className='tabular-nums'>
          {formatDate(operation.created_at)}
        </time>
        {target && (
          <>
            <span aria-hidden>·</span>
            <Link
              href={target.href}
              routerOptions={target.state && { state: target.state }}
              className='text-sm'
            >
              {operation.target_name}
            </Link>
          </>
        )}
      </p>
      {summary && <p className='m-0 truncate text-foreground'>{summary}</p>}
    </div>
  )
}

const resourceMenuPaths: Record<string, string> = {
  website: '/websites',
  certificate: '/certificates',
  dns_zone: '/dns-zones',
  provider: '/providers',
  node: '/nodes',
}

/** Each subtask is one timeline node with its resource's menu icon. */
export function SubtaskList({ operation }: { operation: OperationDetail }) {
  const rows = subtaskRows(operation.subtasks)
  if (!rows.length) return <EmptyState size='sm' title='暂无子任务' />
  return (
    <section aria-label='子任务' className='flex min-w-0 flex-col'>
      <Timeline aria-label='执行步骤' size='sm' density='comfortable'>
        {rows.map(({ subtask, depth }) => {
          const status = describeOperationStatus(subtask.status)
          const failed = status.tone === 'danger' || status.tone === 'warning'
          const Icon =
            navigationItemFor(
              resourceMenuPaths[subtask.resource_type] ?? '/tasks'
            )?.icon ?? ListCheck
          return (
            <Timeline.Item
              key={subtask.id}
              status={
                status.tone === 'accent'
                  ? 'current'
                  : status.tone === 'default'
                    ? 'muted'
                    : status.tone
              }
              className={cn('min-w-0', depth > 0 && 'ms-6')}
            >
              <Timeline.Marker>
                <Icon aria-hidden />
              </Timeline.Marker>
              <Timeline.Content className='min-w-0 gap-1.5'>
                <div className='flex min-h-10 min-w-0 items-center gap-2 sm:min-h-0'>
                  <span
                    title={subtaskTitle(subtask)}
                    className='line-clamp-2 min-w-0 flex-1 text-start text-sm break-words sm:block sm:truncate'
                  >
                    {subtaskTitle(subtask)}
                  </span>
                  <SubtaskStatus subtask={subtask} />
                  <SubtaskNoteSlot
                    subtask={subtask}
                    operationId={operation.id}
                  />
                  <SubtaskDuration subtask={subtask} />
                </div>
                {failed && (subtask.error || status.tone === 'danger') && (
                  <p className='m-0 text-xs break-words whitespace-pre-wrap text-danger'>
                    失败原因：{subtask.error || '未提供失败原因'}
                  </p>
                )}
              </Timeline.Content>
            </Timeline.Item>
          )
        })}
      </Timeline>
    </section>
  )
}

function SubtaskStatus({ subtask }: { subtask: Subtask }) {
  const status = describeOperationStatus(subtask.status)
  return (
    <StatusChip
      label={status.label}
      tone={status.tone}
      reserve={operationStatusLabels}
    />
  )
}

const noteSlot = 'w-6 shrink-0 truncate text-xs leading-5 sm:w-32'

/** The row's fixed extra-info slot (contract §5.2); empty but reserved when
 * nothing applies, so status changes never move the duration. */
function SubtaskNoteSlot({
  subtask,
  operationId,
}: {
  subtask: Subtask
  operationId: string
}) {
  const note = subtaskNote(subtask)
  if (!note) return <span aria-hidden className={noteSlot} />
  switch (note.kind) {
    case 'merged': {
      const text = `由 ${note.label} 执行`
      return note.operationId === operationId ? (
        <span className={noteSlot} title={text}>
          {text}
        </span>
      ) : (
        <RouterLink
          from='/tasks/'
          to='.'
          search={(previous) => ({ ...previous, task: note.operationId })}
          title={text}
          className={cn(noteSlot, 'link block')}
        >
          {text}
        </RouterLink>
      )
    }
    case 'closed': {
      const text = `因 ${note.label} ${note.verb}`
      return (
        <span className={cn(noteSlot, 'text-muted')} title={text}>
          {text}
        </span>
      )
    }
    case 'offline':
      return <span className={cn(noteSlot, 'text-danger')}>节点离线</span>
    case 'retry':
      return (
        <RetryCountdown
          value={note.at}
          note
          className={cn(noteSlot, 'text-warning tabular-nums')}
        />
      )
    case 'warning':
      return <span className={cn(noteSlot, 'text-warning')}>{note.label}</span>
    case 'recovered':
      return (
        <span className={cn(noteSlot, 'text-muted')}>
          失败 {note.failures} 次后成功
        </span>
      )
  }
}

const durationSlot =
  'w-16 shrink-0 text-end text-xs leading-5 whitespace-nowrap text-muted tabular-nums sm:w-20'

/** Settled: the recorded duration. Unsettled: live elapsed time on the
 * shared 1 s clock, since the current claim (`running_since`, renewed on
 * every claim) or else since the subtask was created. */
function SubtaskDuration({ subtask }: { subtask: Subtask }) {
  if (subtask.finished_at)
    return (
      <span className={durationSlot}>
        {formatDuration(subtask.duration_ms)}
      </span>
    )
  return (
    <ElapsedTime
      since={subtask.running_since || subtask.created_at}
      className={durationSlot}
    />
  )
}

function ElapsedTime({
  since,
  className,
}: {
  since: string
  className: string
}): ReactNode {
  const now = useDisplaySeconds()
  const start = parseDate(since)?.getTime()
  return (
    <span className={className}>
      {start === undefined ? '—' : formatElapsed(now - start)}
    </span>
  )
}
