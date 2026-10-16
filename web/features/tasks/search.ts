import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'
import { parseTaskKey, taskStatusGroups } from './task-display'
import { taskResourceTypes, taskStatusValues } from './types'

/** `/tasks` URL state: list filters plus the open detail sheet (`task` =
 * `<resource_type>:<resource_id>`). */
export const tasksSearchSchema = listSearchSchema.extend({
  status: optionalSearchValue(
    z.enum([
      ...(Object.keys(taskStatusGroups) as (keyof typeof taskStatusGroups)[]),
      ...taskStatusValues,
    ])
  ),
  type: optionalSearchValue(z.enum(taskResourceTypes)),
  task: optionalSearchValue(
    z.coerce
      .string()
      .trim()
      .max(200)
      .refine((value) => parseTaskKey(value) !== undefined)
  ),
})
export type TasksSearch = z.infer<typeof tasksSearchSchema>

export const taskFilterKeys = ['status', 'type'] as const
