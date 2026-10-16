import { listSearchSchema, listSheetSearch } from '@/lib/list-search'

/** `/providers/dns` and `/providers/certificate` search params: `q`, paging,
 * and `create=1` / `edit=<id>` for the add and edit sheets. */
export const providersSearchSchema = listSearchSchema.extend({
  ...listSheetSearch,
})
