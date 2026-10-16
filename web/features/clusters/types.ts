import { z } from 'zod'
import { revisionedSchema } from '@/lib/resource'
import { enablementStatuses, statusIds } from '@/lib/status'

/** `cluster_dto` (service/domains/cluster/cluster_read_service.h): every
 * field is written with a non-null fallback. */
export const clusterSchema = revisionedSchema.extend({
  name: z.string(),
  dns_zone_id: z.string(),
  dns_zone_domain: z.string(),
  /** DNS platform code (`cloudflare` / `aliyun`), despite the wire name. */
  dns_provider_name: z.string(),
  hostname_prefix: z.string(),
  access_domain: z.string(),
  node_count: z.number().int(),
  online_node_count: z.number().int(),
  // Writes are validated to enabled/disabled (cluster_schema.h).
  status: z.enum(statusIds(enablementStatuses)),
})
export type Cluster = z.infer<typeof clusterSchema>
