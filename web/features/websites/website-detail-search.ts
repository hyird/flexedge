import { z } from 'zod'
import { optionalSearchValue } from '@/lib/list-search'
import { accessLogsSearchSchema } from './access-log-search'
import { websiteSectionIds } from './website-sections'

export const websiteDetailTabs = [
  'overview',
  'traffic',
  'logs',
  'settings',
] as const
export type WebsiteDetailTab = (typeof websiteDetailTabs)[number]

/** `/websites/$websiteId` search: `tab` selects the in-page tab (overview is
 * the default and never written), the access-log params belong to the logs
 * tab and `section` to the settings tab. */
export const websiteDetailSearchSchema = accessLogsSearchSchema.extend({
  tab: optionalSearchValue(z.enum(['traffic', 'logs', 'settings'])),
  section: optionalSearchValue(z.enum(websiteSectionIds)),
})
export type WebsiteDetailSearch = z.infer<typeof websiteDetailSearchSchema>

/** Params owned by one tab; cleared when that tab is left. */
export const websiteTabSearchKeys: Record<
  WebsiteDetailTab,
  readonly (keyof WebsiteDetailSearch)[]
> = {
  overview: [],
  traffic: [],
  logs: ['q', 'page', 'page_size', 'mode', 'method', 'status_class', 'log'],
  settings: ['section'],
}
