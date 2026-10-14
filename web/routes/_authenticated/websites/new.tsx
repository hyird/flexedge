import { z } from 'zod'
import { createFileRoute } from '@tanstack/react-router'
import { optionalSearchValue } from '@/lib/list-search'
import { websiteSectionIds } from '@/features/websites/website-sections'
import { WebsiteCreatePage } from '@/features/websites/website-settings'

export const Route = createFileRoute('/_authenticated/websites/new')({
  validateSearch: z.object({
    section: optionalSearchValue(z.enum(websiteSectionIds)),
  }),
  staticData: { breadcrumb: '创建网站' },
  component: WebsiteCreatePage,
})
