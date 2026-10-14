import { createFileRoute } from '@tanstack/react-router'
import { WebsiteDetailLayout } from '@/features/websites/website-detail'

export const Route = createFileRoute('/_authenticated/websites/$websiteId')({
  staticData: { breadcrumb: '网站详情' },
  component: WebsiteDetailLayout,
})
