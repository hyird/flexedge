import { z } from 'zod'
import {
  listSearchSchema,
  listSheetSearch,
  optionalSearchValue,
} from '@/lib/list-search'
import { enablementStatuses, statusIds } from '@/lib/status'

/** `/clusters` search params: `create=1` opens the create sheet. */
export const clustersSearchSchema = listSearchSchema.extend({
  status: optionalSearchValue(z.enum(statusIds(enablementStatuses))),
  dns_zone_id: optionalSearchValue(z.string().uuid()),
  ...listSheetSearch,
})
export type ClustersSearch = z.infer<typeof clustersSearchSchema>
export const clusterFilterKeys = ['status', 'dns_zone_id'] as const
