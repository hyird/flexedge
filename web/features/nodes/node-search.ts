import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'

/** Node list search and filters: the `/nodes` URL, or component state on the
 * cluster detail nodes tab. */
const nodeListSearchSchema = listSearchSchema.extend({
  cluster_id: optionalSearchValue(z.string().uuid()),
  status: optionalSearchValue(z.enum(['enabled', 'disabled'])),
  connection_status: optionalSearchValue(
    z.enum(['unregistered', 'online', 'offline'])
  ),
})
export type NodeListSearch = z.infer<typeof nodeListSearchSchema>
export const nodeFilterKeys = [
  'cluster_id',
  'status',
  'connection_status',
] as const

/** `/nodes` search params: the list's plus `create=1` (add-node sheet). */
export const nodesSearchSchema = nodeListSearchSchema.extend({
  create: optionalSearchValue(z.literal(1)),
})
