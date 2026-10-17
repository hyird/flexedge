import type { WebsiteDashboard } from './dashboard-schema'

/** Empty and pre-instrumentation windows have no observed hit rate. */
export function cacheHitRates(
  statistics: Pick<
    WebsiteDashboard['statistics'],
    | 'cache_known_requests'
    | 'cache_known_bytes'
    | 'cache_hit_requests'
    | 'cache_hit_bytes'
  >
) {
  return {
    requests:
      statistics.cache_known_requests > 0
        ? statistics.cache_hit_requests / statistics.cache_known_requests
        : undefined,
    bytes:
      statistics.cache_known_bytes > 0
        ? statistics.cache_hit_bytes / statistics.cache_known_bytes
        : undefined,
  }
}

/** The API's final daily bucket is the current console day (UTC+8).
 * Ranking lists are capped and must not be used as request totals. */
export function todayTrafficStatistics(
  dashboard: Pick<WebsiteDashboard, 'daily' | 'summary'>
) {
  const requests = dashboard.daily.at(-1)?.request_count
  return {
    requests,
    requestsPerIp:
      requests !== undefined && dashboard.summary.today_unique_ips > 0
        ? requests / dashboard.summary.today_unique_ips
        : undefined,
  }
}
