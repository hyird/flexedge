import { createContext } from 'react'

/** Subtasks a trace step may link to (`related_task.subtask_id`): those of
 * the open task, expanded in place by `open`. Without a provider steps show
 * no link. */
export const SubtaskLinkContext = createContext<{
  has: (subtaskId: string) => boolean
  open: (subtaskId: string) => void
} | null>(null)
