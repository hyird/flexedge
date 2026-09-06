import { useEffect, useRef } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { toast } from 'sonner'
import { getData } from '@/lib/api'
import {
  completedTasksSince,
  snapshotTasks,
  type TaskSnapshot,
} from '@/lib/task-completions'
import type { Task, TaskPage } from '@/lib/types'

const taskRefreshKeys: Record<Task['resource_type'], readonly string[][]> = {
  provider: [['providers'], ['dns-zones']],
  dns_zone: [['dns-zones'], ['clusters'], ['nodes']],
  certificate: [['certificates'], ['websites']],
  website: [['websites']],
}

function taskDescription(task: Task) {
  return `${task.resource_name} · ${task.operation}`
}

export function TaskCompletionMonitor() {
  const queryClient = useQueryClient()
  const snapshots = useRef<Map<string, TaskSnapshot>>(new Map())
  const ready = useRef(false)
  const query = useQuery({
    queryKey: ['task-completion-monitor'],
    queryFn: () => getData<TaskPage>('/tasks/', { page: 1, page_size: 100 }),
    refetchInterval: 2_000,
    refetchIntervalInBackground: true,
  })

  useEffect(() => {
    const tasks = query.data?.list
    if (!tasks) return

    if (!ready.current) {
      snapshots.current = snapshotTasks(tasks)
      ready.current = true
      return
    }

    const completed = completedTasksSince(snapshots.current, tasks)
    snapshots.current = snapshotTasks(tasks)
    if (!completed.length) return

    void queryClient.invalidateQueries({ queryKey: ['tasks'] })
    void queryClient.invalidateQueries({ queryKey: ['overview'] })
    for (const task of completed) {
      for (const queryKey of taskRefreshKeys[task.resource_type]) {
        void queryClient.invalidateQueries({ queryKey })
      }
      toast.success(`${taskDescription(task)} 已完成，相关数据已刷新`)
    }
  }, [query.data?.list, queryClient])

  return null
}
