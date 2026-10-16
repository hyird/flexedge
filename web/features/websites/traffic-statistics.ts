import type { WebsiteDashboard } from './dashboard-schema'

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
