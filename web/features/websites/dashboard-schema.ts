import { z } from 'zod'
import { parseApiResponse, requireResponseData } from '@/lib/api-response'

const seriesPoint = z.object({
  timestamp: z.string(),
  request_count: z.number().int(),
  response_bytes: z.number().int(),
  bandwidth_bps: z.number().int(),
})
const ranking = z.object({
  label: z.string(),
  request_count: z.number().int(),
  response_bytes: z.number().int(),
})
const clientIpRanking = ranking.extend({
  asn: z.string(),
  as_name: z.string(),
})
export const websiteDashboardSchema = z.object({
  summary: z.object({
    previous_month_peak_bps: z.number().int(),
    current_month_peak_bps: z.number().int(),
    today_peak_bps: z.number().int(),
    current_bandwidth_bps: z.number().int(),
    today_unique_ips: z.number().int(),
    today_response_bytes: z.number().int(),
  }),
  hourly: z.array(seriesPoint),
  daily: z.array(seriesPoint),
  status_codes: z.array(ranking),
  methods: z.array(ranking),
  countries: z.array(ranking),
  hosts: z.array(ranking),
  referers: z.array(ranking),
  paths: z.array(ranking),
  client_ips_by_bytes: z.array(clientIpRanking),
  client_ips_by_requests: z.array(clientIpRanking),
})
export type WebsiteDashboard = z.infer<typeof websiteDashboardSchema>
export type WebsiteDashboardSeriesPoint = z.infer<typeof seriesPoint>
export type WebsiteDashboardRanking = z.infer<typeof ranking>
export type WebsiteDashboardClientIpRanking = z.infer<typeof clientIpRanking>
export function parseWebsiteDashboard(raw: string) {
  return websiteDashboardSchema.parse(
    requireResponseData(parseApiResponse(JSON.parse(raw)))
  )
}
