import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'
import {
  operationActions,
  operationActorLabels,
  operationStatusGroups,
} from './task-display'
import { operationStatusValues, operationTargetTypes } from './types'

const keysOf = <T extends object>(map: T) =>
  Object.keys(map) as [keyof T & string, ...(keyof T & string)[]]

/** `/tasks` URL state: list filters (contract §2.1) plus the open detail
 * sheet (`task` = operation id). The sheet's expanded subtask is component
 * state. */
export const tasksSearchSchema = listSearchSchema.extend({
  status: optionalSearchValue(
    z.enum([...keysOf(operationStatusGroups), ...operationStatusValues])
  ),
  action: optionalSearchValue(z.enum(keysOf(operationActions))),
  target_type: optionalSearchValue(z.enum(operationTargetTypes)),
  actor: optionalSearchValue(z.enum(keysOf(operationActorLabels))),
  days: optionalSearchValue(z.union([z.literal(1), z.literal(7)])),
  task: optionalSearchValue(z.coerce.string().trim().min(1).max(200)),
})
export type TasksSearch = z.infer<typeof tasksSearchSchema>

export const taskFilterKeys = [
  'status',
  'action',
  'target_type',
  'actor',
  'days',
] as const
