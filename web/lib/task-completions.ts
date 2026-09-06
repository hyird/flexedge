import type { Task } from '@/lib/types'

export type TaskSnapshot = Pick<Task, 'status' | 'version'>

export function snapshotTasks(tasks: Task[]): Map<string, TaskSnapshot> {
  return new Map(
    tasks.map((task) => [
      task.id,
      { status: task.status, version: task.version },
    ])
  )
}

export function completedTasksSince(
  previous: ReadonlyMap<string, TaskSnapshot>,
  tasks: Task[]
): Task[] {
  return tasks.filter((task) => {
    if (task.status !== 'completed') return false

    const previousTask = previous.get(task.id)
    return !previousTask ||
      previousTask.status !== 'completed' ||
      previousTask.version !== task.version
  })
}
