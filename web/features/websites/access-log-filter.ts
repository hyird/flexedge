import type { StatusDescriptor } from '@/lib/status'
import type { AccessLog } from './access-log-schema'

type AccessLogFilter = {
  keyword?: string
  method?: string
  status_class?: string
}

/** Live-mode filtering mirrors the history endpoint's `keyword`, `method` and
 * `status_class` parameters over the tail buffer kept in the browser. */
export function filterAccessLogs(
  logs: readonly AccessLog[],
  filter: AccessLogFilter
) {
  const keyword = filter.keyword?.trim().toLowerCase()
  return logs.filter(
    (log) =>
      (!filter.method || log.method === filter.method) &&
      (!filter.status_class ||
        String(log.status_code)[0] === filter.status_class[0]) &&
      (!keyword ||
        [
          log.host,
          log.target,
          log.node_name,
          log.client_ip,
          log.client_ip_location,
          log.protocol,
          String(log.status_code),
        ]
          .join(' ')
          .toLowerCase()
          .includes(keyword))
  )
}

/** Status chip of a response: the code, toned by its class. */
export function accessLogStatus(code: number): StatusDescriptor {
  const label = String(code)
  if (code >= 500) return { label, tone: 'danger' }
  if (code >= 400) return { label, tone: 'warning' }
  if (code >= 300) return { label, tone: 'accent' }
  return { label, tone: 'success' }
}
