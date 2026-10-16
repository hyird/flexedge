import { cn } from '@/lib/utils'
import { taskNote } from './task-display'
import { RetryCountdown } from './task-history-panel'
import type { Task } from './types'

/** The task's fixed note slot next to its status: retry countdown, 节点离线.
 * Empty but reserved when nothing
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
  }
}
