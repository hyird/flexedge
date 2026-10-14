import { createFileRoute } from '@tanstack/react-router'
import { WebsiteDetailLayout } from '@/features/websites/website-detail'
import { websiteDetailSearchSchema } from '@/features/websites/website-detail-search'

export const Route = createFileRoute('/_authenticated/websites/$websiteId')({
  validateSearch: websiteDetailSearchSchema,
  staticData: { breadcrumb: '网站详情' },
  component: WebsiteDetailLayout,
})
