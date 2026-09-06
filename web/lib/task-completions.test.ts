import { expect, test } from 'vitest'
import {
  completedTasksSince,
  snapshotTasks,
} from '@/lib/task-completions'
import type { Task } from '@/lib/types'

const task = (overrides: Partial<Task> = {}): Task => ({
  id: 'task-1',
  sequence: 1,
  kind: 'dns',
  resource_type: 'dns_zone',
  resource_id: 'zone-1',
  resource_name: 'a-z.xin',
  operation: 'sync',
  status: 'pending',
  version: 1,
  processed_version: 0,
  count_fails: 0,
  next_attempt_at: '2026-09-05T20:00:00+08:00',
  created_at: '2026-09-05T20:00:00+08:00',
  updated_at: '2026-09-05T20:00:00+08:00',
  ...overrides,
})

test('task completion monitor detects a task that transitions to completed', () => {
  const previous = snapshotTasks([task()])
  const completed = completedTasksSince(
    previous,
    [task({ status: 'completed', processed_version: 1 })]
  )

  expect(completed).toHaveLength(1)
})

test('task completion monitor does not notify twice for the same completion', () => {
  const completedTask = task({ status: 'completed', processed_version: 1 })

  expect(completedTasksSince(snapshotTasks([completedTask]), [completedTask])).toHaveLength(0)
})

test('task completion monitor notifies when a newer task version finishes', () => {
  const previous = snapshotTasks([
    task({ status: 'completed', version: 1, processed_version: 1 }),
  ])
  const completed = completedTasksSince(
    previous,
    [task({ status: 'completed', version: 2, processed_version: 2 })]
  )

  expect(completed).toHaveLength(1)
})
