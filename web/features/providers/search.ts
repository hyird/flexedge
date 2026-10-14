import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'

/** `/providers/dns` and `/providers/certificate` URL state; `create=1` opens
 * the add sheet. */
export const providersSearchSchema = listSearchSchema.extend({
  create: optionalSearchValue(z.literal(1)),
})
