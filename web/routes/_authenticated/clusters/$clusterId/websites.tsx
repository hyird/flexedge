import { createFileRoute } from '@tanstack/react-router'
import { ClusterWebsites } from '@/features/clusters/cluster-websites'
import { websitesSearchSchema } from '@/features/websites/websites-search'

export const Route = createFileRoute(
  '/_authenticated/clusters/$clusterId/websites'
)({
  validateSearch: websitesSearchSchema,
  staticData: { breadcrumb: '网站' },
  component: ClusterWebsites,
})
