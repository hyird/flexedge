import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'
import { taskSchema } from './types'

// The router parses numeric-looking values as numbers, so ids are coerced.
const idValue = z.coerce.string().trim().min(1).max(200)

/** `/tasks` URL state: list filters plus the open detail sheet (`task` +
 * `resource_id`, optional `version`). The sheet's own tab is component state. */
export const tasksSearchSchema = listSearchSchema.extend({
  type: optionalSearchValue(taskSchema.shape.resource_type),
  status: optionalSearchValue(taskSchema.shape.status),
  days: optionalSearchValue(z.union([z.literal(1), z.literal(7)])),
  task: optionalSearchValue(idValue),
  resource_id: optionalSearchValue(idValue),
  version: optionalSearchValue(z.coerce.number().int().min(1)),
})
export type TasksSearch = z.infer<typeof tasksSearchSchema>

export const taskFilterKeys = ['type', 'status', 'days'] as const
