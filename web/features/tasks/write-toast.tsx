import { Link, toast } from '@heroui/react'
import { operationHref } from './task-display'

/** `operation_id` of a write response's `data` (contract §6): present only
 * when the write started background work, i.e. created a task. */
export function operationIdOf(data: unknown) {
  if (typeof data !== 'object' || data === null || !('operation_id' in data))
    return undefined
  const id = data.operation_id
  return typeof id === 'string' && id ? id : undefined
}

/** Success toast of a write. When the write created a task (`data` carries
 * `operation_id`) the toast offers 「查看任务」, which opens that task's
 * detail sheet in the task center. */
export function toastWriteSuccess(message: string, data?: unknown) {
  const operationId = operationIdOf(data)
  if (!operationId) {
    toast.success(message)
    return
  }
  const key = toast.success(message, {
    description: (
      <Link
        href={operationHref(operationId)}
        onPress={() => toast.close(key)}
        className='text-sm'
      >
        查看任务
      </Link>
    ),
  })
}
