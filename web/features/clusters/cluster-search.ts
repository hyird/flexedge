import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'

/** `/clusters` search params: `create=1` opens the create sheet. */
export const clustersSearchSchema = listSearchSchema.extend({
  status: optionalSearchValue(z.enum(['enabled', 'disabled'])),
  dns_zone_id: optionalSearchValue(z.string().uuid()),
  create: optionalSearchValue(z.literal(1)),
})
export type ClustersSearch = z.infer<typeof clustersSearchSchema>
export const clusterFilterKeys = ['status', 'dns_zone_id'] as const
