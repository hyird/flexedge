import { useEffect, useEffectEvent, useState, type ReactNode } from 'react'
import { useQuery } from '@tanstack/react-query'
import { Link as RouterLink } from '@tanstack/react-router'
import { Accordion, Link, Skeleton } from '@heroui/react'
import { ApiBusinessError } from '@/lib/api-response'
import { useDisplaySeconds } from '@/lib/display-clock'
import { formatDate, parseDate } from '@/lib/format'
import { useArrivalState } from '@/lib/history-state'
import { cn } from '@/lib/utils'
import { FormSheet } from '@/components/form-sheet'
import { EmptyState, QueryNotice } from '@/components/page'
import { StatusChip } from '@/components/status-chip'
import { operationDetailQuery } from './data'
import { SubtaskLinkContext } from './subtask-link-context'
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
import { RetryCountdown, SubtaskHistoryPanel } from './task-history-panel'
import type { OperationDetail, Subtask } from './types'

/** Contract §7: the task does not exist (or expired). */
export const operationNotFoundCode = 16701
/** Contract §7: the expanded subtask does not belong to the task (cleaned up). */
export const subtaskNotFoundCode = 16705

/** Task detail opened from `/tasks?task=<operation id>`: a header, the
 * subtasks in creation order (children one level in) and the expanded
 * subtask's live 执行记录. One live stream serves all of it; expanding
 * another subtask or collapsing switches its URL, keeping the task on screen.
 * The expanded subtask is component state (seeded by a link's history state). */
export function TaskDetailSheet({
  operationId,
  onClose,
}: {
  operationId: string
  onClose: () => void
}) {
  const [expanded, setExpanded] = useArrivalState<string | null>(
    (state) => state.tasks?.subtask ?? null
  )
  // Errors render inside the sheet (not-found state or QueryNotice).
  const detail = useQuery({
    ...operationDetailQuery(operationId, expanded),
    meta: { inlineError: true },
  })
  // The records panel mounts one commit after the detail query moved to the
  // new URL: this effect runs after the detail query's own (declaration
  // order), so the old stream is released before any binding opens the new
  // one, even though child effects run before parent effects.
  const [recordsFor, setRecordsFor] = useState(expanded)
  useEffect(() => {
    queueMicrotask(() => setRecordsFor(expanded))
  }, [expanded])
  const operation = detail.data
  const code =
    detail.error instanceof ApiBusinessError ? detail.error.code : undefined
  // The expanded subtask was cleaned up: collapse and reconnect without it.
  const collapseMissing = useEffectEvent(() => setExpanded(null))
  useEffect(() => {
    if (code === subtaskNotFoundCode) collapseMissing()
  }, [code])
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
          {code !== subtaskNotFoundCode && (
            <QueryNotice query={detail} onRetry={() => detail.refetch()} />
          )}
          {operation ? (
            <SubtaskLinkContext
              value={{
                has: (id) => operation.subtasks.some((item) => item.id === id),
                open: setExpanded,
              }}
            >
              <OperationHeader operation={operation} />
              <SubtaskList
                operation={operation}
                expanded={expanded}
                onExpandedChange={setExpanded}
                recordsFor={recordsFor}
                streamFailed={detail.isError}
              />
            </SubtaskLinkContext>
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

/** Subtasks in creation order, children one level in; one expanded at a
 * time, and only the expanded one reads its execution records. */
export function SubtaskList({
  operation,
  expanded,
  recordsFor = expanded,
  onExpandedChange,
  streamFailed,
}: {
  operation: OperationDetail
  expanded: string | null
  /** The subtask whose records may be read (see `TaskDetailSheet`). */
  recordsFor?: string | null
  onExpandedChange: (id: string | null) => void
  streamFailed: boolean
}) {
  const rows = subtaskRows(operation.subtasks)
  if (!rows.length) return <EmptyState size='sm' title='暂无子任务' />
  return (
    <section aria-label='子任务' className='flex min-w-0 flex-col'>
      <Accordion
        className='rounded-xl border border-separator'
        expandedKeys={expanded ? [expanded] : []}
        // One item at a time: the set holds the new item or nothing.
        onExpandedChange={(keys) =>
          onExpandedChange((keys.values().next().value as string) ?? null)
        }
      >
        {rows.map(({ subtask, depth }) => (
          <Accordion.Item key={subtask.id} id={subtask.id}>
            <Accordion.Heading
              className={cn(
                'min-w-0 items-center gap-2 pe-3',
                depth > 0 && 'ps-6'
              )}
            >
              <Accordion.Trigger className='min-w-0 justify-start gap-2 px-3 py-2.5 font-normal'>
                <Accordion.Indicator className='order-first ms-0 -rotate-90 data-[expanded=true]:rotate-0' />
                <span className='min-w-0 flex-1 truncate text-start'>
                  {subtaskTitle(subtask)}
                </span>
                <SubtaskStatus subtask={subtask} />
              </Accordion.Trigger>
              <SubtaskNoteSlot
                subtask={subtask}
                operationId={operation.id}
                onExpand={onExpandedChange}
              />
              <SubtaskDuration subtask={subtask} />
            </Accordion.Heading>
            <Accordion.Panel>
              <Accordion.Body className='px-3 pb-3'>
                {/* Only the expanded subtask reads its records. */}
                {expanded === subtask.id && recordsFor === subtask.id && (
                  <SubtaskHistoryPanel
                    operationId={operation.id}
                    subtaskId={subtask.id}
                    resourceType={subtask.resource_type}
                    showErrors={!streamFailed}
                  />
                )}
              </Accordion.Body>
            </Accordion.Panel>
          </Accordion.Item>
        ))}
      </Accordion>
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

const noteSlot = 'w-24 shrink-0 truncate text-xs leading-5 sm:w-44'

/** The row's fixed extra-info slot (contract §5.2); empty but reserved when
 * nothing applies, so status changes never move the duration. */
function SubtaskNoteSlot({
  subtask,
  operationId,
  onExpand,
}: {
  subtask: Subtask
  operationId: string
  onExpand: (id: string) => void
}) {
  const note = subtaskNote(subtask)
  if (!note) return <span aria-hidden className={noteSlot} />
  switch (note.kind) {
    case 'merged': {
      const text = `由 ${note.label} 执行`
      return note.operationId === operationId ? (
        <Link
          className={cn(noteSlot, 'block')}
          onPress={() => onExpand(note.subtaskId)}
        >
          {text}
        </Link>
      ) : (
        <RouterLink
          from='/tasks/'
          to='.'
          search={(previous) => ({ ...previous, task: note.operationId })}
          state={{ tasks: { subtask: note.subtaskId } }}
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
  'w-20 shrink-0 text-end text-xs leading-5 whitespace-nowrap text-muted tabular-nums'

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
