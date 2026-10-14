import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'

/** `/providers` URL state. `tab` omitted = DNS 账号; the list params
 * (q/page/page_size) and `create=1` (opens the add sheet) belong to the
 * current tab and are cleared when switching. */
export const providersSearchSchema = listSearchSchema.extend({
  tab: optionalSearchValue(z.enum(['certificate'])),
  create: optionalSearchValue(z.literal(1)),
})
