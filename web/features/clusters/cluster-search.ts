import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'
import { nodesSearchSchema } from '@/features/nodes/node-search'
import { websitesSearchSchema } from '@/features/websites/websites-search'

/** `/clusters` search params: `create=1` opens the create sheet and
 * `edit=<id>` the edit sheet. */
export const clustersSearchSchema = listSearchSchema.extend({
  status: optionalSearchValue(z.enum(['enabled', 'disabled'])),
  dns_zone_id: optionalSearchValue(z.string().uuid()),
  create: optionalSearchValue(z.literal(1)),
  edit: optionalSearchValue(z.string().uuid()),
})
export type ClustersSearch = z.infer<typeof clustersSearchSchema>
export const clusterFilterKeys = ['status', 'dns_zone_id'] as const

/** `/clusters/$clusterId` search params: the in-page tab (`?tab=websites`;
 * the node list is the default tab) plus the embedded list's params. Both
 * lists share `q`/`page`/`page_size` and the same `status` (enabled/disabled),
 * so one key serves whichever tab is open; `connection_status` and `create=1`
 * (add-node sheet) belong to the node tab. `cluster_id` is accepted only
 * because the shared list shapes declare it — the path fixes the cluster.
 * Switching tabs clears every list param. */
export const clusterDetailSearchSchema = z.object({
  ...nodesSearchSchema.shape,
  ...websitesSearchSchema.shape,
  tab: optionalSearchValue(z.enum(['websites'])),
})
type ClusterDetailSearch = z.infer<typeof clusterDetailSearchSchema>
export type ClusterDetailTab = NonNullable<ClusterDetailSearch['tab']>
