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

/** Access-log tab list state (component state, never in the URL): live mode
 * is the default and represented by absence. */
export type AccessLogsSearch = {
  q?: string
  page?: number
  page_size?: number
  mode?: 'history'
  method?: (typeof accessLogMethods)[number]
  status_class?: (typeof accessLogStatusClasses)[number]
}
