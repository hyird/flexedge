import { z } from 'zod'
import {
  listSearchSchema,
  listSheetSearch,
  optionalSearchValue,
} from '@/lib/list-search'

export const cacheSearchSchema = listSearchSchema.extend({
  status: optionalSearchValue(z.enum(['enabled', 'disabled'])),
  ...listSheetSearch,
})
