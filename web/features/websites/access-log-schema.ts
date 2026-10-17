import { z } from 'zod'
import { pageDataSchema } from '@/lib/api'
import { createLogEventParser } from '@/features/logs/log-event'

// service/domains/website/website_types.h: WebsiteAccessLogDto
const accessLogSchema = z.object({
  id: z.string(),
  occurred_at: z.string(),
  node_id: z.string(),
  node_name: z.string(),
  client_ip: z.string().optional(),
  client_ip_location: z.string().optional(),
  client_ip_asn: z.string().optional(),
  client_ip_as_name: z.string().optional(),
  protocol: z.string(),
  method: z.string(),
  host: z.string(),
  target: z.string(),
  status_code: z.number().int(),
  cache_status: z.string().optional(),
  request_bytes: z.number().int(),
  response_bytes: z.number().int(),
  duration_ms: z.number().int(),
  user_agent: z.string().optional(),
  referer: z.string().optional(),
  request_headers: z.string().optional(),
  request_body: z.string().optional(),
  request_body_truncated: z.boolean(),
  response_headers: z.string().optional(),
  query_string: z.string().optional(),
  cookies: z.string().optional(),
  tls_fingerprint: z.string().optional(),
})
export type AccessLog = z.infer<typeof accessLogSchema>
export const parseAccessLogs = createLogEventParser(accessLogSchema)
export const accessLogPageSchema = pageDataSchema(accessLogSchema)
