import { z } from 'zod'
import { listSearchSchema, optionalSearchValue } from '@/lib/list-search'

/** `/websites` and the cluster detail “网站” tab share this search shape. */
export const websitesSearchSchema = listSearchSchema.extend({
  cluster_id: optionalSearchValue(z.string().uuid()),
  status: optionalSearchValue(z.enum(['enabled', 'disabled'])),
})
export type WebsitesSearch = z.infer<typeof websitesSearchSchema>

export const websiteFilterKeys = ['cluster_id', 'status'] as const
