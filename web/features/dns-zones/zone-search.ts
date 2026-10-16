import { z } from 'zod'
import {
  listSearchSchema,
  listSheetSearch,
  optionalSearchValue,
} from '@/lib/list-search'
import { zoneSyncFilterIds } from './dns-zone-display'

/** Search params of the zones list (`/dns-zones`). */
export const zoneSearchSchema = listSearchSchema.extend({
  dns_provider_id: optionalSearchValue(z.string().uuid()),
  sync_status: optionalSearchValue(z.enum(zoneSyncFilterIds)),
  ...listSheetSearch,
})
