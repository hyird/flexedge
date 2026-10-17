import { useQuery } from '@tanstack/react-query'
import { Link, Skeleton } from '@heroui/react'
import { apiErrorCode } from '@/lib/api'
import { describeStatus } from '@/lib/status'
import { FormSheet } from '@/components/form-sheet'
import { EmptyState, QueryNotice } from '@/components/page'
import { RelativeTime } from '@/components/relative-time'
import { StatusChip } from '@/components/status-chip'
import { taskDetailQuery } from './data'
import {
  operationLabel,
  taskStatuses,
  taskStatusReserve,
  taskTarget,
  taskTitle,
} from './task-display'
import { TaskHistoryPanel } from './task-history-panel'
import { TaskNoteSlot } from './task-note'
import type { Task, TaskRef } from './types'

/** The task does not exist (its resource was deleted). */
export const taskNotFoundCode = 16701

/** Task detail opened from `/tasks?task=<resource_type>:<resource_id>`: one
 * live stream supplies the header (`snapshot`) and the execution records
 * (`history` + `attempt`). */
export function TaskDetailSheet({
  task: ref,
  onClose,
}: {
  task: TaskRef
  onClose: () => void
}) {
  // Errors render inside the sheet (not-found state or QueryNotice).
  const detail = useQuery({
    ...taskDetailQuery(ref),
    meta: { inlineError: true },
  })
  const task = detail.data
  const code = apiErrorCode(detail.error)
  const status = task && describeStatus(taskStatuses, task.status)
  return (
    <FormSheet
      isOpen
      onClose={onClose}
      size='lg'
      title={task ? taskTitle(task) : '任务详情'}
      description={
        status && <StatusChip {...status} reserve={taskStatusReserve} />
      }
    >
      {code === taskNotFoundCode ? (
        <EmptyState title='任务不存在' />
      ) : (
        <>
          <QueryNotice query={detail} onRetry={() => detail.refetch()} />
          {task ? (
            <TaskHeader task={task} />
          ) : (
            detail.isPending && (
              <div
                role='status'
                aria-label='正在加载任务详情'
                className='flex flex-col gap-2'
              >
                <Skeleton className='h-5 w-64 rounded-md' />
                <Skeleton className='h-5 w-48 rounded-md' />
              </div>
            )
          )}
          <section
            aria-label='执行记录'
            className='flex min-w-0 flex-col gap-3'
          >
            <h3 className='subsection-title'>执行记录</h3>
            <TaskHistoryPanel task={ref} showErrors={!detail.isError} />
          </section>
        </>
      )}
    </FormSheet>
  )
}

/** Object link · current operation · note · last update, then the failure
 * reason on its own fixed line. */
function TaskHeader({ task }: { task: Task }) {
  const target = taskTarget(task)
  const unsettled = task.status !== 'completed'
  const error = unsettled && task.error
  return (
    <div className='flex min-w-0 flex-col gap-1 text-sm'>
      <p className='m-0 flex min-w-0 items-center gap-1.5 overflow-x-auto whitespace-nowrap text-muted'>
        {target ? (
          <Link href={target} className='min-w-0 truncate text-sm'>
            {task.name}
          </Link>
        ) : (
          <span className='min-w-0 truncate text-foreground'>{task.name}</span>
        )}
        <span aria-hidden>·</span>
        <span className='shrink-0'>{operationLabel(task.operation)}</span>
        <span aria-hidden>·</span>
        <RelativeTime value={task.updated_at} className='shrink-0' />
        <TaskNoteSlot
          task={task}
          className='ms-auto shrink-0 truncate text-xs leading-5'
        />
      </p>
      <p
        className='m-0 h-5 truncate text-danger'
        title={error || undefined}
        aria-hidden={!error}
      >
        {error && `失败原因：${error}`}
      </p>
    </div>
  )
}
