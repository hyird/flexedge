import { createFileRoute } from '@tanstack/react-router'
import { WebsiteTraffic } from '@/features/websites/website-traffic'

export const Route = createFileRoute(
  '/_authenticated/websites/$websiteId/traffic'
)({
  staticData: { breadcrumb: '流量分析' },
  component: WebsiteTraffic,
})
