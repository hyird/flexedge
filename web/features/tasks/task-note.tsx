import { cn } from '@/lib/utils'
import { taskNote } from './task-display'
import { RetryCountdown } from './task-history-panel'
import type { Task } from './types'

/** The task's fixed note slot next to its status: retry countdown, 节点离线,
 * the warning, or 「失败 N 次后成功」. Empty but reserved when nothing
 * applies, so status changes never move the row. */
export function TaskNoteSlot({
  task,
  className,
}: {
  task: Task
  className: string
}) {
  const note = taskNote(task)
  if (!note) return <span aria-hidden className={className} />
  switch (note.kind) {
    case 'offline':
      return <span className={cn(className, 'text-danger')}>节点离线</span>
    case 'retry':
      return (
        <RetryCountdown
          value={note.at}
          note
          className={cn(className, 'text-warning tabular-nums')}
        />
      )
    case 'warning':
      return (
        <span className={cn(className, 'text-warning')} title={note.label}>
          {note.label}
        </span>
      )
    case 'recovered':
      return (
        <span className={cn(className, 'text-muted')}>
          失败 {note.failures} 次后成功
        </span>
      )
  }
}
