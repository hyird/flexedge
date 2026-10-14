import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'

export const accessLogMethods = [
  'GET',
  'HEAD',
  'POST',
  'PUT',
  'PATCH',
  'DELETE',
  'OPTIONS',
] as const
export const accessLogStatusClasses = [
  '1xx',
  '2xx',
  '3xx',
  '4xx',
  '5xx',
] as const

/** Access-log tab search (`?tab=logs`): live mode is the default and is never
 * written to the URL; `log` is the request whose detail sheet is open. */
export const accessLogsSearchSchema = listSearchSchema.extend({
  mode: optionalSearchValue(z.literal('history')),
  method: optionalSearchValue(z.enum(accessLogMethods)),
  status_class: optionalSearchValue(z.enum(accessLogStatusClasses)),
  log: optionalSearchValue(z.string().min(1).max(200)),
})
export type AccessLogsSearch = z.infer<typeof accessLogsSearchSchema>
