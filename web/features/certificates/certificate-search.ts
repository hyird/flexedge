import { z } from 'zod'
import {
  listSearchSchema,
  listSheetSearch,
  optionalSearchValue,
} from '@/lib/list-search'
import { statusIds } from '@/lib/status'
import { websitesSearchSchema } from '@/features/websites/websites-search'
import { certificateStatuses } from './certificate-display'

/** `/certificates` search params: `create=1` opens the request sheet. */
export const certificatesSearchSchema = listSearchSchema.extend({
  status: optionalSearchValue(z.enum(statusIds(certificateStatuses))),
  expiring: optionalSearchValue(z.literal(1)),
  abnormal: optionalSearchValue(z.literal(1)),
  ...listSheetSearch,
})
export const certificateFilterKeys = ['status', 'expiring', 'abnormal'] as const

/** `/certificates/:id` search params: the referencing-websites table keeps
 * its search, filters and page in the URL; `renewal=1` opens the renewal
 * settings sheet. */
export const certificateDetailSearchSchema = websitesSearchSchema.extend({
  renewal: optionalSearchValue(z.literal(1)),
})
