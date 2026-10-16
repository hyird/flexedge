import { z } from 'zod'
import { revisionedSchema } from '@/lib/resource'
import { enablementStatuses, statusIds } from '@/lib/status'
import { deployStatuses, originHealthStatuses } from './website-display'

// Config enums are validated on every write
// (service/features/website_config/validation.h, common/route_*.h), so a
// stored website only carries these values; the editor must round-trip them.
// Runtime values with a closed producer (deploy status from the runtime
// mapper, origin health accepted by service/features/node_channel/protocol.h)
// are enums too; the domain resolution status is persisted probe output and
// stays a string shown through its dictionary.

const enablementSchema = z.enum(statusIds(enablementStatuses))

/** `website_domain_output` (service/features/website_config/transport.h). */
const websiteDomainSchema = z.object({
  id: z.string(),
  hostname: z.string(),
  dns_mode: z.enum(['managed', 'external']),
})
export type WebsiteDomain = z.infer<typeof websiteDomainSchema>

/** `website_origin_output`. */
const websiteOriginSchema = z.object({
  id: z.string(),
  group: z.string(),
  protocol: z.enum(['http', 'https']),
  host: z.string(),
  port: z.number().int(),
  role: z.enum(['primary', 'backup']),
  weight: z.number().int(),
  status: enablementSchema,
})
export type WebsiteOrigin = z.infer<typeof websiteOriginSchema>

/** `website_route_header_output`. */
const websiteRouteHeaderSchema = z.object({
  name: z.string(),
  value: z.string(),
})

/** `website_route_condition_output`; values per `route_policy::condition_options`. */
const websiteRouteConditionSchema = z.object({
  source: z.enum(['header', 'query']),
  name: z.string(),
  op: z.enum(['equals', 'not_equals', 'exists', 'absent']),
  value: z.string(),
})
export type WebsiteRouteCondition = z.infer<typeof websiteRouteConditionSchema>

/** Route methods (`flexedge::node::route_method`, node/proto/website_validation.h). */
export const routeMethods = [
  'GET',
  'HEAD',
  'POST',
  'PUT',
  'PATCH',
  'DELETE',
  'OPTIONS',
] as const

/** `website_route_rule_output`. `rewrite_mode`/`query_mode` keep a stored
 * empty value (`route_policy::rewrite_mode`/`query_mode` accept it): an empty
 * rewrite mode means `replace_path` with a rewrite path, else `none`; an
 * empty query mode means `preserve`. */
const websiteRouteRuleSchema = z.object({
  id: z.string(),
  name: z.string(),
  description: z.string(),
  hostnames: z.array(z.string()),
  conditions: z.array(websiteRouteConditionSchema),
  rewrite_mode: z.enum([
    '',
    'none',
    'replace_path',
    'strip_prefix',
    'replace_prefix',
  ]),
  query_mode: z.enum(['', 'preserve', 'drop', 'replace']),
  query_string: z.string(),
  status: enablementSchema,
  match_type: z.enum(['exact', 'prefix', 'suffix', 'regex']),
  path: z.string(),
  methods: z.array(z.enum(routeMethods)),
  action: z.enum(['proxy', 'redirect', 'rewrite']),
  rewrite_path: z.string(),
  redirect_url: z.string(),
  redirect_status: z.number().int(),
  origin_group: z.string(),
  request_headers: z.array(websiteRouteHeaderSchema),
  response_headers: z.array(websiteRouteHeaderSchema),
})
export type WebsiteRouteRule = z.infer<typeof websiteRouteRuleSchema>

/** `website_config_output`; `name` is omitted when never set. */
const websiteConfigSchema = z.object({
  name: z.string().optional(),
  domains: z.array(websiteDomainSchema),
  origins: z.array(websiteOriginSchema),
  default_origin_group: z.string(),
  origin_host_header: z.string(),
  origin_connect_timeout_seconds: z.number().int(),
  origin_read_timeout_seconds: z.number().int(),
  pass_client_ip: z.boolean(),
  health_check_enabled: z.boolean(),
  health_check_path: z.string(),
  health_check_interval_seconds: z.number().int(),
  health_check_timeout_seconds: z.number().int(),
  health_check_expected_status: z.number().int(),
  healthy_threshold: z.number().int(),
  unhealthy_threshold: z.number().int(),
  access_log_enabled: z.boolean(),
  access_log_request_headers: z.boolean(),
  access_log_request_body: z.boolean(),
  access_log_response_headers: z.boolean(),
  access_log_query_params: z.boolean(),
  access_log_cookies: z.boolean(),
  access_log_referer: z.boolean(),
  access_log_user_agent: z.boolean(),
  access_log_status_code_ranges: z.array(
    z.enum(['1xx', '2xx', '3xx', '4xx', '5xx'])
  ),
  access_log_client_abort: z.boolean(),
  https_enabled: z.boolean(),
  certificate_ids: z.array(z.string()),
  minimum_tls_version: z.enum(['1.2', '1.3']),
  force_https: z.boolean(),
  hsts_enabled: z.boolean(),
  response_compression_enabled: z.boolean(),
  response_compression_min_bytes: z.number().int(),
  response_compression_max_bytes: z.number().int(),
  response_compression_algorithms: z.array(z.enum(['br', 'zstd', 'gzip'])),
  response_compression_mime_types: z.array(z.string()),
  response_compression_extensions: z.array(z.string()),
  response_compression_excluded_extensions: z.array(z.string()),
  route_rules: z.array(websiteRouteRuleSchema),
})
export type WebsiteConfig = z.infer<typeof websiteConfigSchema>

/** `website_origin_runtime_dto` (service/domains/website/website_types.h). */
const websiteOriginStateSchema = z.object({
  node_id: z.string(),
  node_name: z.string(),
  origin_id: z.string(),
  status: z.enum(statusIds(originHealthStatuses)),
  checked_at_unix_millis: z.number().int(),
  latency_millis: z.number().int(),
  last_error: z.string().optional(),
})

/** `website_runtime_dto`; `deploy_status` is derived by
 * `website_runtime_mapper.h` from the node counts. */
const websiteRuntimeSchema = z.object({
  origin_sources: z
    .array(
      z.object({
        node_id: z.string(),
        node_revision: z.number().int(),
        reported_at: z.string(),
      })
    )
    .optional(),
  domain_states: z.array(
    z.object({
      id: z.string(),
      access_protocol: z.enum(['http', 'https']),
      resolution_status: z.string(),
      last_verified_at: z.string().optional(),
      last_error: z.string().optional(),
    })
  ),
  origin_states: z.array(websiteOriginStateSchema),
  deploy_status: z.enum(statusIds(deployStatuses)),
  target_node_count: z.number().int(),
  synced_node_count: z.number().int(),
})

/** `website_dto`. */
export const websiteSchema = revisionedSchema.extend({
  cluster_id: z.string(),
  cluster_name: z.string(),
  access_domain: z.string(),
  status: enablementSchema,
  config: websiteConfigSchema,
  certificates: z.array(
    z.object({
      id: z.string(),
      domains: z.array(z.string()),
      usable: z.boolean(),
    })
  ),
  runtime: websiteRuntimeSchema,
})
export type Website = z.infer<typeof websiteSchema>

/** `origin_runtime_patch` (service/features/node_channel/runtime_mapper.h):
 * one node's origin health for one website, the `origin-runtime` event. */
export const websiteOriginRuntimePatchSchema = z.object({
  id: z.string(),
  node_id: z.string(),
  node_revision: z.number().int(),
  reported_at: z.string(),
  origin_states: z.array(websiteOriginStateSchema),
})
