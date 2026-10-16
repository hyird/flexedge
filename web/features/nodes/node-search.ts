import { z } from 'zod'
import {
  listSearchSchema,
  listSheetSearch,
  optionalSearchValue,
} from '@/lib/list-search'
import { statusIds } from '@/lib/status'
import { connectionStatuses, nodeStatuses } from './node-display'

/** Node list search and filters: the `/nodes` URL, or component state on the
 * cluster detail nodes tab. */
const nodeListSearchSchema = listSearchSchema.extend({
  cluster_id: optionalSearchValue(z.string().uuid()),
  status: optionalSearchValue(z.enum(statusIds(nodeStatuses))),
  connection_status: optionalSearchValue(z.enum(statusIds(connectionStatuses))),
})
export type NodeListSearch = z.infer<typeof nodeListSearchSchema>
export const nodeFilterKeys = [
  'cluster_id',
  'status',
  'connection_status',
] as const

/** `/nodes` search params: the list's plus the add-node sheet. */
export const nodesSearchSchema = nodeListSearchSchema.extend({
  ...listSheetSearch,
})
