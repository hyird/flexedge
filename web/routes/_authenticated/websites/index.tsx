import { createFileRoute } from '@tanstack/react-router'
import { WebsitesPage } from '@/features/websites/websites-page'
import { websitesSearchSchema } from '@/features/websites/websites-search'

export const Route = createFileRoute('/_authenticated/websites/')({
  validateSearch: websitesSearchSchema,
  staticData: { breadcrumb: '网站' },
  component: WebsitesPage,
})
