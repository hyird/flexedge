import { z } from 'zod'
import { queryOptions } from '@tanstack/react-query'
import { streamPath } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'

const count = z.number().int().nonnegative()
const overviewSchema = z.object({
  resources: z.object({
    website_count: count,
    domain_count: count,
    certificate_count: count,
    cluster_count: count,
  }),
  issues: z.object({
    dns_zone_issue_count: count,
    certificate_expiring_count: count,
    certificate_failed_count: count,
    active_marker_count: count,
    retry_marker_count: count,
  }),
  recent_markers: z.array(
    z.object({
      id: z.string(),
      resource_type: z.string(),
      resource_id: z.string(),
      resource_name: z.string(),
      operation: z.string(),
      status: z.string(),
      last_error: z.string().optional(),
      updated_at: z.string(),
    })
  ),
})
export const overviewQuery = queryOptions({
  queryKey: ['overview'],
  retry: false,
  queryFn: ({ queryKey, signal }) =>
    readLiveQuery(streamPath('/overview'), queryKey, signal, (value) =>
      overviewSchema.parse(value)
    ),
})
