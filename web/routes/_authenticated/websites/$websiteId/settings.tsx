import { z } from 'zod'
import { createFileRoute } from '@tanstack/react-router'
import { optionalSearchValue } from '@/lib/list-search'
import { websiteSectionIds } from '@/features/websites/website-sections'
import { WebsiteSettingsPage } from '@/features/websites/website-settings'

export const Route = createFileRoute(
  '/_authenticated/websites/$websiteId/settings'
)({
  validateSearch: z.object({
    section: optionalSearchValue(z.enum(websiteSectionIds)),
  }),
  staticData: { breadcrumb: '配置' },
  component: WebsiteSettingsPage,
})
