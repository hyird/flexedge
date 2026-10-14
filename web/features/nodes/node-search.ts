import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'

/** `/nodes` and `/clusters/$clusterId` (nodes tab) search params. `create=1`
 * opens the add-node sheet. */
export const nodesSearchSchema = listSearchSchema.extend({
  cluster_id: optionalSearchValue(z.string().uuid()),
  status: optionalSearchValue(z.enum(['enabled', 'disabled'])),
  connection_status: optionalSearchValue(
    z.enum(['unregistered', 'online', 'offline'])
  ),
  create: optionalSearchValue(z.literal(1)),
})
export type NodesSearch = z.infer<typeof nodesSearchSchema>
export const nodeFilterKeys = [
  'cluster_id',
  'status',
  'connection_status',
] as const
