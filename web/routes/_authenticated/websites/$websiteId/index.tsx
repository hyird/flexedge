import { createFileRoute } from '@tanstack/react-router'
import { WebsiteOverview } from '@/features/websites/website-overview'

export const Route = createFileRoute('/_authenticated/websites/$websiteId/')({
  component: WebsiteOverview,
})
