import { Link, toast } from '@heroui/react'
import { taskHref } from './task-display'
import type { TaskRef } from './types'

/** Success toast of a write. A write to a resource that has its own task
 * (website, DNS zone, certificate, provider, node) passes that resource, and
 * the toast offers 「查看任务」, which opens the task's detail sheet in the
 * task center. */
export function toastWriteSuccess(message: string, task?: TaskRef) {
  if (!task) {
    toast.success(message)
    return
  }
  const key = toast.success(message, {
    description: (
      <Link
        href={taskHref(task)}
        onPress={() => toast.close(key)}
        className='text-sm'
      >
        查看任务
      </Link>
    ),
  })
}
